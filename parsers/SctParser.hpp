#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace SctParser {

    struct RgbaImage {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
    };

    [[nodiscard]] RgbaImage ConvertToRgba(std::span<const uint8_t> data);
    [[nodiscard]] std::vector<uint8_t> ConvertToPng(std::span<const uint8_t> data);

} // namespace SctParser
