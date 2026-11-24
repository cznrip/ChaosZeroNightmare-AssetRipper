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
    };

    struct FolderInfo {
        std::vector<FileNode> children;
        uint64_t total_bytes = 0;
        uint32_t file_count = 0;
    };

    struct FileNode {
        std::string name;
        std::string full_path;
        std::variant<FileInfo, FolderInfo> data;

        [[nodiscard]] bool IsFolder() const noexcept {
            return std::holds_alternative<FolderInfo>(data);
        }

        [[nodiscard]] bool IsFile() const noexcept {
            return std::holds_alternative<FileInfo>(data);
        }
    };

    inline void XorBuffer(uint8_t* buffer, size_t size, size_t file_offset) noexcept {
        XorDataPackBuffer(std::span<uint8_t>(buffer, size), file_offset);
    }

} // namespace Core
