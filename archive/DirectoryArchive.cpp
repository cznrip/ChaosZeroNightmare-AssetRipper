#include "archive/DirectoryArchive.hpp"
#include "core/Logger.hpp"
#include "core/StringUtils.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

DirectoryArchive::DirectoryArchive(const std::filesystem::path& directory_path) {
    pack_path = directory_path.wstring();
    type = std::filesystem::is_directory(directory_path) ? PackType::LocalDirectory : PackType::Unknown;
    root_node.name = "root";
    root_node.full_path = "/";
    root_node.data = Core::FolderInfo{};
}

void DirectoryArchive::Scan(Core::ProgressReporter progress) {
    BeginTreeBuild("root");

    try {
        const std::filesystem::path base_path(pack_path);
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 base_path, std::filesystem::directory_options::skip_permission_denied, ec))
        {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!entry.is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }

            const std::filesystem::path rel_path = std::filesystem::relative(entry.path(), base_path, ec);
            if (ec) {
                ec.clear();
                continue;
            }

            const uint64_t size = entry.file_size(ec);
            if (ec) {
                ec.clear();
                continue;
            }

            AddFileToTree(Core::PathToUtf8(rel_path), 0, size);
        }
    } catch (const std::exception& e) {
        LogError(std::string("Error scanning local directory: ") + e.what());
    }

    FinalizeTree();
    progress.Complete();
}

std::vector<uint8_t> DirectoryArchive::GetFileData(const Core::FileNode& node) {
    const auto* info = std::get_if<Core::FileInfo>(&node.data);
    if (!info) return {};

    const std::filesystem::path full_path = std::filesystem::path(pack_path) / Core::Utf8ToWString(node.full_path);
    try {
        std::ifstream file(full_path, std::ios::binary);
        if (!file.is_open()) {
            LogError("Could not open local file: " + Core::PathToUtf8(full_path));
            return {};
        }

        std::vector<uint8_t> data(info->size);
        if (info->size > 0 && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(info->size))) {
            LogError("Failed to read local file: " + Core::PathToUtf8(full_path));
            return {};
        }
        return data;
    } catch (const std::exception& e) {
        LogError(std::string("Error reading local directory file data: ") + e.what());
        return {};
    }
}
