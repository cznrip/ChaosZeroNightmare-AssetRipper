#include "parsers/SpineCatalog.hpp"
#include "archive/IArchive.hpp"
#include "core/AssetFormat.hpp"
#include "core/Logger.hpp"
#include "core/StringUtils.hpp"

#include <algorithm>
#include <format>
#include <sstream>

void SpineCatalog::Clear() {
    m_entries.clear();
    m_root_category = SpineCategory{};
    m_scsp_files.clear();
    m_atlas_files.clear();
    m_all_files.clear();
    m_built = false;
}

void SpineCatalog::CollectFiles(const Core::FileNode& node) {
    if (node.IsFile()) {
        const std::string path = Core::NormalizeArchivePath(node.full_path);
        const std::string ext = Core::GetExtensionLower(node.name);
        m_all_files[path] = &node;

        if (Core::IsScspExtension(ext)) {
            m_scsp_files[path] = &node;
        } else if (Core::IsAtlasExtension(ext)) {
            m_atlas_files[path] = &node;
        }
        return;
    }

    for (const auto& child : std::get<Core::FolderInfo>(node.data).children) {
        CollectFiles(child);
    }
}

std::vector<std::string> SpineCatalog::ParseAtlasTextureNames(const std::vector<uint8_t>& atlas_data) {
    std::vector<std::string> textures;
    const std::string content(atlas_data.begin(), atlas_data.end());
    std::istringstream ss(content);
    std::string line;
    bool after_blank = true;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            after_blank = true;
            continue;
        }
        if (after_blank && line[0] != ' ' && line[0] != '\t' && line.find('.') != std::string::npos) {
            textures.push_back(line);
        }
        after_blank = false;
    }
    return textures;
}

void SpineCatalog::MatchEntries() {
    size_t skipped_no_atlas = 0;

    std::unordered_map<std::string, std::vector<const Core::FileNode*>> atlases_by_dir;
    for (const auto& [path, node] : m_atlas_files) {
        atlases_by_dir[Core::GetDirectory(path)].push_back(node);
    }

    m_entries.reserve(m_scsp_files.size());
    for (const auto& [scsp_path, scsp_node] : m_scsp_files) {
        const std::string base_name = Core::StripExtension(scsp_node->name);
        const std::string dir = Core::GetDirectory(scsp_path);

        const Core::FileNode* atlas_node = nullptr;
        const std::string atlas_candidate = dir.empty() ? (base_name + ".atlas") : (dir + "/" + base_name + ".atlas");
        if (const auto it = m_atlas_files.find(atlas_candidate); it != m_atlas_files.end()) {
            atlas_node = it->second;
        } else if (const auto dir_it = atlases_by_dir.find(dir); dir_it != atlases_by_dir.end() && dir_it->second.size() == 1) {
            atlas_node = dir_it->second.front();
        }

        if (!atlas_node) {
            ++skipped_no_atlas;
            continue;
        }

        SpineEntry entry;
        entry.name = base_name;
        entry.display_name = base_name;
        entry.category = dir;
        entry.scsp_node = scsp_node;
        entry.atlas_node = atlas_node;
        m_entries.push_back(std::move(entry));
    }

    LogInfo(std::format("SpineCatalog: {} entries with atlas, {} skipped (no atlas)", m_entries.size(), skipped_no_atlas));

    std::ranges::sort(m_entries, [](const SpineEntry& a, const SpineEntry& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.display_name < b.display_name;
    });
}

void SpineCatalog::EnsureDetailsLoaded(IArchive& archive, const SpineEntry& entry) const {
    if (entry.details_loaded) return;
    entry.details_loaded = true;

    try {
        if (const auto data = archive.GetFileData(*entry.scsp_node); !data.empty()) {
            entry.header_info = ScspParser::ExtractHeader(data);
        }
    } catch (...) {}

    try {
        if (const auto atlas_data = archive.GetFileData(*entry.atlas_node); !atlas_data.empty()) {
            const auto tex_names = ParseAtlasTextureNames(atlas_data);
            const std::string scsp_dir = Core::GetDirectory(Core::NormalizeArchivePath(entry.scsp_node->full_path));

            for (const auto& tex_name : tex_names) {
                const std::string direct_target = Core::NormalizeArchivePath(
                    scsp_dir.empty() ? tex_name : (scsp_dir + "/" + tex_name));

                if (const auto it = m_all_files.find(direct_target);
                    it != m_all_files.end() && Core::IsAnyImageExtension(Core::GetExtension(it->second->name)))
                {
                    entry.image_nodes.push_back(it->second);
                    continue;
                }

                if (!entry.header_info.images_path.empty()) {
                    const std::string rel = Core::NormalizeArchivePath(entry.header_info.images_path + "/" + tex_name);
                    const std::string full_target = Core::NormalizeArchivePath(
                        scsp_dir.empty() ? rel : (scsp_dir + "/" + rel));

                    if (const auto it2 = m_all_files.find(full_target);
                        it2 != m_all_files.end() && Core::IsAnyImageExtension(Core::GetExtension(it2->second->name)))
                    {
                        entry.image_nodes.push_back(it2->second);
                    }
                }
            }
        }
    } catch (...) {}
}

size_t SpineCatalog::ComputeCategoryCounts(SpineCategory& category) noexcept {
    size_t total = category.entry_indices.size();
    for (auto &sub: category.subcategories | std::views::values) {
        total += ComputeCategoryCounts(sub);
    }
    category.total_entries = total;
    return total;
}

void SpineCatalog::BuildCategories() {
    m_root_category = SpineCategory{};
    m_root_category.name = "Root";

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const std::string& cat_path = m_entries[i].category;
        if (cat_path.empty()) {
            m_root_category.entry_indices.push_back(i);
            continue;
        }

        SpineCategory* current = &m_root_category;
        std::string cur_path;
        for (const auto& part : Core::SplitPath(cat_path)) {
            if (!cur_path.empty()) cur_path.push_back('/');
            cur_path.append(part);

            auto& sub = current->subcategories[part];
            if (sub.name.empty()) {
                sub.name = part;
                sub.full_path = cur_path;
            }
            current = &sub;
        }
        current->entry_indices.push_back(i);
    }

    ComputeCategoryCounts(m_root_category);
}

void SpineCatalog::Build(const IArchive& archive, const Core::FileNode& root) {
    (void)archive;
    Clear();
    LogInfo("SpineCatalog: scanning file tree...");
    CollectFiles(root);
    LogInfo(std::format(
        "SpineCatalog: found {} .scsp, {} .atlas, {} total files",
        m_scsp_files.size(), m_atlas_files.size(), m_all_files.size()));
    MatchEntries();
    BuildCategories();
    LogInfo("SpineCatalog: built tree successfully");
    m_built = true;
}
