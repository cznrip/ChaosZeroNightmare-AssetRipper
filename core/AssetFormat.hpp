#pragma once

#include <string>
#include <string_view>

#include "core/StringUtils.hpp"

namespace Core {

    enum class AssetType {
        Sct,
        Image,
        AnimatedWebP,
        Database,
        Scsp,
        Atlas,
        Json,
        Text,
        Binary
    };

    inline bool IsSctExtension(std::string_view ext) {
        const std::string lower = ToLower(ext);
        return lower == ".sct" || lower == ".sct2";
    }

    inline bool IsDatabaseExtension(std::string_view ext) {
        return ToLower(ext) == ".db";
    }

    inline bool IsScspExtension(std::string_view ext) {
        return ToLower(ext) == ".scsp";
    }

    inline bool IsAtlasExtension(std::string_view ext) {
        return ToLower(ext) == ".atlas";
    }

    inline bool IsAnimatedWebPExtension(std::string_view ext) {
        return ToLower(ext) == ".webp";
    }

    inline bool IsStandardImageExtension(std::string_view ext) {
        const std::string lower = ToLower(ext);
        return lower == ".png" || lower == ".jpg" || lower == ".jpeg" ||
               lower == ".bmp" || lower == ".tga";
    }

    inline bool IsAnyImageExtension(std::string_view ext) {
        return IsSctExtension(ext) || IsStandardImageExtension(ext) || IsAnimatedWebPExtension(ext);
    }

    inline bool IsPreviewableImageExtension(std::string_view ext) {
        return IsSctExtension(ext) || IsStandardImageExtension(ext);
    }

    inline bool IsJsonExtension(std::string_view ext) {
        const std::string lower = ToLower(ext);
        return lower == ".json" || lower == ".battletemplate" || lower == ".config" ||
               lower == ".sfgm" || lower == ".camera" || lower == ".setting" ||
               lower == ".sract" || lower == ".srmd" || lower == ".srcs" ||
               lower == ".monstertest" || lower == ".dat";
    }

    inline bool IsTextExtension(std::string_view ext) {
        const std::string lower = ToLower(ext);
        return lower == ".txt" || lower == ".cfx" || lower == ".atlas" ||
               lower == ".fnt" || lower == ".srt" || lower == ".bat";
    }

    inline AssetType ClassifyExtension(std::string_view ext) {
        const std::string lower = ToLower(ext);
        if (lower == ".sct" || lower == ".sct2") return AssetType::Sct;
        if (lower == ".db") return AssetType::Database;
        if (lower == ".scsp") return AssetType::Scsp;
        if (lower == ".atlas") return AssetType::Atlas;
        if (lower == ".webp") return AssetType::AnimatedWebP;
        if (IsStandardImageExtension(lower)) return AssetType::Image;
        if (IsJsonExtension(lower)) return AssetType::Json;
        if (IsTextExtension(lower)) return AssetType::Text;
        return AssetType::Binary;
    }

    /**
     * Rewrites all .sct2 and .sct texture references inside an atlas text to .png.
     * Always replaces ".sct2" before ".sct" so ".sct2" never becomes ".png2".
     */
    inline std::string RewriteAtlasExtensions(std::string_view atlas_text) {
        std::string result(atlas_text);
        for (const std::string_view from : {std::string_view{".sct2"}, std::string_view{".sct"}}) {
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), ".png");
                pos += 4;
            }
        }
        return result;
    }

} // namespace Core
