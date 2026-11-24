#include "archive/ArchiveBase.hpp"
#include "core/AssetFormat.hpp"
#include "core/FileTree.hpp"
#include "core/Logger.hpp"
#include "parsers/DbParser.hpp"
#include "parsers/ScspParser.hpp"
#include "parsers/SctParser.hpp"

#include <filesystem>
#include <fstream>
#include <variant>

ArchiveBase::ArchiveBase() {
    root_node.name = "/";
    root_node.full_path = "/";
    root_node.data = Core::FolderInfo{};
}

void ArchiveBase::BeginTreeBuild(std::string root_name) {
    parsed_file_count.store(0, std::memory_order_relaxed);
    parsed_total_size.store(0, std::memory_order_relaxed);
    tree_builder.Reset(std::move(root_name));
}

void ArchiveBase::AddFileToTree(std::string_view path, uint64_t offset, uint64_t size) {
    tree_builder.AddFile(path, offset, size);
    parsed_file_count.store(tree_builder.FileCount(), std::memory_order_relaxed);
    parsed_total_size.store(tree_builder.TotalSize(), std::memory_order_relaxed);
}

void ArchiveBase::FinalizeTree() {
    root_node = tree_builder.Build();
    parsed_file_count.store(tree_builder.FileCount(), std::memory_order_relaxed);
    parsed_total_size.store(tree_builder.TotalSize(), std::memory_order_relaxed);
    tree_builder.Reset(root_node.name);
}

void ArchiveBase::ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, const bool convert_sct_to_png, const bool convert_db_to_json) {
    LogInfo("ExtractAll started");
    Extract(root_node, output_path, progress, convert_sct_to_png, convert_db_to_json);
    LogInfo("ExtractAll finished");
}

void ArchiveBase::Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, const bool convert_sct_to_png, const bool convert_db_to_json) {
    const uint64_t total_size_to_extract = Core::NodeTotalBytes(node);

    if (total_size_to_extract == 0) {
        progress = 1.0f;
        return;
    }
    std::atomic<uint64_t> extracted_size = 0;
    LogInfo("Extract begin for node");
    ExtractNode(node, output_path, extracted_size, total_size_to_extract, progress, convert_sct_to_png, convert_db_to_json);
    progress = 1.0f;
    LogInfo("Extract end for node");
}

void ArchiveBase::ExtractNode(const Core::FileNode& node, const std::wstring& current_path, std::atomic<uint64_t>& extracted_size, const uint64_t total_size, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) {
    try {
        if (const auto* info = std::get_if<Core::FileInfo>(&node.data)) {
            std::filesystem::path final_path = std::filesystem::path(current_path) / node.name;
            LogInfo(std::string("Extracting file: ") + node.name + " size=" + std::to_string(info->size));

            const Core::AssetType asset_type = Core::ClassifyExtension(info->format);
            const bool is_sct   = (asset_type == Core::AssetType::Sct);
            const bool is_db    = (asset_type == Core::AssetType::Database);
            const bool is_scsp  = (asset_type == Core::AssetType::Scsp);
            const bool is_atlas = (asset_type == Core::AssetType::Atlas);

            if (is_sct && convert_sct_to_png) final_path.replace_extension(".png");
            if (is_db && convert_db_to_json)  final_path.replace_extension(".json");
            if (is_scsp)                      final_path.replace_extension(".json");

            std::filesystem::create_directories(final_path.parent_path());

            std::vector<uint8_t> buffer = GetFileData(node);

            if (!buffer.empty()) {
                if (is_sct && convert_sct_to_png) {
                    try {
                        LogInfo(std::string("Converting SCT to PNG: ") + node.name);
                        if (std::vector<uint8_t> png_data = SctParser::ConvertToPng(buffer); !png_data.empty()) {
                            buffer = std::move(png_data);
                        }
                    } catch (const std::exception& e) {
                        LogError(std::string("SCT conversion failed for ") + node.name + ": " + e.what());
                    }
                } else if (is_atlas && convert_sct_to_png) {
                    try {
                        LogInfo(std::string("Rewriting atlas texture refs: ") + node.name);
                        const std::string rewritten = Core::RewriteAtlasExtensions(
                            std::string_view(reinterpret_cast<const char*>(buffer.data()), buffer.size()));
                        buffer.assign(rewritten.begin(), rewritten.end());
                    } catch (const std::exception& e) {
                        LogError(std::string("Atlas rewrite failed for ") + node.name + ": " + e.what());
                    }
                } else if (is_db && convert_db_to_json) {
                    try {
                        LogInfo(std::string("Converting DB to JSON: ") + node.name);
                        const std::string json_str = DbParser::ConvertToJson(buffer);
                        buffer.assign(json_str.begin(), json_str.end());
                    } catch (const std::exception& e) {
                        LogError(std::string("DB to JSON conversion failed for ") + node.name + ": " + e.what());
                    }
                } else if (is_scsp) {
                    try {
                        LogInfo(std::string("Converting SCSP to JSON: ") + node.name);
                        const std::string json_str = ScspParser::ConvertToJson(buffer);
                        buffer.assign(json_str.begin(), json_str.end());
                    } catch (const std::exception& e) {
                        LogError(std::string("SCSP to JSON conversion failed for ") + node.name + ": " + e.what());
                    }
                }

                if (std::ofstream out(final_path, std::ios::binary); out.is_open()) {
                    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                } else {
                    LogError(std::string("Failed to open file for writing: ") + Core::PathToUtf8(final_path));
                }
            }

            extracted_size.fetch_add(info->size, std::memory_order_relaxed);
            progress = static_cast<float>(extracted_size.load(std::memory_order_relaxed)) / static_cast<float>(total_size);
        } else if (const auto* folder_info = std::get_if<Core::FolderInfo>(&node.data)) {
            std::filesystem::path new_path = current_path;
            if (node.name != "/") {
                new_path /= node.name;
            }
            for (const auto& child : folder_info->children) {
                ExtractNode(child, new_path.wstring(), extracted_size, total_size, progress, convert_sct_to_png, convert_db_to_json);
            }
        }
    } catch (const std::exception& e) {
        LogError("Error extracting node: " + node.name + " - " + std::string(e.what()));
    }
}
