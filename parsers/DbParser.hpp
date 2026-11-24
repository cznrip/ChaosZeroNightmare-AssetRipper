#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace DbParser
{
    std::string ConvertToJson(const std::vector<uint8_t> &data);
}
