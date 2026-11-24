#include "gui/FileBrowserPanel.hpp"
#include "core/FileDialogs.hpp"
#include "core/FileTree.hpp"
#include "core/StringUtils.hpp"
#include "gui/UiStyle.hpp"
#include "services/FileMap.hpp"

#include <algorithm>
#include <format>
#include <iostream>

namespace {

    template <typename NodeT, typename SetT>
    void UpdateMultiSelection(bool ctrl, SetT& selected_nodes, const NodeT*& selected_node, const NodeT* node) {
        if (ctrl) {
            if (auto it = selected_nodes.find(node); it != selected_nodes.end()) {
                selected_nodes.erase(it);
            } else {
                selected_nodes.insert(node);
            }
        } else {
            selected_nodes.clear();
            selected_nodes.insert(node);
        }
        selected_node = node;
    }

    template <typename NodeT>
    void ScrollTreeToSelected(nk_context* ctx, const SelectionState<NodeT>& selection, bool& scroll_to_selected) {
        if (!scroll_to_selected || !selection.selected_node) return;

        if (const auto it = std::ranges::find(selection.visible_nodes, selection.selected_node);
            it != selection.visible_nodes.end())
        {
            const int index = static_cast<int>(std::distance(selection.visible_nodes.begin(), it));
            nk_uint current_x = 0, current_y = 0;
            nk_group_get_scroll(ctx, "FileTree", &current_x, &current_y);

            const float row_height = 26.0f + ctx->style.window.spacing.y;
            const float node_y = static_cast<float>(index) * row_height;
            const float view_h = nk_window_get_content_region(ctx).h;

            const float margin = row_height * 2.0f;
            const float visible_top = static_cast<float>(current_y) + margin;
            const float visible_bottom = static_cast<float>(current_y) + view_h - margin;

            if (node_y < visible_top) {
                const float target = (std::max)(0.0f, node_y - margin);
                nk_group_set_scroll(ctx, "FileTree", current_x, static_cast<nk_uint>(target));
            } else if (node_y + row_height > visible_bottom) {
                const float target = (std::max)(0.0f, node_y + row_height - view_h + margin);
                nk_group_set_scroll(ctx, "FileTree", current_x, static_cast<nk_uint>(target));
            }
        }
        scroll_to_selected = false;
    }

    std::string FormatDiffDisplayName(const DiffNode& node) {
        switch (node.status) {
            case DiffStatus::Added:    return "[ADD] " + node.name;
            case DiffStatus::Modified: return "[MOD] " + node.name;
            case DiffStatus::Removed:  return "[DEL] " + node.name;
            default:                   return node.name;
        }
    }

    bool CollectMatchingFileNodes(
        const Core::FileNode& node,
        const std::string& query_lower,
        std::unordered_set<const Core::FileNode*>& visible_out,
        std::unordered_set<const Core::FileNode*>& direct_out)
    {
        const bool self_matches = Core::ToLower(node.name).find(query_lower) != std::string::npos;
        if (self_matches) direct_out.insert(&node);

        bool any_child_matches = false;
        if (const auto* folder = std::get_if<Core::FolderInfo>(&node.data)) {
            for (const auto& child : folder->children) {
                if (CollectMatchingFileNodes(child, query_lower, visible_out, direct_out)) {
                    any_child_matches = true;
                }
            }
        }

        if (self_matches || any_child_matches) {
            visible_out.insert(&node);
            return true;
        }
        return false;
    }

    bool CollectMatchingDiffNodes(
        const DiffNode& node,
        const std::string& query_lower,
        std::unordered_set<const DiffNode*>& visible_out)
    {
        const bool self_matches = Core::ToLower(node.name).find(query_lower) != std::string::npos;
        bool any_child_matches = false;
        if (node.is_folder) {
            for (const auto& child : node.children) {
                if (CollectMatchingDiffNodes(*child, query_lower, visible_out)) {
                    any_child_matches = true;
                }
            }
        }
        if (self_matches || any_child_matches) {
            visible_out.insert(&node);
            return true;
        }
        return false;
    }

} // namespace

