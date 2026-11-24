#pragma once

#include "json.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace DbParser {

    /**
     * Decrypts and parses a Yuna engine .db blob directly into a JSON array of row objects.
     * Returns an empty JSON object on failure.
     */
    [[nodiscard]] nlohmann::ordered_json ParseToJson(std::span<const uint8_t> data);

    /**
     * Decrypts and converts a .db blob into a pretty-printed JSON string.
     */
    [[nodiscard]] std::string ConvertToJson(std::span<const uint8_t> data);

} // namespace DbParser
