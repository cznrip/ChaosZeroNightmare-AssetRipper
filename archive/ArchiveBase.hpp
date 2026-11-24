#pragma once

#include "archive/IArchive.hpp"
#include "core/FileTree.hpp"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

class ArchiveBase : public IArchive {
public:
    ArchiveBase();
    ~ArchiveBase() override = default;

    [[nodiscard]] PackType GetType() const override { return type; }
    [[nodiscard]] const Core::FileNode& GetFileTree() const override { return root_node; }
    [[nodiscard]] std::wstring GetPackPath() const override { return pack_path; }
    [[nodiscard]] uint32_t GetParsedFileCount() const override { return parsed_file_count.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t GetParsedTotalSize() const override { return parsed_total_size.load(std::memory_order_relaxed); }

    void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) override;
    void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) override;

    void Scan(Core::ProgressReporter progress) override = 0;
    std::vector<uint8_t> GetFileData(const Core::FileNode& node) override = 0;

protected:
    void BeginTreeBuild(std::string root_name = "/");
    void AddFileToTree(std::string_view path, uint64_t offset, uint64_t size);
    void FinalizeTree();

    void ExtractNode(const Core::FileNode& node, const std::wstring& current_path, std::atomic<uint64_t>& extracted_size, uint64_t total_size, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json);

    std::wstring pack_path;
    std::atomic<uint32_t> parsed_file_count{0};
    std::atomic<uint64_t> parsed_total_size{0};
    PackType type{PackType::Unknown};
    Core::FileNode root_node;
    Core::FileTreeBuilder tree_builder;
};
