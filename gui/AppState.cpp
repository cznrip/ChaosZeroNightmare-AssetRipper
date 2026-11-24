#include "gui/AppState.hpp"
#include "core/AppSettings.hpp"
#include "parsers/SpineRenderer.hpp"

SpineViewerState::SpineViewerState() = default;
SpineViewerState::~SpineViewerState() = default;

AppState::AppState() = default;
AppState::~AppState() = default;

void AppState::LoadOptionsFromSettings() {
    const auto& opts = Core::AppSettings::Instance().Get();
    ui.export_sct_as_png = opts.export_sct_as_png;
    ui.export_db_as_json = opts.export_db_as_json;
    ui.enable_open_folder = opts.enable_open_folder;
}

void AppState::SaveOptionsToSettings() const {
    auto& opts = Core::AppSettings::Instance().Mutable();
    opts.export_sct_as_png = ui.export_sct_as_png;
    opts.export_db_as_json = ui.export_db_as_json;
    opts.enable_open_folder = ui.enable_open_folder;
    Core::AppSettings::Instance().Save();
}

void AppState::ResetArchiveState() {
    view_mode = ViewMode::FileTree;
    browser.archive.reset();
    tasks.scan_complete.store(false, std::memory_order_relaxed);
    browser.selection.Clear();
    browser.search_query.clear();
    browser.search_buffer.fill('\0');
    ++browser.tree_revision;

    if (spine.build_future.valid()) {
        spine.build_future.wait();
    }
    spine.catalog.Clear();
    spine.selected_index = -1;
    spine.search_buffer.fill('\0');
    spine.search_query.clear();
    spine.bone_search_buffer.fill('\0');
    spine.active_gizmo = 0;
    spine.export_pending = false;
    spine.bone_just_reset = false;
    spine.selected_bone.clear();
    if (spine.viewer) {
        spine.viewer->Unload();
    }
    spine.viewer.reset();
    spine.expanded_categories.clear();
    spine.collapsed_bones.clear();
    spine.visible_indices.clear();

    ui.dragging_splitter = false;
    context_menu.visible = false;
    context_menu.node = nullptr;

    diff.root.reset();
    diff.selection.Clear();
    ++diff.diff_revision;
}
