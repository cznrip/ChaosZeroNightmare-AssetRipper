#include "gui/FileBrowserPanel.hpp"
#include "gui/PreviewPanel.hpp"
#include "gui/UIHelpers.hpp"
#include "core/Core.hpp"
#include "core/FileDialogs.hpp"
#include "core/FileTree.hpp"
#include "services/FileMap.hpp"

#include "nuklear.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

    template <typename NodeT, typename SetT>
    void update_multi_selection(bool ctrl, SetT &selected_nodes, const NodeT *&selected_node, const NodeT *node)
    {
        if (ctrl)
        {
            if (auto it = selected_nodes.find(node); it != selected_nodes.end())
                selected_nodes.erase(it);
            else
                selected_nodes.insert(node);
        }
        else
        {
            selected_nodes.clear();
            selected_nodes.insert(node);
        }
        selected_node = node;
    }

    void handle_node_right_click(const Core::FileNode *node, struct nk_vec2 pos)
    {
        g_state.context_menu.node = node;
        g_state.context_menu.position = pos;
        g_state.context_menu.visible = true;
    }

    bool load_diff_tree_from_filemap()
    {
        try
        {
            const std::string picked_path = Core::FileDialogs::OpenFile(
                Core::DialogSlot::Export,
                "Select an older filemap.json",
                {"JSON Files", "*.json", "All Files", "*.*"});
            if (picked_path.empty()) return false;

            const auto old_json = Services::FileMapSerializer::LoadFromFile(Core::Utf8ToWString(picked_path));
            g_state.diff.root = Services::ArchiveDiff::Build(old_json, g_state.browser.data_pack->GetFileTree());
            g_state.diff.show_tree = true;
            g_state.diff.selection.expanded_folders.clear();
            g_state.diff.selection.expanded_folders.insert(g_state.diff.root.get());
            g_state.diff.selection.selected_node = nullptr;
            g_state.diff.selection.selected_nodes.clear();
            g_state.tasks.runner.SetStatus("Diff view generated.");
            return true;
        }
        catch (const std::exception &e)
        {
            g_state.tasks.runner.SetStatus("Error loading filemap: " + std::string(e.what()));
            g_state.common.error_message = e.what();
            g_state.common.show_error_popup = true;
            return false;
        }
    }

    std::string diff_display_name(const DiffNode &node)
    {
        switch (node.status)
        {
            case DiffStatus::Added:    return "[ADD] " + node.name;
            case DiffStatus::Modified: return "[MOD] " + node.name;
            case DiffStatus::Removed:  return "[DEL] " + node.name;
            default:                   return node.name;
        }
    }

    void handle_diff_node_click(const DiffNode *node, bool is_folder)
    {
        const bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
        const uint32_t current_time = SDL_GetTicks();

        if (current_time - g_state.browser.selection.last_click_time < 250 && node == g_state.diff.selection.last_clicked_node)
        {
            g_state.browser.selection.last_click_time = current_time;
            return;
        }

        update_multi_selection(ctrl_pressed, g_state.diff.selection.selected_nodes, g_state.diff.selection.selected_node, node);

        if (!is_folder && g_state.browser.data_pack)
        {
            if (const Core::FileNode *file_node = Core::FindNodeByPath(g_state.browser.data_pack->GetFileTree(), node->full_path))
            {
                load_preview(*file_node);
            }
            else
            {
                clear_preview();
                g_state.preview.error = "File not found in current archive (perhaps it was removed).";
            }
        }
        else
        {
            clear_preview();
        }

        g_state.browser.selection.last_click_time = current_time;
        g_state.diff.selection.last_clicked_node = node;
    }

    bool matches_diff_search(const DiffNode &node, const std::string &query_lower)
    {
        if (query_lower.empty()) return true;
        return Core::ToLower(node.name).find(query_lower) != std::string::npos;
    }

    bool has_matching_diff_child(const DiffNode &node, const std::string &query_lower)
    {
        if (query_lower.empty() || matches_diff_search(node, query_lower)) return true;
        if (node.is_folder)
        {
            for (const auto &child : node.children)
            {
                if (has_matching_diff_child(*child, query_lower)) return true;
            }
        }
        return false;
    }

    void draw_diff_node(nk_context *ctx, const DiffNode &node, const std::string &query_lower, int depth = 0)
    {
        try
        {
            if (node.is_folder)
            {
                if (!has_matching_diff_child(node, query_lower)) return;

                g_state.diff.selection.visible_nodes.push_back(&node);

                const bool is_expanded = g_state.diff.selection.expanded_folders.contains(&node);
                const bool is_selected = g_state.diff.selection.selected_nodes.contains(&node) ||
                                         (g_state.diff.selection.selected_node == &node);

                const std::string info = std::to_string(node.file_count) + " items | " + Core::FormatSize(node.size);
                const nk_color text_color = get_diff_status_color(node.status, is_selected, true);

                bool toggle_expand = false;
                if (draw_tree_row(ctx, depth, true, is_expanded, is_selected, diff_display_name(node), info, text_color, toggle_expand))
                {
                    handle_diff_node_click(&node, true);
                }

                if (toggle_expand)
                {
                    if (is_expanded) g_state.diff.selection.expanded_folders.erase(&node);
                    else g_state.diff.selection.expanded_folders.insert(&node);
                }

                if (is_expanded)
                {
                    for (const auto &child : node.children)
                        draw_diff_node(ctx, *child, query_lower, depth + 1);
                }
            }
            else
            {
                if (!matches_diff_search(node, query_lower)) return;

                g_state.diff.selection.visible_nodes.push_back(&node);

                const bool is_selected = g_state.diff.selection.selected_nodes.contains(&node) ||
                                         (g_state.diff.selection.selected_node == &node);
                const std::string size_str = Core::FormatSize(node.size) + " | " + node.format;
                const nk_color text_color = get_diff_status_color(node.status, is_selected, false);

                bool toggle_expand = false;
                if (draw_tree_row(ctx, depth, false, false, is_selected, diff_display_name(node), size_str, text_color, toggle_expand))
                {
                    handle_diff_node_click(&node, false);
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error drawing diff node: " << e.what() << std::endl;
        }
    }

    void draw_file_node(nk_context *ctx, const Core::FileNode &node, int depth = 0)
    {
        try
        {
            if (!has_matching_child(node, g_state.browser.search_query)) return;

            g_state.browser.selection.visible_nodes.push_back(&node);

            if (const auto *folder = std::get_if<Core::FolderInfo>(&node.data))
            {
                const bool is_expanded = g_state.browser.selection.expanded_folders.contains(&node);
                const bool is_selected = g_state.browser.selection.selected_nodes.contains(&node) ||
                                         (g_state.browser.selection.selected_node == &node);

                const bool highlight_match = !g_state.browser.search_query.empty() && matches_search(node, g_state.browser.search_query);
                const nk_color text_color = is_selected ? nk_rgb(255, 255, 255) : (highlight_match ? nk_rgb(100, 255, 100) : nk_rgb(220, 220, 220));

                const std::string info = std::to_string(folder->file_count) + " items | " + Core::FormatSize(folder->total_bytes);

                bool toggle_expand = false;
                if (draw_tree_row(ctx, depth, true, is_expanded, is_selected, node.name, info, text_color, toggle_expand))
                {
                    handle_node_click(&node, true);
                }

                if (toggle_expand)
                {
                    if (is_expanded) g_state.browser.selection.expanded_folders.erase(&node);
                    else g_state.browser.selection.expanded_folders.insert(&node);
                }

                if (is_expanded)
                {
                    for (const auto &child : folder->children)
                        draw_file_node(ctx, child, depth + 1);
                }
            }
            else
            {
                if (!matches_search(node, g_state.browser.search_query)) return;

                const auto &file_info = std::get<Core::FileInfo>(node.data);
                const bool is_selected = g_state.browser.selection.selected_nodes.contains(&node) ||
                                         (g_state.browser.selection.selected_node == &node);

                const nk_color text_color = is_selected ? nk_rgb(255, 255, 255) : nk_rgb(200, 200, 200);
                const std::string size_str = Core::FormatSize(file_info.size) + " | " + file_info.format;

                bool right_clicked = false;
                struct nk_vec2 mouse_pos = {0, 0};
                bool toggle_expand = false;
                if (draw_tree_row(ctx, depth, false, false, is_selected, node.name, size_str, text_color, toggle_expand, &right_clicked, &mouse_pos))
                {
                    handle_node_click(&node, false);
                }

                if (right_clicked)
                {
                    handle_node_right_click(&node, mouse_pos);
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error drawing node: " << e.what() << std::endl;
        }
    }

    template <typename NodeT>
    void scroll_tree_to_selected(nk_context *ctx, const SelectionState<NodeT> &selection, bool &scroll_to_selected)
    {
        if (!scroll_to_selected || !selection.selected_node) return;

        if (const auto it = std::ranges::find(selection.visible_nodes, selection.selected_node); it != selection.visible_nodes.end())
        {
            const int index = static_cast<int>(std::distance(selection.visible_nodes.begin(), it));
            nk_uint current_x = 0, current_y = 0;
            nk_group_get_scroll(ctx, "FileTree", &current_x, &current_y);

            const float row_height = 26.0f + ctx->style.window.spacing.y;
            const float node_y = index * row_height;
            const float view_h = nk_window_get_content_region(ctx).h;

            const float margin = row_height * 2.0f;
            const float visible_top = static_cast<float>(current_y) + margin;
            const float visible_bottom = static_cast<float>(current_y) + view_h - margin;

            if (node_y < visible_top)
            {
                const float target = (std::max)(0.0f, node_y - margin);
                nk_group_set_scroll(ctx, "FileTree", current_x, static_cast<nk_uint>(target));
            }
            else if (node_y + row_height > visible_bottom)
            {
                const float target = (std::max)(0.0f, node_y + row_height - view_h + margin);
                nk_group_set_scroll(ctx, "FileTree", current_x, static_cast<nk_uint>(target));
            }
        }
        scroll_to_selected = false;
    }

} // namespace

void handle_node_click(const Core::FileNode *node, bool is_folder)
{
    const bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    const uint32_t current_time = SDL_GetTicks();

    if (current_time - g_state.browser.selection.last_click_time < 250 && node == g_state.browser.selection.last_clicked_node)
    {
        g_state.browser.selection.last_click_time = current_time;
        return;
    }

    update_multi_selection(ctrl_pressed, g_state.browser.selection.selected_nodes, g_state.browser.selection.selected_node, node);

    if (!is_folder) load_preview(*node);
    else clear_preview();

    g_state.browser.selection.last_click_time = current_time;
    g_state.browser.selection.last_clicked_node = node;
}

void export_to_json()
{
    try
    {
        if (const std::string save_path = Core::FileDialogs::SaveFile("Export File Map", "filemap.json", {"JSON Files", "*.json", "All Files", "*.*"}); !save_path.empty())
        {
            Services::FileMapSerializer::SaveToFile(g_state.browser.data_pack->GetFileTree(), Core::Utf8ToWString(save_path));
            g_state.common.success_message = "File map exported successfully!";
            g_state.common.show_success_popup = true;
            g_state.tasks.runner.SetStatus("Exported to: " + save_path);
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.runner.SetStatus("Export error: " + std::string(e.what()));
    }
}

void set_file_tree_mode()
{
    g_state.spine.show_window = false;
    g_state.diff.show_tree = false;
}

void set_spine_viewer_mode()
{
    g_state.diff.show_tree = false;
    g_state.spine.show_window = true;
}

bool set_diff_viewer_mode()
{
    if (!g_state.diff.root && !load_diff_tree_from_filemap())
    {
        return false;
    }
    g_state.spine.show_window = false;
    g_state.diff.show_tree = true;
    return true;
}

void draw_file_browser_panel(nk_context *ctx, bool tree_scanned, bool &scroll_to_selected)
{
    if (nk_group_begin(ctx, "FileTree", NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        g_state.browser.selection.visible_nodes.clear();
        g_state.diff.selection.visible_nodes.clear();

        if (g_state.browser.data_pack && tree_scanned)
        {
            if (g_state.diff.show_tree && g_state.diff.root)
            {
                const std::string query_lower = Core::ToLower(g_state.browser.search_query);
                draw_diff_node(ctx, *g_state.diff.root, query_lower);
                scroll_tree_to_selected(ctx, g_state.diff.selection, scroll_to_selected);
            }
            else
            {
                draw_file_node(ctx, g_state.browser.data_pack->GetFileTree());
                scroll_tree_to_selected(ctx, g_state.browser.selection, scroll_to_selected);
            }
        }
        else if (g_state.browser.data_pack && g_state.tasks.runner.IsRunning())
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, "Scanning archive...", NK_TEXT_CENTERED);
        }
        else if (g_state.browser.data_pack)
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, "Click 'Scan Tree' to load files...", NK_TEXT_CENTERED);
        }
        else
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, "No pack file loaded.", NK_TEXT_CENTERED);
        }
        nk_group_end(ctx);
    }
}