FileBrowserPanel::FileBrowserPanel(AppState& state, PreviewManager& preview)
    : m_state(state), m_preview(preview) {}

void FileBrowserPanel::EnsureFileFilterCache() {
    if (!m_state.browser.archive) return;
    if (m_cached_file_query == m_state.browser.search_query &&
        m_cached_file_revision == m_state.browser.tree_revision)
    {
        return;
    }

    m_cached_file_query = m_state.browser.search_query;
    m_cached_file_revision = m_state.browser.tree_revision;
    m_file_visible_set.clear();
    m_file_direct_match_set.clear();

    if (!m_cached_file_query.empty()) {
        const std::string query_lower = Core::ToLower(m_cached_file_query);
        CollectMatchingFileNodes(
            m_state.browser.archive->GetFileTree(),
            query_lower,
            m_file_visible_set,
            m_file_direct_match_set);
    }
}

void FileBrowserPanel::EnsureDiffFilterCache() {
    if (!m_state.diff.root) return;
    if (m_cached_diff_query == m_state.browser.search_query &&
        m_cached_diff_revision == m_state.diff.diff_revision)
    {
        return;
    }

    m_cached_diff_query = m_state.browser.search_query;
    m_cached_diff_revision = m_state.diff.diff_revision;
    m_diff_visible_set.clear();

    if (!m_cached_diff_query.empty()) {
        const std::string query_lower = Core::ToLower(m_cached_diff_query);
        CollectMatchingDiffNodes(*m_state.diff.root, query_lower, m_diff_visible_set);
    }
}

void FileBrowserPanel::HandleFileNodeClick(const Core::FileNode* node, bool is_folder) {
    const bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    const uint32_t now = SDL_GetTicks();

    if (now - m_state.browser.selection.last_click_time < kDoubleClickDebounceMs &&
        node == m_state.browser.selection.last_clicked_node)
    {
        m_state.browser.selection.last_click_time = now;
        return;
    }

    UpdateMultiSelection(ctrl_pressed, m_state.browser.selection.selected_nodes, m_state.browser.selection.selected_node, node);

    if (!is_folder && m_state.browser.archive) {
        m_preview.Load(*m_state.browser.archive, *node);
    } else {
        m_preview.Clear();
    }

    m_state.browser.selection.last_click_time = now;
    m_state.browser.selection.last_clicked_node = node;
}

void FileBrowserPanel::HandleDiffNodeClick(const DiffNode* node, bool is_folder) {
    const bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    const uint32_t now = SDL_GetTicks();

    if (now - m_state.diff.selection.last_click_time < kDoubleClickDebounceMs &&
        node == m_state.diff.selection.last_clicked_node)
    {
        m_state.diff.selection.last_click_time = now;
        return;
    }

    UpdateMultiSelection(ctrl_pressed, m_state.diff.selection.selected_nodes, m_state.diff.selection.selected_node, node);

    if (!is_folder && m_state.browser.archive) {
        if (const Core::FileNode* file_node = Core::FindNodeByPath(m_state.browser.archive->GetFileTree(), node->full_path)) {
            m_preview.Load(*m_state.browser.archive, *file_node);
        } else {
            m_preview.SetError("File not found in current archive (perhaps it was removed).");
        }
    } else {
        m_preview.Clear();
    }

    m_state.diff.selection.last_click_time = now;
    m_state.diff.selection.last_clicked_node = node;
}

