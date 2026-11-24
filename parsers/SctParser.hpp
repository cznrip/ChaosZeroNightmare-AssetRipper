#pragma once
#include <vector>
#include <cstdint>

namespace SctParser {
    struct RgbaImage {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
    };

    RgbaImage ConvertToRgba(const std::vector<uint8_t> &data);
    std::vector<uint8_t> ConvertToPng(const std::vector<uint8_t> &data);
}
