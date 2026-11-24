#pragma once

#include "archive/ArchiveBase.hpp"

#include <filesystem>
#include <string>
#include <vector>

/**
 * Archive implementation that browses and reads files directly from a local filesystem directory.
 */
class DirectoryArchive final : public ArchiveBase {
public:
    explicit DirectoryArchive(const std::filesystem::path& directory_path);
    ~DirectoryArchive() override = default;

    void Scan(Core::ProgressReporter progress) override;
    std::vector<uint8_t> GetFileData(const Core::FileNode& node) override;
};