void FileBrowserPanel::HandleKeyDown(SDL_Keycode key, bool& scroll_to_selected) {
    if (m_state.view_mode != ViewMode::FileTree) return;
    auto& sel = m_state.browser.selection;
    if (!sel.selected_node) return;

    if ((key == SDLK_UP || key == SDLK_DOWN) && !sel.visible_nodes.empty()) {
        const auto it = std::ranges::find(sel.visible_nodes, sel.selected_node);
        if (it == sel.visible_nodes.end()) return;

        if (key == SDLK_UP && it != sel.visible_nodes.begin()) {
            const Core::FileNode* next = *std::prev(it);
            HandleFileNodeClick(next, next->IsFolder());
            scroll_to_selected = true;
        } else if (key == SDLK_DOWN && std::next(it) != sel.visible_nodes.end()) {
            const Core::FileNode* next = *std::next(it);
            HandleFileNodeClick(next, next->IsFolder());
            scroll_to_selected = true;
        }
    } else if (sel.selected_node->IsFolder()) {
        if (key == SDLK_RETURN) {
            if (sel.expanded_folders.contains(sel.selected_node)) {
                sel.expanded_folders.erase(sel.selected_node);
            } else {
                sel.expanded_folders.insert(sel.selected_node);
            }
        } else if (key == SDLK_RIGHT) {
            sel.expanded_folders.insert(sel.selected_node);
        } else if (key == SDLK_LEFT) {
            sel.expanded_folders.erase(sel.selected_node);
        }
    }
}

void FileBrowserPanel::DrawFileNode(nk_context* ctx, const Core::FileNode& node, int depth) {
    const bool filtering = !m_cached_file_query.empty();
    if (filtering && !m_file_visible_set.contains(&node)) return;

    auto& sel = m_state.browser.selection;
    sel.visible_nodes.push_back(&node);

    if (const auto* folder = std::get_if<Core::FolderInfo>(&node.data)) {
        const bool is_expanded = sel.expanded_folders.contains(&node);
        const bool is_selected = sel.selected_nodes.contains(&node) || (sel.selected_node == &node);
        const bool highlight = filtering && m_file_direct_match_set.contains(&node);
        const nk_color color = is_selected ? nk_rgb(255, 255, 255)
                             : highlight   ? nk_rgb(100, 255, 100)
                                           : nk_rgb(220, 220, 220);

        const std::string info = std::format("{} items | {}", folder->file_count, Core::FormatSize(folder->total_bytes));

        bool toggle_expand = false;
        if (UiStyle::DrawTreeRow(ctx, depth, true, is_expanded, is_selected, node.name, info, color, toggle_expand)) {
            HandleFileNodeClick(&node, true);
        }
        if (toggle_expand) {
            if (is_expanded) sel.expanded_folders.erase(&node);
            else sel.expanded_folders.insert(&node);
        }
        if (is_expanded) {
            for (const auto& child : folder->children) {
                DrawFileNode(ctx, child, depth + 1);
            }
        }
    } else {
        const auto& file_info = std::get<Core::FileInfo>(node.data);
        const bool is_selected = sel.selected_nodes.contains(&node) || (sel.selected_node == &node);
        const nk_color color = is_selected ? nk_rgb(255, 255, 255) : nk_rgb(200, 200, 200);
        const std::string info = Core::FormatSize(file_info.size) + " | " + file_info.format;

        bool right_clicked = false;
        struct nk_vec2 mouse_pos{0.0f, 0.0f};
        bool toggle_expand = false;
        if (UiStyle::DrawTreeRow(ctx, depth, false, false, is_selected, node.name, info, color, toggle_expand, &right_clicked, &mouse_pos)) {
            HandleFileNodeClick(&node, false);
        }
        if (right_clicked) {
            m_state.context_menu.node = &node;
            m_state.context_menu.position = mouse_pos;
            m_state.context_menu.visible = true;
        }
    }
}

void FileBrowserPanel::DrawDiffNode(nk_context* ctx, const DiffNode& node, int depth) {
    const bool filtering = !m_cached_diff_query.empty();
    if (filtering && !m_diff_visible_set.contains(&node)) return;

    auto& sel = m_state.diff.selection;
    sel.visible_nodes.push_back(&node);

    const bool is_selected = sel.selected_nodes.contains(&node) || (sel.selected_node == &node);
    const nk_color color = UiStyle::GetDiffStatusColor(node.status, is_selected, node.is_folder);

    if (node.is_folder) {
        const bool is_expanded = sel.expanded_folders.contains(&node);
        const std::string info = std::format("{} items | {}", node.file_count, Core::FormatSize(node.size));

        bool toggle_expand = false;
        if (UiStyle::DrawTreeRow(ctx, depth, true, is_expanded, is_selected, FormatDiffDisplayName(node), info, color, toggle_expand)) {
            HandleDiffNodeClick(&node, true);
        }
        if (toggle_expand) {
            if (is_expanded) sel.expanded_folders.erase(&node);
            else sel.expanded_folders.insert(&node);
        }
        if (is_expanded) {
            for (const auto& child : node.children) {
                DrawDiffNode(ctx, *child, depth + 1);
            }
        }
    } else {
        const std::string info = Core::FormatSize(node.size) + " | " + node.format;
        bool toggle_expand = false;
        if (UiStyle::DrawTreeRow(ctx, depth, false, false, is_selected, FormatDiffDisplayName(node), info, color, toggle_expand)) {
            HandleDiffNodeClick(&node, false);
        }
    }
}

