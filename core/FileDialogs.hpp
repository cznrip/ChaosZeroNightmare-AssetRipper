#pragma once

#include "core/AppSettings.hpp"
#include "core/StringUtils.hpp"
#include "portable-file-dialogs.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Core::FileDialogs {

    namespace Detail {
        struct FolderSeed {
            std::string path;
            pfd::opt options;
        };

        inline FolderSeed MakeFolderSeed(const std::string& remembered) {
            if (remembered.empty()) {
                return {".", pfd::opt::none};
            }
            return {remembered, pfd::opt::force_path};
        }

        inline std::string MakeSaveStartPath(const std::string& remembered, const std::string& default_name) {
            if (remembered.empty()) return default_name;
            return PathToUtf8(std::filesystem::path(Utf8ToWString(remembered)) / Utf8ToWString(default_name));
        }
    } // namespace Detail

    inline std::string SaveFile(
        const std::string& title,
        const std::string& default_name,
        const std::vector<std::string>& filters)
    {
        auto& settings = AppSettings::Instance();
        const std::string remembered = settings.GetRememberedDir(DialogSlot::Export);
        pfd::save_file dialog(title, Detail::MakeSaveStartPath(remembered, default_name), filters);
        const std::string picked = dialog.result();
        settings.RememberParentOf(DialogSlot::Export, picked);
        return picked;
    }

    inline std::string OpenFile(
        DialogSlot slot,
        const std::string& title,
        const std::vector<std::string>& filters)
    {
        auto& settings = AppSettings::Instance();
        const std::string remembered = settings.GetRememberedDir(slot);
        pfd::open_file dialog(title, remembered, filters);
        const std::vector<std::string> picked = dialog.result();
        if (picked.empty()) return {};
        settings.RememberParentOf(slot, picked[0]);
        return picked[0];
    }

    inline std::string SelectFolder(DialogSlot slot, const std::string& title) {
        auto& settings = AppSettings::Instance();
        const auto [path, options] = Detail::MakeFolderSeed(settings.GetRememberedDir(slot));
        pfd::select_folder dialog(title, path, options);
        const std::string picked = dialog.result();
        settings.RememberDir(slot, picked);
        return picked;
    }

} // namespace Core::FileDialogs
