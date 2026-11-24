#include "archive/ArchiveFactory.hpp"
#include "archive/DataPack.hpp"
#include "archive/SsraArchive.hpp"
#include "core/StringUtils.hpp"

std::unique_ptr<IArchive> ArchiveFactory::Open(const std::filesystem::path& path) {
    const std::string ext = Core::ToLower(Core::PathToUtf8(path.extension()));
    if (ext == ".ssra" || Core::ToLower(Core::PathToUtf8(path.filename())) == "manifest.ssra") {
        return std::make_unique<SsraArchive>(path.wstring());
    }
    return std::make_unique<DataPack>(path.wstring());
}

std::unique_ptr<IArchive> ArchiveFactory::Open(const std::wstring& wpath) {
    return Open(std::filesystem::path(wpath));
}
