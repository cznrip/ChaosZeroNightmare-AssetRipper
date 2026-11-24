#include "services/FileMap.hpp"
#include "core/AppInfo.hpp"
#include "core/StringUtils.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace Services {

    namespace {

        nlohmann::ordered_json SerializeNode(const Core::FileNode& node) {
            nlohmann::ordered_json j;
            j["name"] = node.name;
            j["path"] = node.full_path;

            if (const auto* info = std::get_if<Core::FileInfo>(&node.data)) {
                j["type"] = "file";
                j["size"] = info->size;
                j["offset"] = info->offset;
                j["format"] = info->format;
            } else if (const auto* folder = std::get_if<Core::FolderInfo>(&node.data)) {
                j["type"] = "folder";
                j["children"] = nlohmann::ordered_json::array();
                for (const auto& child : folder->children) {
                    j["children"].push_back(SerializeNode(child));
                }
            }
            return j;
        }

        void CollectOldFiles(const nlohmann::ordered_json& node, std::unordered_map<std::string, uint64_t>& out_map) {
            if (!node.is_object() || !node.contains("type")) return;
            const auto& type = node["type"];
            if (type == "file") {
                if (node.contains("path") && node.contains("size")) {
                    out_map[Core::NormalizeArchivePath(node["path"].get<std::string>())] = node["size"].get<uint64_t>();
                }
            } else if (type == "folder" && node.contains("children") && node["children"].is_array()) {
                for (const auto& child : node["children"]) {
                    CollectOldFiles(child, out_map);
                }
            }
        }

        void CollectCurrentFiles(const Core::FileNode& node, std::unordered_map<std::string, uint64_t>& out_map) {
            if (const auto* info = std::get_if<Core::FileInfo>(&node.data)) {
                out_map[Core::NormalizeArchivePath(node.full_path)] = info->size;
            } else if (const auto* folder = std::get_if<Core::FolderInfo>(&node.data)) {
                for (const auto& child : folder->children) {
                    CollectCurrentFiles(child, out_map);
                }
            }
        }

        /**
         * Hash-indexed O(N) builder for DiffNode trees with precomputed folder stats.
         */
        class DiffTreeBuilder {
        public:
            DiffTreeBuilder() {
                BuildNode root;
                root.name = "Diff Root";
                root.is_folder = true;
                m_nodes.push_back(std::move(root));
            }

            void Insert(std::string_view raw_path, uint64_t size, DiffStatus status) {
                const std::string clean = Core::NormalizeArchivePath(raw_path);
                const auto parts = Core::SplitPath(clean);
                if (parts.empty()) return;

                size_t current_idx = 0;
                std::string current_full_path;

                for (size_t i = 0; i < parts.size(); ++i) {
                    if (!current_full_path.empty()) current_full_path.push_back('/');
                    current_full_path.append(parts[i]);
                    const bool is_last = (i + 1 == parts.size());

                    auto& index_map = m_nodes[current_idx].child_index;
                    if (const auto it = index_map.find(parts[i]); it != index_map.end()) {
                        current_idx = it->second;
                        BuildNode& existing = m_nodes[current_idx];
                        if (is_last) {
                            if (status != DiffStatus::Unchanged) existing.status = status;
                        } else {
                            if (status != DiffStatus::Unchanged && existing.status == DiffStatus::Unchanged) {
                                existing.status = DiffStatus::Modified;
                            }
                        }
                    } else {
                        const size_t new_idx = m_nodes.size();
                        index_map.emplace(parts[i], new_idx);
                        m_nodes[current_idx].children.push_back(new_idx);

                        BuildNode node;
                        node.name = parts[i];
                        node.full_path = current_full_path;
                        node.is_folder = !is_last;
                        if (is_last) {
                            node.size = size;
                            node.status = status;
                            node.format = Core::GetExtension(parts[i]);
                        } else {
                            node.status = (status == DiffStatus::Unchanged) ? DiffStatus::Unchanged : DiffStatus::Modified;
                        }
                        m_nodes.push_back(std::move(node));
                        current_idx = new_idx;
                    }
                }
            }

            [[nodiscard]] std::unique_ptr<DiffNode> Build() const {
                return Materialize(0);
            }

        private:
            struct BuildNode {
                std::string name;
                std::string full_path;
                bool is_folder = false;
                uint64_t size = 0;
                std::string format;
                DiffStatus status = DiffStatus::Unchanged;
                std::vector<size_t> children;
                std::unordered_map<std::string, size_t> child_index;
            };

            [[nodiscard]] std::unique_ptr<DiffNode> Materialize(size_t idx) const {
                const BuildNode& src = m_nodes[idx];
                auto out = std::make_unique<DiffNode>();
                out->name = src.name;
                out->full_path = src.full_path;
                out->is_folder = src.is_folder;
                out->format = src.format;
                out->status = src.status;

                if (!src.is_folder) {
                    out->size = src.size;
                    out->file_count = 1;
                    return out;
                }

                out->children.reserve(src.children.size());
                for (const size_t child_idx : src.children) {
                    auto child = Materialize(child_idx);
                    out->size += child->size;
                    out->file_count += child->file_count;
                    out->children.push_back(std::move(child));
                }

                std::ranges::sort(out->children, [](const std::unique_ptr<DiffNode>& a, const std::unique_ptr<DiffNode>& b) {
                    if (a->is_folder != b->is_folder) return a->is_folder > b->is_folder;
                    return a->name < b->name;
                });

                return out;
            }

            std::vector<BuildNode> m_nodes;
        };

    } // namespace

    nlohmann::ordered_json FileMapSerializer::ToJson(const Core::FileNode& root) {
        nlohmann::ordered_json tree = SerializeNode(root);
        nlohmann::ordered_json result;
        result["generator"] = AppInfo::FileMapGeneratorId;
        for (auto& el : tree.items()) {
            result[el.key()] = std::move(el.value());
        }
        return result;
    }

    void FileMapSerializer::SaveToFile(const Core::FileNode& root, const std::filesystem::path& path) {
        std::ofstream out(path);
        if (!out.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + Core::PathToUtf8(path));
        }
        out << ToJson(root).dump(2);
    }

    bool FileMapSerializer::IsValid(const nlohmann::ordered_json& json_doc) noexcept {
        if (!json_doc.is_object()) return false;
        if (!json_doc.contains("generator") || !json_doc["generator"].is_string()) return false;
        if (json_doc["generator"].get<std::string>() != AppInfo::FileMapGeneratorId) return false;
        if (!json_doc.contains("type") || json_doc["type"] != "folder") return false;
        if (!json_doc.contains("children") || !json_doc["children"].is_array()) return false;
        return true;
    }

    nlohmann::ordered_json FileMapSerializer::LoadFromFile(const std::filesystem::path& path) {
        std::ifstream in(path);
        if (!in.is_open()) {
            throw std::runtime_error("Error opening filemap.json: " + Core::PathToUtf8(path));
        }
        nlohmann::ordered_json parsed = nlohmann::ordered_json::parse(in);
        if (!IsValid(parsed)) {
            throw std::runtime_error("Selected file is not a valid filemap generated by this tool.");
        }
        return parsed;
    }

    std::unique_ptr<DiffNode> ArchiveDiff::Build(
        const nlohmann::ordered_json& old_filemap,
        const Core::FileNode& current_root)
    {
        std::unordered_map<std::string, uint64_t> old_map;
        CollectOldFiles(old_filemap, old_map);

        std::unordered_map<std::string, uint64_t> new_map;
        CollectCurrentFiles(current_root, new_map);

        DiffTreeBuilder builder;

        for (const auto& [path, old_size] : old_map) {
            if (const auto it = new_map.find(path); it == new_map.end()) {
                builder.Insert(path, old_size, DiffStatus::Removed);
            } else if (it->second != old_size) {
                builder.Insert(path, it->second, DiffStatus::Modified);
            } else {
                builder.Insert(path, it->second, DiffStatus::Unchanged);
            }
        }

        for (const auto& [path, new_size] : new_map) {
            if (!old_map.contains(path)) {
                builder.Insert(path, new_size, DiffStatus::Added);
            }
        }

        return builder.Build();
    }

} // namespace Services