bool FileBrowserPanel::LoadDiffTreeFromFileMap() {
    try {
        const std::string picked_path = Core::FileDialogs::OpenFile(
            Core::DialogSlot::Export,
            "Select an older filemap.json",
            {"JSON Files", "*.json", "All Files", "*.*"});
        if (picked_path.empty()) return false;

        const auto old_json = Services::FileMapSerializer::LoadFromFile(Core::Utf8ToWString(picked_path));
        m_state.diff.root = Services::ArchiveDiff::Build(old_json, m_state.browser.archive->GetFileTree());
        m_state.diff.selection.Clear();
        m_state.diff.selection.expanded_folders.insert(m_state.diff.root.get());
        ++m_state.diff.diff_revision;
        m_state.tasks.runner.SetStatus("Diff view generated.");
        return true;
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Error loading filemap: " + std::string(e.what()));
        m_state.ui.error_message = e.what();
        m_state.ui.show_error_popup = true;
        return false;
    }
}

void FileBrowserPanel::ExportFileMapJson() {
    if (!m_state.browser.archive) return;
    try {
        const std::string save_path = Core::FileDialogs::SaveFile(
            "Export File Map", "filemap.json", {"JSON Files", "*.json", "All Files", "*.*"});
        if (save_path.empty()) return;

        Services::FileMapSerializer::SaveToFile(m_state.browser.archive->GetFileTree(), Core::Utf8ToWString(save_path));
        m_state.ui.success_message = "File map exported successfully!";
        m_state.ui.show_success_popup = true;
        m_state.tasks.runner.SetStatus("Exported to: " + save_path);
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Export error: " + std::string(e.what()));
    }
}

void FileBrowserPanel::SetFileTreeMode() {
    m_state.view_mode = ViewMode::FileTree;
}

void FileBrowserPanel::SetSpineViewerMode() {
    m_state.view_mode = ViewMode::SpineViewer;
}

bool FileBrowserPanel::SetDiffViewerMode() {
    if (!m_state.diff.root && !LoadDiffTreeFromFileMap()) {
        return false;
    }
    m_state.view_mode = ViewMode::DiffTree;
    return true;
}

void FileBrowserPanel::Render(nk_context* ctx, bool tree_scanned, bool& scroll_to_selected) {
    if (!nk_group_begin(ctx, "FileTree", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        return;
    }

    m_state.browser.selection.visible_nodes.clear();
    m_state.diff.selection.visible_nodes.clear();

    if (m_state.browser.archive && tree_scanned) {
        if (m_state.view_mode == ViewMode::DiffTree && m_state.diff.root) {
            EnsureDiffFilterCache();
            DrawDiffNode(ctx, *m_state.diff.root);
            ScrollTreeToSelected(ctx, m_state.diff.selection, scroll_to_selected);
        } else {
            EnsureFileFilterCache();
            DrawFileNode(ctx, m_state.browser.archive->GetFileTree());
            ScrollTreeToSelected(ctx, m_state.browser.selection, scroll_to_selected);
        }
    } else if (m_state.browser.archive && m_state.tasks.runner.IsRunning()) {
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, "Scanning archive...", NK_TEXT_CENTERED);
    } else if (m_state.browser.archive) {
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, "Click 'Scan Tree' to load files...", NK_TEXT_CENTERED);
    } else {
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, "No pack file loaded.", NK_TEXT_CENTERED);
    }

    nk_group_end(ctx);
}
