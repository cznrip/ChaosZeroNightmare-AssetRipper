#include "archive/SsraArchive.hpp"
#include "core/BinaryReader.hpp"
#include "core/Compression.hpp"
#include "core/Logger.hpp"
#include "core/StringUtils.hpp"
#include "libs/zstd/zstd.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>

namespace {

    constexpr uint16_t kHotfixGroupId = 0xFFFF;
    constexpr size_t kHeaderSize = 64;
    constexpr size_t kChunkEntrySize = 32;
    constexpr size_t kFileEntrySize = 40;
    constexpr size_t kGroupEntrySize = 24;
    constexpr size_t kMaxOpenChunkStreams = 16;

    struct ManifestHeader {
        uint32_t version = 0;
        uint32_t unk0 = 0;
        uint32_t chunk_count = 0;
        uint32_t file_count = 0;
        uint32_t flags = 0;
        uint64_t string_table_offset = 0;
        uint64_t string_table_size = 0;
        uint64_t chunk_table_offset = 0;
        uint64_t file_table_offset = 0;
        uint64_t extra = 0;
    };

    bool ParseManifestHeader(Core::BinaryReader& reader, ManifestHeader& out) noexcept {
        if (!reader.HasBytes(kHeaderSize) || !reader.MatchMagic("SSRA")) {
            return false;
        }
        out.version = *reader.TryRead<uint32_t>();
        out.unk0 = *reader.TryRead<uint32_t>();
        out.chunk_count = *reader.TryRead<uint32_t>();
        out.file_count = *reader.TryRead<uint32_t>();
        out.flags = *reader.TryRead<uint32_t>();
        out.string_table_offset = *reader.TryRead<uint64_t>();
        out.string_table_size = *reader.TryRead<uint64_t>();
        out.chunk_table_offset = *reader.TryRead<uint64_t>();
        out.file_table_offset = *reader.TryRead<uint64_t>();
        out.extra = *reader.TryRead<uint64_t>();
        return true;
    }

    std::string FormatChunkName(
        uint16_t group_id,
        uint32_t chunk_id,
        const std::unordered_map<uint16_t, std::string>& group_names)
    {
        if (group_id == kHotfixGroupId) {
            return std::format("hotfix_{:04}.ssrc", chunk_id);
        }
        if (const auto it = group_names.find(group_id); it != group_names.end() && !it->second.empty()) {
            return std::format("{}_{:04}.ssrc", it->second, chunk_id);
        }
        return std::format("group_{}_{:04}.ssrc", group_id, chunk_id);
    }

} // namespace

SsraArchive::SsraArchive(const std::wstring& manifest_path)
    : m_manifest_path(manifest_path)
{
    pack_path = manifest_path;
    type = PackType::Ssra;
    const std::filesystem::path p(manifest_path);
    m_chunks_dir = (p.parent_path() / L"chunks").wstring();
}

const SsraArchive::ChunkInfo* SsraArchive::FindChunkForOffset(uint16_t group_id, uint64_t virtual_offset) const noexcept {
    const auto group_it = m_chunks_by_group.find(group_id);
    if (group_it == m_chunks_by_group.end() || group_it->second.empty()) {
        return nullptr;
    }

    const auto& group_chunks = group_it->second;
    // First chunk whose global_offset > virtual_offset
    const auto it = std::ranges::upper_bound(group_chunks, virtual_offset, {}, &ChunkInfo::global_offset);
    if (it == group_chunks.begin()) {
        return nullptr;
    }

    const ChunkInfo& candidate = *std::prev(it);
    if (virtual_offset >= candidate.global_offset &&
        virtual_offset < candidate.global_offset + candidate.size_bytes)
    {
        return &candidate;
    }
    return nullptr;
}

bool SsraArchive::IsRangeAvailable(uint16_t group_id, uint64_t virtual_offset, uint64_t length) const noexcept {
    const ChunkInfo* chunk = FindChunkForOffset(group_id, virtual_offset);
    return chunk != nullptr &&
           !chunk->physical_path.empty() &&
           (virtual_offset + length) <= (chunk->global_offset + chunk->size_bytes);
}

