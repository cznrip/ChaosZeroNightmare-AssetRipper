#pragma once

#include "core/Core.hpp"
#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

enum class DiffStatus {
    Unchanged,
    Added,
    Modified,
    Removed
};

struct DiffNode {
    std::string name{};
    std::string full_path{};
    bool is_folder = false;
    uint64_t size = 0;         // file size or precomputed subtree total bytes
    uint32_t file_count = 0;   // 1 for file, precomputed subtree file count for folder
    std::string format{};
    DiffStatus status = DiffStatus::Unchanged;
    std::vector<std::unique_ptr<DiffNode>> children;
};

namespace Services {

    class FileMapSerializer {
    public:
        [[nodiscard]] static nlohmann::ordered_json ToJson(const Core::FileNode& root);
        static void SaveToFile(const Core::FileNode& root, const std::filesystem::path& path);
        [[nodiscard]] static bool IsValid(const nlohmann::ordered_json& filemap) noexcept;
        [[nodiscard]] static nlohmann::ordered_json LoadFromFile(const std::filesystem::path& path);
    };

    class ArchiveDiff {
    public:
        [[nodiscard]] static std::unique_ptr<DiffNode> Build(const nlohmann::ordered_json& old_filemap, const Core::FileNode& current_root);
    };

} // namespace Services
