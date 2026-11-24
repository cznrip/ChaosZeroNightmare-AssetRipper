#pragma once

#include "converters/IAssetConverter.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ConvertedAsset {
    std::vector<uint8_t> data;
    std::string extension;
    bool converted = false;
};

/**
 * Registry/dispatcher managing IAssetConverter strategies.
 * Single source of truth for converting SCT, Atlas, DB, and SCSP assets.
 */
class AssetConverter {
public:
    AssetConverter();

    void Register(std::unique_ptr<IAssetConverter> converter);

    [[nodiscard]] std::string ResolveExtension(std::string_view original_ext, const ExportOptions& options) const;

    [[nodiscard]] ConvertedAsset Convert(
        std::span<const uint8_t> raw_data,
        std::string_view filename,
        std::string_view original_ext,
        const ExportOptions& options) const;

private:
    [[nodiscard]] const IAssetConverter* FindConverter(Core::AssetType type, const ExportOptions& options) const noexcept;

    std::vector<std::unique_ptr<IAssetConverter>> m_converters;
};
