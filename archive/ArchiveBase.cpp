#include "archive/ArchiveBase.hpp"

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
