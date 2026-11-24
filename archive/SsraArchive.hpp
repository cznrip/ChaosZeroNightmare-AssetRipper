#pragma once

#include "archive/ArchiveBase.hpp"

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Standalone archive implementation for .ssra manifests and sibling chunks/*.ssrc files.
 * Pre-indexes chunks per group sorted by virtual offset for O(log C) lookups and caches
 * open chunk streams to avoid repeated file open/close overhead during extraction.
 */
class SsraArchive final : public ArchiveBase {
public:
    explicit SsraArchive(const std::wstring& manifest_path);
    ~SsraArchive() override = default;

    void Scan(Core::ProgressReporter progress) override;
    std::vector<uint8_t> GetFileData(const Core::FileNode& node) override;

private:
    struct ChunkInfo {
        uint64_t index = 0;
        uint32_t chunk_id = 0;
        uint16_t group_id = 0;
        std::string name;
        uint64_t size_bytes = 0;
        uint64_t compressed_size = 0;
        uint64_t global_offset = 0;
        std::wstring physical_path;
    };

    struct FileEntry {
        uint16_t group_id = 0;
        uint64_t virtual_offset = 0;
        uint64_t uncompressed_size = 0;
        uint64_t compressed_size = 0;
        bool is_compressed = false;
        bool is_encrypted = false;
    };

    [[nodiscard]] const ChunkInfo* FindChunkForOffset(uint16_t group_id, uint64_t virtual_offset) const noexcept;
    [[nodiscard]] bool IsRangeAvailable(uint16_t group_id, uint64_t virtual_offset, uint64_t length) const noexcept;
    std::ifstream* GetOrOpenChunkStream(const std::wstring& physical_path);

    std::wstring m_manifest_path;
    std::wstring m_chunks_dir;

    std::unordered_map<uint16_t, std::string> m_group_names;
    std::unordered_map<uint16_t, std::vector<ChunkInfo>> m_chunks_by_group;
    std::unordered_map<std::string, FileEntry> m_file_map;

    std::mutex m_stream_mutex;
    std::unordered_map<std::wstring, std::ifstream> m_chunk_streams;
};