std::ifstream* SsraArchive::GetOrOpenChunkStream(const std::wstring& physical_path) {
    if (const auto it = m_chunk_streams.find(physical_path); it != m_chunk_streams.end()) {
        if (it->second.is_open()) {
            it->second.clear();
            return &it->second;
        }
        m_chunk_streams.erase(it);
    }

    if (m_chunk_streams.size() >= kMaxOpenChunkStreams) {
        m_chunk_streams.clear();
    }

    std::ifstream stream(physical_path, std::ios::binary);
    if (!stream.is_open()) {
        return nullptr;
    }

    auto [it, _] = m_chunk_streams.emplace(physical_path, std::move(stream));
    return &it->second;
}

void SsraArchive::Scan(Core::ProgressReporter progress) {
    BeginTreeBuild("/");
    m_group_names.clear();
    m_chunks_by_group.clear();
    m_file_map.clear();
    {
        std::lock_guard lock(m_stream_mutex);
        m_chunk_streams.clear();
    }

    LogInfo("Scanning SSRA manifest: " + Core::WStringToUtf8(m_manifest_path));

    std::ifstream file(m_manifest_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LogError("Failed to open manifest.ssra: " + Core::WStringToUtf8(m_manifest_path));
        return;
    }

    const std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (file_size < static_cast<std::streamsize>(kHeaderSize)) {
        LogError("manifest.ssra too small");
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(data.data()), file_size)) {
        LogError("Failed to read manifest.ssra");
        return;
    }

    Core::BinaryReader reader(data);
    ManifestHeader header{};
    if (!ParseManifestHeader(reader, header)) {
        LogError("Invalid SSRA header or magic");
        return;
    }

    // String table slice (zero-copy view into manifest buffer)
    std::span<const uint8_t> string_table;
    if (header.string_table_offset <= data.size() &&
        header.string_table_size <= data.size() - header.string_table_offset)
    {
        string_table = std::span<const uint8_t>(data).subspan(
            static_cast<size_t>(header.string_table_offset),
            static_cast<size_t>(header.string_table_size));
    }

    // Optional group table (GRPS) immediately following string table
    const uint64_t grps_offset = header.string_table_offset + header.string_table_size;
    if (grps_offset <= data.size() && data.size() - grps_offset >= 16 &&
        reader.MatchesAt(static_cast<size_t>(grps_offset), "GRPS"))
    {
        Core::BinaryReader grps_reader(data, static_cast<size_t>(grps_offset) + 4);
        const uint32_t group_count = *grps_reader.TryRead<uint32_t>();
        const uint32_t sec_str_size = *grps_reader.TryRead<uint32_t>();
        const uint64_t entries_offset = grps_offset + 16;
        const uint64_t sec_str_offset = entries_offset + static_cast<uint64_t>(group_count) * kGroupEntrySize;

        if (sec_str_offset <= data.size() && sec_str_size <= data.size() - sec_str_offset) {
            const auto sec_strings = std::span<const uint8_t>(data).subspan(
                static_cast<size_t>(sec_str_offset), sec_str_size);

            for (uint32_t g = 0; g < group_count; ++g) {
                Core::BinaryReader entry_reader(data, static_cast<size_t>(entries_offset + g * kGroupEntrySize));
                const uint16_t group_id = *entry_reader.TryRead<uint16_t>();
                entry_reader.Skip(2);
                const uint32_t name_off = *entry_reader.TryRead<uint32_t>();

                if (std::string gname = Core::BinaryReader::ReadCString(sec_strings, name_off); !gname.empty()) {
                    m_group_names[group_id] = gname;
                    LogInfo(std::format("Discovered SSRA group {} -> '{}'", group_id, gname));
                }
            }
        }
    }

    // Discover physical .ssrc files on disk once
    struct PhysicalChunkFile {
        std::filesystem::path path;
        std::string filename;
        uint64_t size = 0;
    };

    std::vector<PhysicalChunkFile> physical_files;
    std::unordered_map<uint64_t, std::vector<const PhysicalChunkFile*>> physical_by_size;
    {
        std::error_code ec;
        const std::filesystem::path chunks_path(m_chunks_dir);
        if (std::filesystem::is_directory(chunks_path, ec) && !ec) {
            for (const auto& entry : std::filesystem::directory_iterator(chunks_path, ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec) && entry.path().extension() == L".ssrc") {
                    const uint64_t sz = entry.file_size(ec);
                    if (!ec) {
                        physical_files.push_back({entry.path(), Core::PathToUtf8(entry.path().filename()), sz});
                    }
                }
            }
        }
        for (const auto& pf : physical_files) {
            physical_by_size[pf.size].push_back(&pf);
        }
    }

    // Parse chunk table and group by group_id
    for (uint32_t i = 0; i < header.chunk_count; ++i) {
        const uint64_t entry_off = header.chunk_table_offset + static_cast<uint64_t>(i) * kChunkEntrySize;
        if (entry_off > data.size() || data.size() - entry_off < kChunkEntrySize) break;

        Core::BinaryReader entry_reader(data, static_cast<size_t>(entry_off));
        ChunkInfo c_info;
        c_info.index = i;
        c_info.chunk_id = *entry_reader.TryRead<uint32_t>();
        c_info.group_id = *entry_reader.TryRead<uint16_t>();
        entry_reader.Skip(2); // unk_06
        c_info.size_bytes = *entry_reader.TryRead<uint64_t>();
        c_info.compressed_size = *entry_reader.TryRead<uint64_t>();
        // checksum (8 bytes) skipped

        c_info.name = FormatChunkName(c_info.group_id, c_info.chunk_id, m_group_names);

        // Match physical .ssrc file by candidate name + exact size, or unique size fallback
        std::vector<std::string> candidate_names = {c_info.name};
        if (const auto gn = m_group_names.find(c_info.group_id); gn != m_group_names.end() && !gn->second.empty()) {
            candidate_names.push_back(std::format("{}_b{:02}_0.ssrc", gn->second, c_info.chunk_id));
        }

        if (const auto by_size = physical_by_size.find(c_info.compressed_size); by_size != physical_by_size.end()) {
            for (const PhysicalChunkFile* pf : by_size->second) {
                if (std::ranges::find(candidate_names, pf->filename) != candidate_names.end()) {
                    c_info.physical_path = pf->path.wstring();
                    break;
                }
            }
            if (c_info.physical_path.empty() && by_size->second.size() == 1) {
                c_info.physical_path = by_size->second.front()->path.wstring();
            }
        }

        if (c_info.physical_path.empty()) {
            LogInfo("Skipping unavailable SSRA chunk: " + c_info.name);
        }

        m_chunks_by_group[c_info.group_id].push_back(std::move(c_info));
    }

    // Sort chunks per group by chunk_id and compute contiguous virtual global offsets
    size_t unavailable_chunk_count = 0;
    for (auto& [group_id, list] : m_chunks_by_group) {
        std::ranges::sort(list, {}, &ChunkInfo::chunk_id);
        uint64_t running_offset = 0;
        for (auto& chunk : list) {
            chunk.global_offset = running_offset;
            running_offset += chunk.size_bytes;
            if (chunk.physical_path.empty()) {
                ++unavailable_chunk_count;
            }
        }
    }
    if (unavailable_chunk_count != 0) {
        LogInfo("SSRA unavailable chunks skipped: " + std::to_string(unavailable_chunk_count));
    }

    // Parse file table using O(log C) chunk availability checks
    m_file_map.reserve(header.file_count);
    for (uint32_t i = 0; i < header.file_count; ++i) {
        const uint64_t entry_off = header.file_table_offset + static_cast<uint64_t>(i) * kFileEntrySize;
        if (entry_off > data.size() || data.size() - entry_off < kFileEntrySize) break;

        Core::BinaryReader entry_reader(data, static_cast<size_t>(entry_off));
        entry_reader.Skip(8); // path_hash + unk_04
        const uint64_t virtual_offset = *entry_reader.TryRead<uint64_t>();
        const uint32_t comp_sz = *entry_reader.TryRead<uint32_t>();
        const uint32_t uncomp_sz = *entry_reader.TryRead<uint32_t>();
        entry_reader.Skip(4); // unk_18
        const uint32_t name_off = *entry_reader.TryRead<uint32_t>();
        const uint8_t is_compressed = *entry_reader.TryRead<uint8_t>();
        const uint8_t is_encrypted = *entry_reader.TryRead<uint8_t>();
        const uint16_t group_id = *entry_reader.TryRead<uint16_t>();
        const uint32_t flags = *entry_reader.TryRead<uint32_t>();

        // Skip tombstoned/deleted entries
        if ((flags & 1u) != 0) continue;

        if (!IsRangeAvailable(group_id, virtual_offset, comp_sz)) continue;

        const std::string raw_name = Core::BinaryReader::ReadCString(string_table, name_off);
        const std::string clean_path = Core::NormalizeArchivePath(raw_name);
        if (clean_path.empty()) continue;

        m_file_map[clean_path] = FileEntry{
            .group_id = group_id,
            .virtual_offset = virtual_offset,
            .uncompressed_size = uncomp_sz,
            .compressed_size = comp_sz,
            .is_compressed = (is_compressed != 0),
            .is_encrypted = (is_encrypted != 0)
        };

        AddFileToTree(clean_path, virtual_offset, uncomp_sz);

        if (i % 100 == 0 || i + 1 == header.file_count) {
            progress.Report(static_cast<float>(i + 1) / static_cast<float>(header.file_count));
        }
    }

    FinalizeTree();
    progress.Complete();
}

