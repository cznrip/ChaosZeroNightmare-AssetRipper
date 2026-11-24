#include "archive/ArchiveFactory.hpp"
#include "archive/DataPackArchive.hpp"
#include "archive/DirectoryArchive.hpp"
#include "archive/SsraArchive.hpp"
#include "core/StringUtils.hpp"

#include <system_error>

std::unique_ptr<IArchive> ArchiveFactory::Open(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        return std::make_unique<DirectoryArchive>(path);
    }

    const std::string ext = Core::ToLower(Core::PathToUtf8(path.extension()));
    if (ext == ".ssra" || Core::ToLower(Core::PathToUtf8(path.filename())) == "manifest.ssra") {
        return std::make_unique<SsraArchive>(path.wstring());
    }

    return std::make_unique<DataPackArchive>(path.wstring());
}

std::unique_ptr<IArchive> ArchiveFactory::Open(const std::wstring& wpath) {
    return Open(std::filesystem::path(wpath));
}
