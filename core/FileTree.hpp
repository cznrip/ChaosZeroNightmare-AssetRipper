#pragma once

#include "core/Core.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace Core {

    /**
     * Depth-first search for a node by its archive-relative full_path.
     */
    inline const FileNode* FindNodeByPath(const FileNode& current, std::string_view path) {
        if (current.full_path == path) return &current;

        if (std::holds_alternative<FolderInfo>(current.data)) {
            for (const auto& child : std::get<FolderInfo>(current.data).children) {
                if (const FileNode* found = FindNodeByPath(child, path)) {
                    return found;
                }
            }
        }
        return nullptr;
    }

    /**
     * Sums the stored sizes and counts the files under a node in a single walk.
     */
    inline void NodeStats(const FileNode& node, uint64_t& out_bytes, uint32_t& out_files) {
        if (std::holds_alternative<FileInfo>(node.data)) {
            out_bytes += std::get<FileInfo>(node.data).size;
            ++out_files;
            return;
        }

        for (const auto& child : std::get<FolderInfo>(node.data).children) {
            NodeStats(child, out_bytes, out_files);
        }
    }

    inline uint64_t NodeTotalBytes(const FileNode& node) {
        uint64_t bytes = 0;
        uint32_t files = 0;
        NodeStats(node, bytes, files);
        return bytes;
    }

    inline uint32_t NodeFileCount(const FileNode& node) {
        uint64_t bytes = 0;
        uint32_t files = 0;
        NodeStats(node, bytes, files);
        return files;
    }

} // namespace Core