std::vector<uint8_t> SsraArchive::GetFileData(const Core::FileNode& node) {
    if (!node.IsFile()) return {};

    const std::string norm_path = Core::NormalizeArchivePath(node.full_path);
    const auto it = m_file_map.find(norm_path);
    if (it == m_file_map.end()) {
        LogError("File not found in SSRA map: " + node.full_path);
        return {};
    }

    const FileEntry& entry = it->second;
    const size_t read_size = static_cast<size_t>(entry.is_compressed ? entry.compressed_size : entry.uncompressed_size);
    std::vector<uint8_t> buffer(read_size);

    uint64_t virtual_offset = entry.virtual_offset;
    size_t bytes_remaining = read_size;
    size_t buffer_offset = 0;

    {
        std::lock_guard lock(m_stream_mutex);
        while (bytes_remaining != 0) {
            const ChunkInfo* current_chunk = FindChunkForOffset(entry.group_id, virtual_offset);
            if (!current_chunk) {
                LogError(std::format(
                    "Failed to locate chunk for virtual offset {} in group {} ({})",
                    virtual_offset, entry.group_id, node.full_path));
                return {};
            }

            if (current_chunk->physical_path.empty()) {
                LogError(std::format("Unavailable chunk {} for file: {}", current_chunk->name, node.full_path));
                return {};
            }

            const uint64_t local_offset = virtual_offset - current_chunk->global_offset;
            const auto available = static_cast<size_t>(current_chunk->size_bytes - local_offset);
            const size_t segment_size = (std::min)(bytes_remaining, available);

            std::ifstream* stream = GetOrOpenChunkStream(current_chunk->physical_path);
            if (!stream) {
                LogError("Failed to open chunk file: " + Core::WStringToUtf8(current_chunk->physical_path));
                return {};
            }

            stream->seekg(static_cast<std::streamoff>(local_offset), std::ios::beg);
            if (!stream->read(reinterpret_cast<char*>(buffer.data() + buffer_offset), static_cast<std::streamsize>(segment_size))) {
                LogError(std::format("Failed to read data from chunk {} for file: {}", current_chunk->name, node.full_path));
                return {};
            }

            virtual_offset += segment_size;
            buffer_offset += segment_size;
            bytes_remaining -= segment_size;
        }
    }

    if (entry.is_encrypted) {
        LogError("Encrypted SSRA files are not yet supported for: " + node.full_path);
        return buffer;
    }

    if (entry.is_compressed) {
        const bool legacy_frame = Core::NormalizeLegacyZstdFrame(buffer);
        std::vector<uint8_t> decompressed(static_cast<size_t>(entry.uncompressed_size));
        const size_t d_size = ZSTD_decompress(decompressed.data(), decompressed.size(), buffer.data(), buffer.size());
        if (ZSTD_isError(d_size)) {
            LogError(std::format(
                "ZSTD decompression failed for {}{}: {}",
                node.full_path,
                legacy_frame ? " (legacy frame normalized)" : "",
                ZSTD_getErrorName(d_size)));
            return buffer;
        }
        decompressed.resize(d_size);
        return decompressed;
    }

    return buffer;
}
