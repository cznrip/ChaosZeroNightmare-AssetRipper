#pragma once

#include "core/Core.hpp"
#include "parsers/ScspParser.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class IArchive;

struct SpineEntry {
    std::string name;                                       // raw .scsp filename minus extension
    std::string display_name;                               // display name in catalog
    std::string category;                                   // normalized parent directory path
    const Core::FileNode* scsp_node = nullptr;              // .scsp skeleton file
    const Core::FileNode* atlas_node = nullptr;             // matching .atlas file
    mutable std::vector<const Core::FileNode*> image_nodes; // texture images referenced by atlas

    mutable ScspParser::HeaderInfo header_info;
    mutable bool details_loaded = false;
};

struct SpineCategory {
    std::string name;
    std::string full_path;
    std::vector<size_t> entry_indices;
    std::map<std::string, SpineCategory> subcategories;
    size_t total_entries = 0; // precomputed recursive count
};

/**
 * Indexes .scsp skeletons paired with their .atlas files and organizes them
 * into a hierarchical category tree with lazy detail loading.
 */
class SpineCatalog {
public:
    void Build(IArchive& archive, const Core::FileNode& root);
    void Clear();
    void EnsureDetailsLoaded(IArchive& archive, const SpineEntry& entry) const;

    [[nodiscard]] const std::vector<SpineEntry>& GetEntries() const noexcept { return m_entries; }
    [[nodiscard]] const SpineCategory& GetRootCategory() const noexcept { return m_root_category; }
    [[nodiscard]] bool IsBuilt() const noexcept { return m_built; }

private:
    void CollectFiles(const Core::FileNode& node);
    void MatchEntries();
    void BuildCategories();
    static size_t ComputeCategoryCounts(SpineCategory& category) noexcept;
    static std::vector<std::string> ParseAtlasTextureNames(const std::vector<uint8_t>& atlas_data);

    std::vector<SpineEntry> m_entries;
    SpineCategory m_root_category;
    bool m_built = false;

    std::unordered_map<std::string, const Core::FileNode*> m_scsp_files;
    std::unordered_map<std::string, const Core::FileNode*> m_atlas_files;
    std::unordered_map<std::string, const Core::FileNode*> m_all_files;
};
