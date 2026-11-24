#pragma once

#include "archive/IArchive.hpp"

#include <filesystem>
#include <memory>
#include <string>

/**
 * Factory for opening standalone archives:
 * - .ssra / manifest.ssra -> SsraArchive
 * - directory or data.pack -> DataPack (DirectoryArchive will be split out next)
 */
class ArchiveFactory {
public:
    [[nodiscard]] static std::unique_ptr<IArchive> Open(const std::filesystem::path& path);
    [[nodiscard]] static std::unique_ptr<IArchive> Open(const std::wstring& wpath);
};
