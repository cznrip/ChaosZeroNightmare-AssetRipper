#pragma once

#include <string>
#include <string_view>

namespace Core {

    enum class DialogSlot {
        Open,
        Extract,
        Export
    };

    struct AppOptions {
        bool export_sct_as_png = true;
        bool export_db_as_json = true;
        bool enable_open_folder = false;
        std::string last_open_dir;
        std::string last_extract_dir;
        std::string last_export_dir;
    };

    class AppSettings {
    public:
        static AppSettings& Instance();

        void Load(std::string_view ini_path = "czn_ripper.ini");
        void Save() const;

        [[nodiscard]] const AppOptions& Get() const noexcept { return m_options; }
        [[nodiscard]] AppOptions& Mutable() noexcept { return m_options; }

        [[nodiscard]] std::string GetRememberedDir(DialogSlot slot) const;
        void RememberDir(DialogSlot slot, std::string_view dir);
        void RememberParentOf(DialogSlot slot, std::string_view file_path);

    private:
        AppSettings();

        [[nodiscard]] std::string& SlotRef(DialogSlot slot) noexcept;
        [[nodiscard]] const std::string& SlotRef(DialogSlot slot) const noexcept;

        std::string m_ini_path = "czn_ripper.ini";
        AppOptions m_options;
        bool m_loaded = false;
    };

} // namespace Core
