#pragma once

#include "core/Core.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Core {

    /**
     * Depth-first search for a node by its archive-relative full_path.
     */
    inline const FileNode* FindNodeByPath(const FileNode& current, std::string_view path) {
        if (current.full_path == path) return &current;

        if (const auto* folder = std::get_if<FolderInfo>(&current.data)) {
            for (const auto& child : folder->children) {
                if (const FileNode* found = FindNodeByPath(child, path)) {
                    return found;
                }
            }
        }
        return nullptr;
    }

    /**
     * Reads precomputed subtree statistics in O(1).
     */
    inline void NodeStats(const FileNode& node, uint64_t& out_bytes, uint32_t& out_files) noexcept {
        if (const auto* file = std::get_if<FileInfo>(&node.data)) {
            out_bytes += file->size;
            ++out_files;
        } else if (const auto* folder = std::get_if<FolderInfo>(&node.data)) {
            out_bytes += folder->total_bytes;
            out_files += folder->file_count;
        }
    }

    inline uint64_t NodeTotalBytes(const FileNode& node) noexcept {
        uint64_t bytes = 0;
        uint32_t files = 0;
        NodeStats(node, bytes, files);
        return bytes;
    }

    inline uint32_t NodeFileCount(const FileNode& node) noexcept {
        uint64_t bytes = 0;
        uint32_t files = 0;
        NodeStats(node, bytes, files);
        return files;
    }

    /**
     * Hash-indexed O(N) builder for Core::FileNode trees.
     * Eliminates O(N * D) linear scans during archive scanning and precomputes
     * folder byte sizes and file counts in a single post-order pass.
     */
    class FileTreeBuilder {
    public:
        explicit FileTreeBuilder(std::string root_name = "/") {
            Reset(std::move(root_name));
        }

        void Reset(std::string root_name = "/") {
            m_nodes.clear();
            m_file_count = 0;
            m_total_size = 0;

            BuildNode root;
            root.name = std::move(root_name);
            root.full_path = "/";
            root.is_folder = true;
            m_nodes.push_back(std::move(root));
        }

        void AddFile(std::string_view raw_path, uint64_t offset, uint64_t size) {
            const std::string clean_path = NormalizeArchivePath(raw_path);
            if (clean_path.empty()) return;

            const auto parts = SplitPath(clean_path);
            if (parts.empty()) return;

            size_t current_idx = 0;
            std::string current_path;

            for (size_t i = 0; i + 1 < parts.size(); ++i) {
                if (!current_path.empty()) current_path.push_back('/');
                current_path.append(parts[i]);

                if (!m_nodes[current_idx].is_folder) {
                    LogError("Cannot add file to tree: intermediate node is not a folder: " + clean_path);
                    return;
                }

                auto& index_map = m_nodes[current_idx].child_index;
                if (const auto it = index_map.find(parts[i]); it != index_map.end()) {
                    current_idx = it->second;
                } else {
                    const size_t new_idx = m_nodes.size();
                    index_map.emplace(parts[i], new_idx);
                    m_nodes[current_idx].children.push_back(new_idx);

                    BuildNode folder;
                    folder.name = parts[i];
                    folder.full_path = current_path;
                    folder.is_folder = true;
                    m_nodes.push_back(std::move(folder));
                    current_idx = new_idx;
                }
            }

            if (!m_nodes[current_idx].is_folder) {
                LogError("Cannot add file to tree: parent node is not a folder: " + clean_path);
                return;
            }

            const std::string& filename = parts.back();
            auto& index_map = m_nodes[current_idx].child_index;
            if (const auto it = index_map.find(filename); it != index_map.end()) {
                BuildNode& existing = m_nodes[it->second];
                if (!existing.is_folder) {
                    m_total_size -= existing.size;
                    existing.offset = offset;
                    existing.size = size;
                    m_total_size += size;
                }
                return;
            }

            const size_t file_idx = m_nodes.size();
            index_map.emplace(filename, file_idx);
            m_nodes[current_idx].children.push_back(file_idx);

            BuildNode file;
            file.name = filename;
            file.full_path = current_path.empty() ? filename : (current_path + "/" + filename);
            file.is_folder = false;
            file.offset = offset;
            file.size = size;
            file.format = GetExtension(filename);
            m_nodes.push_back(std::move(file));

            ++m_file_count;
            m_total_size += size;
        }

        [[nodiscard]] uint32_t FileCount() const noexcept { return m_file_count; }
        [[nodiscard]] uint64_t TotalSize() const noexcept { return m_total_size; }

        [[nodiscard]] FileNode Build() const {
            return Materialize(0);
        }

    private:
        struct BuildNode {
            std::string name;
            std::string full_path;
            bool is_folder = false;
            uint64_t offset = 0;
            uint64_t size = 0;
            std::string format;
            std::vector<size_t> children;
            std::unordered_map<std::string, size_t> child_index;
        };

        [[nodiscard]] FileNode Materialize(size_t idx) const {
            const BuildNode& src = m_nodes[idx];
            FileNode out;
            out.name = src.name;
            out.full_path = src.full_path;

            if (!src.is_folder) {
                out.data = FileInfo{src.offset, src.size, src.format};
                return out;
            }

            FolderInfo folder;
            folder.children.reserve(src.children.size());
            for (const size_t child_idx : src.children) {
                FileNode child = Materialize(child_idx);
                if (const auto* fi = std::get_if<FileInfo>(&child.data)) {
                    folder.total_bytes += fi->size;
                    ++folder.file_count;
                } else if (const auto* sub = std::get_if<FolderInfo>(&child.data)) {
                    folder.total_bytes += sub->total_bytes;
                    folder.file_count += sub->file_count;
                }
                folder.children.push_back(std::move(child));
            }

            std::ranges::sort(folder.children, [](const FileNode& a, const FileNode& b) {
                const bool a_folder = a.IsFolder();
                const bool b_folder = b.IsFolder();
                if (a_folder != b_folder) return a_folder > b_folder;
                return a.name < b.name;
            });

            out.data = std::move(folder);
            return out;
        }

        std::vector<BuildNode> m_nodes;
        uint32_t m_file_count = 0;
        uint64_t m_total_size = 0;
    };

} // namespace Core
