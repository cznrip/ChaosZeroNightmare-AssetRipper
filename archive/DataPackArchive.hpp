#pragma once

#include "archive/ArchiveBase.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

/**
 * Archive implementation for encrypted or decrypted Yuna engine data.pack files
 * (including split parts data.pack~1, data.pack~2, ...), using RAII sliding memory-mapped views.
 */
class DataPackArchive final : public ArchiveBase {
public:
    explicit DataPackArchive(const std::wstring& path);
    ~DataPackArchive() override = default;

    DataPackArchive(const DataPackArchive&) = delete;
    DataPackArchive& operator=(const DataPackArchive&) = delete;

    void Scan(Core::ProgressReporter progress) override;
    std::vector<uint8_t> GetFileData(const Core::FileNode& node) override;

private:
    class ScopedWinHandle {
    public:
        ScopedWinHandle() = default;
        explicit ScopedWinHandle(HANDLE h) noexcept : m_handle(h) {}
        ~ScopedWinHandle() { Reset(); }

        ScopedWinHandle(const ScopedWinHandle&) = delete;
        ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

        ScopedWinHandle(ScopedWinHandle&& other) noexcept
            : m_handle(std::exchange(other.m_handle, nullptr)) {}

        ScopedWinHandle& operator=(ScopedWinHandle&& other) noexcept {
            if (this != &other) {
                Reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void Reset(HANDLE new_handle = nullptr) noexcept {
            if (IsValid()) {
                CloseHandle(m_handle);
            }
            m_handle = new_handle;
        }

        [[nodiscard]] bool IsValid() const noexcept {
            return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
        }

        [[nodiscard]] HANDLE Get() const noexcept { return m_handle; }

    private:
        HANDLE m_handle = nullptr;
    };

    class MappedFileView {
    public:
        MappedFileView() = default;
        ~MappedFileView() { Unmap(); }

        MappedFileView(const MappedFileView&) = delete;
        MappedFileView& operator=(const MappedFileView&) = delete;

        MappedFileView(MappedFileView&& other) noexcept
            : m_data(std::exchange(other.m_data, nullptr)),
              m_offset(std::exchange(other.m_offset, 0)),
              m_size(std::exchange(other.m_size, 0)) {}

        MappedFileView& operator=(MappedFileView&& other) noexcept {
            if (this != &other) {
                Unmap();
                m_data = std::exchange(other.m_data, nullptr);
                m_offset = std::exchange(other.m_offset, 0);
                m_size = std::exchange(other.m_size, 0);
            }
            return *this;
        }

        void Unmap() noexcept {
            if (m_data) {
                UnmapViewOfFile(m_data);
                m_data = nullptr;
                m_offset = 0;
                m_size = 0;
            }
        }

        void Assign(const uint8_t* ptr, uint64_t offset, size_t size) noexcept {
            Unmap();
            m_data = ptr;
            m_offset = offset;
            m_size = size;
        }

        [[nodiscard]] bool Covers(uint64_t offset, size_t needed) const noexcept {
            return m_data != nullptr &&
                   offset >= m_offset &&
                   (offset + needed) <= (m_offset + m_size);
        }

        [[nodiscard]] std::span<const uint8_t> SliceFrom(uint64_t offset, size_t max_bytes) const noexcept {
            const auto rel = static_cast<size_t>(offset - m_offset);
            const size_t avail = m_size - rel;
            return {m_data + rel, (std::min)(avail, max_bytes)};
        }

    private:
        const uint8_t* m_data = nullptr;
        uint64_t m_offset = 0;
        size_t m_size = 0;
    };

    struct PackPart {
        ScopedWinHandle file;
        ScopedWinHandle mapping;
        uint64_t file_size = 0;
        MappedFileView view;
    };

    static constexpr size_t kWindowSize = 64ULL * 1024 * 1024;

    void ScanPack(bool encrypted, const Core::ProgressReporter& progress);

    static std::vector<std::wstring> FindPackParts(const std::wstring& base_path);
    bool LoadPackPart(const std::wstring& path);

    bool EnsureWindow(PackPart& part, uint64_t offset, size_t needed) const;
    std::span<const uint8_t> GetViewAtOffset(uint64_t offset);
    size_t ReadBytes(uint64_t offset, std::span<uint8_t> dest);

    DWORD m_alloc_granularity = 65536;
    std::vector<PackPart> m_parts;
    uint64_t m_total_file_size = 0;
};
