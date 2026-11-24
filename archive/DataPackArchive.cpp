#include "archive/DataPackArchive.hpp"
#include "core/BinaryReader.hpp"
#include "core/Crypto.hpp"
#include "core/Logger.hpp"
#include "core/StringUtils.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string_view>
#include <system_error>

namespace {

    constexpr std::array<uint8_t, 5> kEncryptedMagic = {0x71, 0x40, 0xBD, 0x73, 0x93};
    constexpr std::array<uint8_t, 5> kDecryptedMagic = {'P', 'L', 'P', 'c', 'K'};
    constexpr uint8_t kEntryMarker = 0x02;
    constexpr size_t kEntryHeaderSize = 15;
    constexpr uint32_t kContainerOverhead = 19;

    bool IsLikelyPackPath(std::string_view path) noexcept {
        if (path.size() < 3 || path.size() > 2048) return false;
        if (path.find('\0') != std::string_view::npos) return false;
        bool has_file_like_char = false;
        for (const unsigned char c : path) {
            if (c < 0x20 && c != '\t') return false;
            if (c == '.' || c == '/' || c == '\\') has_file_like_char = true;
        }
        return has_file_like_char;
    }

} // namespace

DataPackArchive::DataPackArchive(const std::wstring& path) {
    pack_path = path;
    type = PackType::Unknown;
    root_node.name = "root";
    root_node.full_path = "/";
    root_node.data = Core::FolderInfo{};

    SYSTEM_INFO sys_info{};
    GetSystemInfo(&sys_info);
    m_alloc_granularity = sys_info.dwAllocationGranularity;

    const auto pack_parts = FindPackParts(path);
    if (pack_parts.empty()) {
        LogError("No pack files found: " + Core::PathToUtf8(path));
        return;
    }

    for (const auto& part_path : pack_parts) {
        if (!LoadPackPart(part_path)) {
            LogError("Failed to load pack part: " + Core::PathToUtf8(part_path));
        }
    }

    if (m_parts.empty()) {
        LogError("Failed to load any pack parts from: " + Core::PathToUtf8(path));
        return;
    }

    if (m_total_file_size < kEncryptedMagic.size()) {
        return;
    }

    std::array<uint8_t, 5> magic{};
    if (ReadBytes(0, magic) == magic.size()) {
        if (magic == kEncryptedMagic) {
            type = PackType::Encrypted;
        } else if (magic == kDecryptedMagic) {
            type = PackType::Decrypted;
        }
    }
}

std::vector<std::wstring> DataPackArchive::FindPackParts(const std::wstring& base_path) {
    std::vector<std::wstring> found_parts;
    found_parts.push_back(base_path);

    std::error_code ec;
    for (int i = 1; i < 1000; ++i) {
        std::wstring part_path = base_path + L"~" + std::to_wstring(i);
        if (!std::filesystem::is_regular_file(part_path, ec) || ec) {
            break;
        }
        found_parts.push_back(std::move(part_path));
    }
    return found_parts;
}

