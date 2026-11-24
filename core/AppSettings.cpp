#include "core/AppSettings.hpp"
#include "core/StringUtils.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace Core {

    namespace {
        bool ParseBool(const std::string_view value, const bool default_value) {
            const std::string normalized = ToLower(Trim(value));
            if (normalized == "1" || normalized == "true") return true;
            if (normalized == "0" || normalized == "false") return false;
            return default_value;
        }
    } // namespace

    AppSettings::AppSettings() {
        Load(m_ini_path);
    }

    AppSettings& AppSettings::Instance() {
        static AppSettings instance;
        return instance;
    }

    void AppSettings::Load(std::string_view ini_path) {
        m_ini_path = std::string(ini_path);
        m_options = AppOptions{};
        m_loaded = true;

        std::ifstream in(m_ini_path);
        if (!in.is_open()) {
            Save();
            return;
        }

        std::string line;
        while (std::getline(in, line)) {
            const std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
                continue;
            }

            const size_t sep = trimmed.find('=');
            if (sep == std::string::npos) continue;

            const std::string key = Trim(std::string_view(trimmed).substr(0, sep));
            const std::string value = Trim(std::string_view(trimmed).substr(sep + 1));

            if (key == "export_sct_as_png") {
                m_options.export_sct_as_png = ParseBool(value, m_options.export_sct_as_png);
            } else if (key == "export_db_as_json") {
                m_options.export_db_as_json = ParseBool(value, m_options.export_db_as_json);
            } else if (key == "enable_open_folder") {
                m_options.enable_open_folder = ParseBool(value, m_options.enable_open_folder);
            } else if (key == "last_open_dir") {
                m_options.last_open_dir = value;
            } else if (key == "last_extract_dir") {
                m_options.last_extract_dir = value;
            } else if (key == "last_export_dir") {
                m_options.last_export_dir = value;
            }
        }
    }

    void AppSettings::Save() const {
        std::ofstream out(m_ini_path, std::ios::trunc);
        if (!out.is_open()) return;

        out << "[options]\n";
        out << "export_sct_as_png=" << (m_options.export_sct_as_png ? 1 : 0) << '\n';
        out << "export_db_as_json=" << (m_options.export_db_as_json ? 1 : 0) << '\n';
        out << "enable_open_folder=" << (m_options.enable_open_folder ? 1 : 0) << '\n';
        out << "last_open_dir=" << m_options.last_open_dir << '\n';
        out << "last_extract_dir=" << m_options.last_extract_dir << '\n';
        out << "last_export_dir=" << m_options.last_export_dir << '\n';
        out.flush();
    }

    std::string& AppSettings::SlotRef(const DialogSlot slot) noexcept {
        switch (slot) {
            case DialogSlot::Extract: return m_options.last_extract_dir;
            case DialogSlot::Export:  return m_options.last_export_dir;
            case DialogSlot::Open:
            default:                  return m_options.last_open_dir;
        }
    }

    const std::string& AppSettings::SlotRef(const DialogSlot slot) const noexcept {
        switch (slot) {
            case DialogSlot::Extract: return m_options.last_extract_dir;
            case DialogSlot::Export:  return m_options.last_export_dir;
            case DialogSlot::Open:
            default:                  return m_options.last_open_dir;
        }
    }

    std::string AppSettings::GetRememberedDir(const DialogSlot slot) const {
        const std::string& stored = SlotRef(slot);
        if (stored.empty()) return {};

        std::error_code error_code;
        if (!std::filesystem::is_directory(std::filesystem::path(Utf8ToWString(stored)), error_code)) {
            return {};
        }
        return stored;
    }

    void AppSettings::RememberDir(const DialogSlot slot, const std::string_view dir) {
        if (dir.empty()) return;
        std::string& stored = SlotRef(slot);
        if (stored == dir) return;
        stored = std::string(dir);
        Save();
    }

    void AppSettings::RememberParentOf(const DialogSlot slot, const std::string_view file_path) {
        if (file_path.empty()) return;
        const std::filesystem::path parent = std::filesystem::path(Utf8ToWString(file_path)).parent_path();
        if (!parent.empty()) {
            RememberDir(slot, PathToUtf8(parent));
        }
    }

} // namespace Core
