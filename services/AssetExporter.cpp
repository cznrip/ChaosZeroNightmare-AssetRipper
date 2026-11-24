#include "services/AssetExporter.hpp"
#include "core/FileTree.hpp"
#include "core/Logger.hpp"
#include "core/StringUtils.hpp"

#include <format>
#include <fstream>

bool AssetExporter::WriteBinaryFile(const std::filesystem::path& path, const std::span<const uint8_t> data) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code error_code;
        std::filesystem::create_directories(parent, error_code);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        LogError("Failed to open file for writing: " + Core::PathToUtf8(path));
        return false;
    }

    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

void AssetExporter::ExtractAll(
    IArchive& archive,
    const std::filesystem::path& output_dir,
    const ExportOptions& options,
    const Core::ProgressReporter& progress,
    const FileExtractedCallback& on_file) const
{
    LogInfo("ExtractAll started");
    ExtractNode(archive, archive.GetFileTree(), output_dir, options, progress, on_file);
    LogInfo("ExtractAll finished");
}

void AssetExporter::ExtractNode(
    IArchive& archive,
    const Core::FileNode& node,
    const std::filesystem::path& output_dir,
    const ExportOptions& options,
    const Core::ProgressReporter& progress,
    const FileExtractedCallback& on_file) const
{
    const uint64_t total_bytes = Core::NodeTotalBytes(node);
    if (total_bytes == 0) {
        progress.Complete();
        return;
    }

    uint64_t extracted_bytes = 0;
    LogInfo("Extract begin for node: " + node.name);
    ExtractRecursive(archive, node, output_dir, extracted_bytes, total_bytes, options, progress, on_file);
    progress.Complete();
    LogInfo("Extract end for node: " + node.name);
}

void AssetExporter::ExtractNodes(
    IArchive& archive,
    const std::vector<const Core::FileNode*>& nodes,
    const std::filesystem::path& output_dir,
    const ExportOptions& options,
    const Core::ProgressReporter& progress,
    const FileExtractedCallback& on_file) const
{
    uint64_t total_bytes = 0;
    for (const auto* node : nodes) {
        if (node) total_bytes += Core::NodeTotalBytes(*node);
    }

    if (total_bytes == 0) {
        progress.Complete();
        return;
    }

    uint64_t extracted_bytes = 0;
    for (const auto* node : nodes) {
        if (!node) continue;
        ExtractRecursive(archive, *node, output_dir, extracted_bytes, total_bytes, options, progress, on_file);
    }
    progress.Complete();
}

bool AssetExporter::ExportSingleFile(
    IArchive& archive,
    const Core::FileNode& node,
    const std::filesystem::path& destination_file,
    const ExportOptions& options,
    bool apply_conversion) const
{
    const auto* info = std::get_if<Core::FileInfo>(&node.data);
    if (!info) return false;

    const std::vector<uint8_t> raw = archive.GetFileData(node);
    if (raw.empty()) return false;

    if (!apply_conversion) {
        return WriteBinaryFile(destination_file, raw);
    }

    const ConvertedAsset converted = m_converter.Convert(raw, node.name, info->format, options);
    if (converted.data.empty()) return false;
    return WriteBinaryFile(destination_file, converted.data);
}

void AssetExporter::ExtractRecursive(
    IArchive& archive,
    const Core::FileNode& node,
    const std::filesystem::path& current_dir,
    uint64_t& extracted_bytes,
    const uint64_t total_bytes,
    const ExportOptions& options,
    const Core::ProgressReporter& progress,
    const FileExtractedCallback& on_file) const
{
    try {
        if (const auto* file_info = std::get_if<Core::FileInfo>(&node.data)) {
            LogInfo(std::format("Extracting file: {} size={}", node.name, file_info->size));

            std::filesystem::path output_path = current_dir / Core::Utf8ToWString(node.name);
            const std::string target_ext = m_converter.ResolveExtension(file_info->format, options);
            if (target_ext != file_info->format) {
                output_path.replace_extension(Core::Utf8ToWString(target_ext));
            }

            if (const std::vector<uint8_t> raw = archive.GetFileData(node); !raw.empty()) {
                const ConvertedAsset asset = m_converter.Convert(raw, node.name, file_info->format, options);
                WriteBinaryFile(output_path, asset.data);
            }

            extracted_bytes += file_info->size;
            progress.Report(static_cast<float>(extracted_bytes) / static_cast<float>(total_bytes));
            if (on_file) {
                on_file(node);
            }
        } else if (const auto* folder = std::get_if<Core::FolderInfo>(&node.data)) {
            std::filesystem::path next_dir = current_dir;
            if (node.full_path != "/") {
                next_dir /= Core::Utf8ToWString(node.name);
            }
            for (const auto& child : folder->children) {
                ExtractRecursive(archive, child, next_dir, extracted_bytes, total_bytes, options, progress, on_file);
            }
        }
    } catch (const std::exception& e) {
        LogError(std::format("Error extracting node {}: {}", node.name, e.what()));
    }
}