bool DataPackArchive::LoadPackPart(const std::wstring& path) {
    PackPart part;
    part.file.Reset(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

    if (!part.file.IsValid()) {
        LogError("Failed to open file: " + Core::PathToUtf8(path));
        return false;
    }

    LARGE_INTEGER fs{};
    if (!GetFileSizeEx(part.file.Get(), &fs)) {
        LogError("Failed to get file size for: " + Core::PathToUtf8(path));
        return false;
    }

    part.file_size = static_cast<uint64_t>(fs.QuadPart);
    if (part.file_size == 0) {
        LogError("Empty file, skipping: " + Core::PathToUtf8(path));
        return false;
    }

    part.mapping.Reset(CreateFileMappingW(part.file.Get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!part.mapping.IsValid()) {
        LogError("Failed to create file mapping for: " + Core::PathToUtf8(path));
        return false;
    }

    m_total_file_size += part.file_size;
    m_parts.push_back(std::move(part));
    return true;
}

bool DataPackArchive::EnsureWindow(PackPart& part, uint64_t offset, size_t needed) const {
    if (part.view.Covers(offset, needed)) {
        return true;
    }

    part.view.Unmap();

    const uint64_t aligned_offset = (offset / m_alloc_granularity) * m_alloc_granularity;
    const uint64_t adjustment = offset - aligned_offset;

    size_t window_size = (std::max)(kWindowSize, static_cast<size_t>(needed + adjustment));
    if (aligned_offset + window_size > part.file_size) {
        window_size = static_cast<size_t>(part.file_size - aligned_offset);
    }
    if (window_size == 0) return false;

    const auto offset_high = static_cast<DWORD>(aligned_offset >> 32);
    const auto offset_low = static_cast<DWORD>(aligned_offset & 0xFFFFFFFFu);

    const auto* mapped = static_cast<const uint8_t*>(
        MapViewOfFile(part.mapping.Get(), FILE_MAP_READ, offset_high, offset_low, window_size));

    if (!mapped) {
        LogError(std::format(
            "MapViewOfFile failed at offset {} size {} error {}",
            aligned_offset, window_size, GetLastError()));
        return false;
    }

    part.view.Assign(mapped, aligned_offset, window_size);
    return true;
}

std::span<const uint8_t> DataPackArchive::GetViewAtOffset(uint64_t offset) {
    uint64_t current_pos = 0;
    for (auto& part : m_parts) {
        if (offset < current_pos + part.file_size) {
            const uint64_t local_offset = offset - current_pos;
            const auto remaining = static_cast<size_t>(part.file_size - local_offset);
            if (!EnsureWindow(part, local_offset, 1)) {
                return {};
            }
            return part.view.SliceFrom(local_offset, remaining);
        }
        current_pos += part.file_size;
    }
    return {};
}

size_t DataPackArchive::ReadBytes(uint64_t offset, std::span<uint8_t> dest) {
    size_t total_read = 0;
    while (total_read < dest.size()) {
        const auto view = GetViewAtOffset(offset);
        if (view.empty()) break;

        const size_t to_copy = (std::min)(dest.size() - total_read, view.size());
        std::copy_n(view.data(), to_copy, dest.data() + total_read);
        total_read += to_copy;
        offset += to_copy;
    }
    return total_read;
}

std::vector<uint8_t> DataPackArchive::GetFileData(const Core::FileNode& node) {
    const auto* info = std::get_if<Core::FileInfo>(&node.data);
    if (!info) return {};

    if (info->offset >= m_total_file_size || info->offset + info->size > m_total_file_size) {
        LogError("Invalid file offset/size for: " + node.name);
        return {};
    }

    try {
        std::vector<uint8_t> data(info->size);
        if (const size_t bytes_read = ReadBytes(info->offset, data); bytes_read != info->size) {
            LogError(std::format(
                "Failed to read full file data for: {} (read {} of {})",
                node.name, bytes_read, info->size));
            return {};
        }

        if (type == PackType::Encrypted) {
            Core::XorDataPackBuffer(data, info->offset);
        }
        return data;
    } catch (const std::exception& e) {
        LogError(std::string("Error reading file data: ") + e.what());
        return {};
    }
}

void DataPackArchive::Scan(Core::ProgressReporter progress) {
    BeginTreeBuild("root");

    try {
        if (type == PackType::Encrypted) {
            ScanPack(true, progress);
        } else if (type == PackType::Decrypted) {
            ScanPack(false, progress);
        }
    } catch (const std::exception& e) {
        LogError(std::string("Error during scan: ") + e.what());
    }

    FinalizeTree();
    progress.Complete();
}

void DataPackArchive::ScanPack(bool encrypted, const Core::ProgressReporter& progress) {
    const auto& xor_key = Core::kDataPackXorKey;

    uint64_t cursor = encrypted ? 4 : 0;
    uint64_t next_progress_cursor = 0;
    const uint64_t progress_interval = std::clamp<uint64_t>(
        m_total_file_size / 500, 64ULL * 1024, 2ULL * 1024 * 1024);

    std::array<uint8_t, kEntryHeaderSize> header_buffer{};
    std::vector<uint8_t> path_buffer;

    while (cursor < m_total_file_size) {
        if (cursor >= next_progress_cursor) {
            if (m_total_file_size > 0) {
                progress.Report(static_cast<float>(cursor) / static_cast<float>(m_total_file_size));
            }
            next_progress_cursor = cursor + progress_interval;
        }

        const auto block = GetViewAtOffset(cursor);
        if (block.empty()) {
            ++cursor;
            continue;
        }

        const size_t max_scan = static_cast<size_t>((std::min<uint64_t>)(block.size(), m_total_file_size - cursor));
        size_t candidate_pos = max_scan;

        if (encrypted) {
            for (size_t pos = 0; pos < max_scan; ++pos) {
                const uint64_t abs_pos = cursor + pos;
                if ((block[pos] ^ xor_key[abs_pos % Core::kDataPackKeySize]) == kEntryMarker) {
                    candidate_pos = pos;
                    break;
                }
            }
        } else {
            const auto slice = block.first(max_scan);
            if (const auto it = std::ranges::find(slice, kEntryMarker); it != slice.end()) {
                candidate_pos = static_cast<size_t>(std::distance(slice.begin(), it));
            }
        }

        if (candidate_pos >= max_scan) {
            cursor += max_scan;
            continue;
        }

        const uint64_t abs_pos = cursor + candidate_pos;
        if (abs_pos < 4) {
            cursor = abs_pos + 1;
            continue;
        }

        const uint64_t header_offset = abs_pos - 4;
        if (header_offset + kEntryHeaderSize > m_total_file_size ||
            ReadBytes(header_offset, header_buffer) != kEntryHeaderSize)
        {
            cursor = abs_pos + 1;
            continue;
        }

        if (encrypted) {
            Core::XorDataPackBuffer(header_buffer, header_offset);
        }

        Core::BinaryReader hdr_reader(header_buffer);
        const uint32_t container_len = *hdr_reader.TryRead<uint32_t>();
        hdr_reader.Skip(1); // marker 0x02
        const uint8_t path_len = *hdr_reader.TryRead<uint8_t>();
        const uint32_t data_len = *hdr_reader.TryRead<uint32_t>();

        if (container_len > m_total_file_size ||
            path_len == 0 ||
            data_len > m_total_file_size ||
            container_len != static_cast<uint32_t>(path_len) + data_len + kContainerOverhead)
        {
            cursor = abs_pos + 1;
            continue;
        }

        const uint64_t file_offset = header_offset + kEntryHeaderSize + path_len;
        if (file_offset + data_len > m_total_file_size) {
            cursor = abs_pos + 1;
            continue;
        }

        path_buffer.resize(path_len);
        if (ReadBytes(header_offset + kEntryHeaderSize, path_buffer) != path_len) {
            cursor = abs_pos + 1;
            continue;
        }

        if (encrypted) {
            Core::XorDataPackBuffer(path_buffer, header_offset + kEntryHeaderSize);
        }

        const std::string path_str = Core::NormalizeArchivePath(
            std::string_view(reinterpret_cast<const char*>(path_buffer.data()), path_buffer.size()));
        if (!IsLikelyPackPath(path_str)) {
            cursor = abs_pos + 1;
            continue;
        }

        AddFileToTree(path_str, file_offset, data_len);
        cursor = encrypted ? (header_offset + 4 + container_len) : (file_offset + data_len);
    }
}
