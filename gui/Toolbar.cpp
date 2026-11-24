#include <future>
#include <string>
#include <vector>

#include <SDL.h>

#include "archive/ArchiveFactory.hpp"
#include "archive/IArchive.hpp"
#include "core/Core.hpp"
#include "core/FileDialogs.hpp"
#include "core/FileTree.hpp"
#include "gui/AppState.hpp"
#include "gui/FileBrowserPanel.hpp"
#include "gui/PreviewPanel.hpp"
#include "gui/Toolbar.hpp"
#include "services/AssetExporter.hpp"

#include "nuklear.h"

void draw_toolbar(nk_context *ctx)
{
    const bool pack_loaded = (g_state.browser.data_pack != nullptr);
    const bool tree_scanned = pack_loaded && g_state.tasks.scan_complete.load();
    const bool running = g_state.tasks.runner.IsRunning();
    const bool selection_exists = g_state.diff.show_tree
        ? (g_state.diff.selection.selected_node != nullptr)
        : (g_state.browser.selection.selected_node != nullptr);
    const bool has_file_selection = g_state.diff.show_tree
        ? !g_state.diff.selection.selected_nodes.empty()
        : !g_state.browser.selection.selected_nodes.empty();
    const bool has_extract_selection = has_file_selection || selection_exists;

    nk_layout_row_dynamic(ctx, 38, g_state.common.enable_open_folder ? 10 : 9);

    nk_style_button btn_style = ctx->style.button;
    btn_style.rounding = 4.0f;
    btn_style.padding = nk_vec2(10, 8);
    btn_style.normal = nk_style_item_color(nk_rgb(70, 70, 75));
    btn_style.hover = nk_style_item_color(nk_rgb(90, 90, 95));

    auto toolbar_button = [&](const char *label, bool enabled) -> bool {
        if (enabled) {
            return nk_button_label_styled(ctx, &btn_style, label) != 0;
        }
        nk_widget_disable_begin(ctx);
        nk_button_label_styled(ctx, &btn_style, label);
        nk_widget_disable_end(ctx);
        return false;
    };

    const bool can_open_pack = !running && !pack_loaded;

    if (toolbar_button("Open Pack", can_open_pack))
    {
        try
        {
            const std::string selected_path = Core::FileDialogs::OpenFile(
                Core::DialogSlot::Open,
                "Select an archive or manifest file",
                {"Pack / Manifest Files", "*.pack;*.ssra", "All Files", "*.*"});
            if (!selected_path.empty())
            {
                const std::wstring wpath = Core::Utf8ToWString(selected_path);
                reset_app_state();

                g_state.browser.data_pack = ArchiveFactory::Open(wpath);
                if (g_state.browser.data_pack->GetType() == IArchive::PackType::Unknown)
                {
                    g_state.tasks.runner.SetStatus("Error: Invalid or unknown file.");
                    g_state.browser.data_pack.reset();
                }
                else
                {
                    g_state.tasks.runner.SetStatus("Loaded. Click 'Scan Tree' to analyze contents.");
                }
            }
        }
        catch (const std::exception &e)
        {
            g_state.tasks.runner.SetStatus("Error opening file: " + std::string(e.what()));
        }
    }

    if (g_state.common.enable_open_folder)
    {
        if (toolbar_button("Open Folder", can_open_pack))
        {
            try
            {
                const std::string selected_path = Core::FileDialogs::SelectFolder(Core::DialogSlot::Open, "Select a folder to view");
                if (!selected_path.empty())
                {
                    const std::wstring wpath = Core::Utf8ToWString(selected_path);
                    reset_app_state();

                    g_state.browser.data_pack = ArchiveFactory::Open(wpath);
                    if (g_state.browser.data_pack->GetType() == IArchive::PackType::Unknown)
                    {
                        g_state.tasks.runner.SetStatus("Error: Invalid or unknown folder.");
                        g_state.browser.data_pack.reset();
                    }
                    else
                    {
                        g_state.tasks.runner.SetStatus("Folder Loaded. Click 'Scan Tree' to build the view.");
                    }
                }
            }
            catch (const std::exception &e)
            {
                g_state.tasks.runner.SetStatus("Error opening folder: " + std::string(e.what()));
            }
        }
    }

    if (toolbar_button("Scan Tree", pack_loaded && !tree_scanned && !running))
    {
        g_state.tasks.scan_complete = false;
        g_state.browser.selection.expanded_folders.clear();
        g_state.browser.selection.selected_node = nullptr;
        g_state.browser.selection.selected_nodes.clear();
        g_state.browser.selection.last_clicked_node = nullptr;
        clear_preview();

        g_state.tasks.runner.Start(
            Services::TaskKind::Scan,
            "Scanning...",
            [](Core::ProgressReporter reporter) {
                g_state.browser.data_pack->Scan(std::move(reporter));
            });
    }

    if (toolbar_button(g_state.spine.show_window ? "File Tree" : "Spine Viewer", tree_scanned && !running))
    {
        if (!g_state.spine.show_window)
        {
            if (!g_state.spine.catalog.IsBuilt() && !g_state.spine.building)
            {
                g_state.spine.building = true;
                g_state.spine.build_future = std::async(std::launch::async, []() {
                    try {
                        g_state.spine.catalog.Build(*g_state.browser.data_pack, g_state.browser.data_pack->GetFileTree());
                    } catch (...) {}
                    g_state.spine.building = false;
                });
            }
            set_spine_viewer_mode();
        }
        else
        {
            set_file_tree_mode();
        }
    }

    if (toolbar_button(g_state.diff.show_tree ? "File Tree" : "Diff Viewer", tree_scanned && !running))
    {
        if (g_state.diff.show_tree)
            set_file_tree_mode();
        else
            set_diff_viewer_mode();
    }

    if (toolbar_button("Extract All", tree_scanned && !g_state.diff.show_tree && !g_state.spine.show_window && !running))
    {
        try
        {
            if (const std::string dest_str = Core::FileDialogs::SelectFolder(Core::DialogSlot::Extract, "Select destination folder"); !dest_str.empty())
            {
                const std::filesystem::path dest_path(Core::Utf8ToWString(dest_str));
                const ExportOptions export_opts{
                    .convert_sct_to_png = (g_state.common.export_sct_as_png != 0),
                    .convert_db_to_json = (g_state.common.export_db_as_json != 0)
                };
                g_state.tasks.runner.Start(
                    Services::TaskKind::Extract,
                    "Extracting all files...",
                    [dest_path, export_opts](Core::ProgressReporter reporter) {
                        AssetExporter exporter;
                        exporter.ExtractAll(*g_state.browser.data_pack, dest_path, export_opts, reporter);
                    });
            }
        }
        catch (const std::exception &e)
        {
            g_state.tasks.runner.SetStatus("Error starting extraction: " + std::string(e.what()));
        }
    }

    if (toolbar_button("Extract Selected", tree_scanned && !g_state.spine.show_window && has_extract_selection && !running))
    {
        try
        {
            if (const std::string dest_str = Core::FileDialogs::SelectFolder(Core::DialogSlot::Extract, "Select destination folder"); !dest_str.empty())
            {
                const std::filesystem::path dest_path(Core::Utf8ToWString(dest_str));
                std::vector<const Core::FileNode *> nodes_to_extract;
                if (g_state.diff.show_tree)
                {
                    nodes_to_extract.reserve(g_state.diff.selection.selected_nodes.size() + 1);
                    for (const auto *n : g_state.diff.selection.selected_nodes)
                    {
                        if (n)
                        {
                            if (const Core::FileNode *fn = Core::FindNodeByPath(g_state.browser.data_pack->GetFileTree(), n->full_path))
                                nodes_to_extract.push_back(fn);
                        }
                    }
                    if (nodes_to_extract.empty() && g_state.diff.selection.selected_node)
                    {
                        if (const Core::FileNode *fn = Core::FindNodeByPath(g_state.browser.data_pack->GetFileTree(), g_state.diff.selection.selected_node->full_path))
                            nodes_to_extract.push_back(fn);
                    }
                }
                else
                {
                    nodes_to_extract.reserve(g_state.browser.selection.selected_nodes.size() + 1);
                    for (const auto *n : g_state.browser.selection.selected_nodes)
                    {
                        if (n) nodes_to_extract.push_back(n);
                    }
                    if (nodes_to_extract.empty() && g_state.browser.selection.selected_node)
                    {
                        nodes_to_extract.push_back(g_state.browser.selection.selected_node);
                    }
                }

                const ExportOptions export_opts{
                    .convert_sct_to_png = (g_state.common.export_sct_as_png != 0),
                    .convert_db_to_json = (g_state.common.export_db_as_json != 0)
                };
                g_state.tasks.runner.Start(
                    Services::TaskKind::Extract,
                    "Extracting " + std::to_string(nodes_to_extract.size()) + " item(s)...",
                    [dest_path, nodes = std::move(nodes_to_extract), export_opts](Core::ProgressReporter reporter) {
                        AssetExporter exporter;
                        exporter.ExtractNodes(*g_state.browser.data_pack, nodes, dest_path, export_opts, reporter);
                    });
            }
        }
        catch (const std::exception &e)
        {
            g_state.tasks.runner.SetStatus("Error starting extraction: " + std::string(e.what()));
        }
    }

    if (toolbar_button("Export filemap JSON", tree_scanned && !running))
    {
        export_to_json();
    }

    if (toolbar_button("Options", !running))
    {
        g_state.common.show_options = true;
    }

    if (toolbar_button("Credits", true))
    {
        g_state.credits.show_window = true;
    }
}
