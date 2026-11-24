#pragma once

#include "archive/IArchive.hpp"
#include "converters/AssetConverter.hpp"
#include "core/Core.hpp"
#include "core/ProgressReporter.hpp"

#include <filesystem>
#include <functional>
#include <vector>

class AssetExporter {
public:
    using FileExtractedCallback = std::function<void(const Core::FileNode&)>;

    AssetExporter() = default;
    explicit AssetExporter(AssetConverter converter) : m_converter(std::move(converter)) {}

    [[nodiscard]] const AssetConverter& Converter() const noexcept { return m_converter; }

    void ExtractAll(
        IArchive& archive,
        const std::filesystem::path& output_dir,
        const ExportOptions& options,
        const Core::ProgressReporter& progress = Core::ProgressReporter::Null(),
        const FileExtractedCallback& on_file = nullptr) const;

    void ExtractNode(
        IArchive& archive,
        const Core::FileNode& node,
        const std::filesystem::path& output_dir,
        const ExportOptions& options,
        const Core::ProgressReporter& progress = Core::ProgressReporter::Null(),
        const FileExtractedCallback& on_file = nullptr) const;

    void ExtractNodes(
        IArchive& archive,
        const std::vector<const Core::FileNode*>& nodes,
        const std::filesystem::path& output_dir,
        const ExportOptions& options,
        const Core::ProgressReporter& progress = Core::ProgressReporter::Null(),
        const FileExtractedCallback& on_file = nullptr) const;

    bool ExportSingleFile(
        IArchive& archive,
        const Core::FileNode& node,
        const std::filesystem::path& destination_file,
        const ExportOptions& options,
        bool apply_conversion = true) const;

private:
    void ExtractRecursive(
        IArchive& archive,
        const Core::FileNode& node,
        const std::filesystem::path& current_dir,
        uint64_t& extracted_bytes,
        uint64_t total_bytes,
        const ExportOptions& options,
        const Core::ProgressReporter& progress,
        const FileExtractedCallback& on_file) const;

    static bool WriteBinaryFile(const std::filesystem::path& path, std::span<const uint8_t> data);

    AssetConverter m_converter;
};
