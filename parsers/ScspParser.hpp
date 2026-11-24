#pragma once

#include "json.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace ScspParser {

    struct HeaderInfo {
        float width = 0.0f;
        float height = 0.0f;
        std::string version;
        std::string images_path;
        std::string audio_path;
        std::string hash;
    };

    [[nodiscard]] nlohmann::ordered_json ParseToJson(std::span<const uint8_t> scsp_data);
    [[nodiscard]] std::string ConvertToJson(std::span<const uint8_t> scsp_data);
    [[nodiscard]] HeaderInfo ExtractHeader(std::span<const uint8_t> scsp_data);

} // namespace ScspParser
