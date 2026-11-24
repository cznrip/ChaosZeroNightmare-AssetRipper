#pragma once

#include "core/Crypto.hpp"
#include "core/StringUtils.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Core {

    struct FileNode;

    struct FileInfo {
        uint64_t offset = 0;
        uint64_t size = 0;
        std::string format;
        uint32_t archive_id = 0;
    };

    struct FolderInfo {
        std::vector<FileNode> children;
    };

    struct FileNode {
        std::string name;
        std::string full_path;
        std::variant<FileInfo, FolderInfo> data;
    };

    inline void XorBuffer(uint8_t* buffer, size_t size, size_t file_offset) noexcept {
        XorDataPackBuffer(std::span<uint8_t>(buffer, size), file_offset);
    }

} // namespace Core
