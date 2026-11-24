#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Core {

    inline std::string WStringToUtf8(std::wstring_view wstr) {
        if (wstr.empty()) return {};
#ifdef _WIN32
        const int size_needed = WideCharToMultiByte(
            CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
            nullptr, 0, nullptr, nullptr);
        if (size_needed <= 0) return {};
        std::string result(static_cast<size_t>(size_needed), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
            result.data(), size_needed, nullptr, nullptr);
        return result;
#else
        return std::string(wstr.begin(), wstr.end());
#endif
    }

    inline std::wstring Utf8ToWString(std::string_view str) {
        if (str.empty()) return {};
#ifdef _WIN32
        const int size_needed = MultiByteToWideChar(
            CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
            nullptr, 0);
        if (size_needed <= 0) return {};
        std::wstring result(static_cast<size_t>(size_needed), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
            result.data(), size_needed);
        return result;
#else
        return std::wstring(str.begin(), str.end());
#endif
    }

    inline std::string PathToUtf8(const std::filesystem::path& p) {
        return WStringToUtf8(p.wstring());
    }

    inline std::string ToLower(std::string_view s) {
        std::string result(s);
        std::ranges::transform(result, result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return result;
    }

    inline std::string Trim(std::string_view value) {
        constexpr std::string_view kWhitespace = " \t\r\n";
        const size_t start = value.find_first_not_of(kWhitespace);
        if (start == std::string_view::npos) return {};
        const size_t end = value.find_last_not_of(kWhitespace);
        return std::string(value.substr(start, end - start + 1));
    }

    /**
     * Normalizes an archive-relative path: truncates at NUL, converts '\' to '/',
     * collapses consecutive slashes, and strips leading/trailing slashes.
     */
    inline std::string NormalizeArchivePath(std::string_view raw) {
        if (const size_t nul = raw.find('\0'); nul != std::string_view::npos) {
            raw = raw.substr(0, nul);
        }
        std::string normalized;
        normalized.reserve(raw.size());

        bool last_was_slash = true;
        for (const char c : raw) {
            const char ch = (c == '\\') ? '/' : c;
            if (ch == '/') {
                if (last_was_slash) continue;
                last_was_slash = true;
                normalized.push_back('/');
                continue;
            }
            last_was_slash = false;
            normalized.push_back(ch);
        }

        while (!normalized.empty() && normalized.back() == '/') {
            normalized.pop_back();
        }
        return normalized;
    }

    inline std::vector<std::string> SplitPath(std::string_view path) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start < path.size()) {
            const size_t slash = path.find('/', start);
            const size_t end = (slash == std::string_view::npos) ? path.size() : slash;
            if (end > start) {
                parts.emplace_back(path.substr(start, end - start));
            }
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        return parts;
    }

    inline std::string GetDirectory(std::string_view path) {
        const size_t sep = path.find_last_of('/');
        if (sep == std::string_view::npos) return {};
        return std::string(path.substr(0, sep));
    }

    inline std::string GetBasename(std::string_view path) {
        const size_t sep = path.find_last_of('/');
        if (sep == std::string_view::npos) return std::string(path);
        return std::string(path.substr(sep + 1));
    }

    inline std::string GetExtension(std::string_view name) {
        const size_t dot = name.find_last_of('.');
        if (dot == std::string_view::npos) return {};
        return std::string(name.substr(dot));
    }

    inline std::string GetExtensionLower(std::string_view name) {
        return ToLower(GetExtension(name));
    }

    inline std::string StripExtension(std::string_view name) {
        const size_t dot = name.find_last_of('.');
        if (dot == std::string_view::npos) return std::string(name);
        return std::string(name.substr(0, dot));
    }

    inline std::string ReplaceExtension(std::string_view name, std::string_view new_ext) {
        const size_t dot = name.find_last_of('.');
        std::string result(dot != std::string_view::npos ? name.substr(0, dot) : name);
        result.append(new_ext);
        return result;
    }

    inline std::string FormatSize(uint64_t bytes) {
        static constexpr std::array<std::string_view, 5> kUnits = {"B", "KB", "MB", "GB", "TB"};
        auto size = static_cast<double>(bytes);
        size_t unit = 0;
        while (size >= 1024.0 && unit + 1 < kUnits.size()) {
            size /= 1024.0;
            ++unit;
        }
        return std::format("{:.2f} {}", size, kUnits[unit]);
    }

} // namespace Core
