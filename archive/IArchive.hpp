#pragma once

#include "core/Core.hpp"
#include "core/ProgressReporter.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IArchive {
public:
    enum class PackType { Unknown, Encrypted, Decrypted, LocalDirectory, Ssra };

    virtual ~IArchive() = default;

    [[nodiscard]] virtual PackType GetType() const = 0;
    [[nodiscard]] virtual const Core::FileNode& GetFileTree() const = 0;
    [[nodiscard]] virtual std::wstring GetPackPath() const = 0;
    [[nodiscard]] virtual uint32_t GetParsedFileCount() const = 0;
    [[nodiscard]] virtual uint64_t GetParsedTotalSize() const = 0;

    virtual void Scan(Core::ProgressReporter progress) = 0;
    virtual void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) = 0;
    virtual void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) = 0;
    virtual std::vector<uint8_t> GetFileData(const Core::FileNode& node) = 0;
};
