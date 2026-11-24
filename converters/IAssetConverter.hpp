#pragma once

#include "core/AssetFormat.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ExportOptions {
    bool convert_sct_to_png = true;
    bool convert_db_to_json = true;
};

/**
 * Strategy interface for transforming raw archive bytes into an exported format.
 */
class IAssetConverter {
public:
    virtual ~IAssetConverter() = default;

    [[nodiscard]] virtual bool Supports(Core::AssetType type, const ExportOptions& options) const noexcept = 0;
    [[nodiscard]] virtual std::string TargetExtension(std::string_view original_ext) const = 0;
    [[nodiscard]] virtual std::optional<std::vector<uint8_t>> Convert(
        std::span<const uint8_t> raw_data,
        std::string_view filename) const = 0;
};
