#pragma once

#include "archive/IArchive.hpp"
#include "core/Core.hpp"
#include "parsers/SpineCatalog.hpp"
#include "services/AsyncTaskRunner.hpp"
#include "services/FileMap.hpp"

#include <SDL.h>

#ifndef NK_NUKLEAR_H_
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#endif
#include "nuklear.h"

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class SpineViewer;

inline constexpr int kInitialWindowWidth = 1400;
inline constexpr int kInitialWindowHeight = 900;
inline constexpr uint32_t kDoubleClickDebounceMs = 250;

enum class ViewMode {
    FileTree,
    DiffTree,
    SpineViewer
};

template <typename NodeT>
struct SelectionState {
    const NodeT* selected_node = nullptr;
    const NodeT* last_clicked_node = nullptr;
    std::unordered_set<const NodeT*> selected_nodes;
    std::unordered_set<const NodeT*> expanded_folders;
    std::vector<const NodeT*> visible_nodes;
    uint32_t last_click_time = 0;

    void Clear() {
        selected_node = nullptr;
        last_clicked_node = nullptr;
        selected_nodes.clear();
        expanded_folders.clear();
        visible_nodes.clear();
        last_click_time = 0;
    }
};

struct FileBrowserState {
    std::unique_ptr<IArchive> archive;
    SelectionState<Core::FileNode> selection;
    std::array<char, 256> search_buffer{};
    std::string search_query;
    uint64_t tree_revision = 0;
};

struct TaskState {
    Services::AsyncTaskRunner runner;
    std::atomic<bool> scan_complete{false};
};

struct ContextMenuState {
    bool visible = false;
    const Core::FileNode* node = nullptr;
    struct nk_vec2 position{0.0f, 0.0f};
};

struct UiOptionsState {
    bool show_options = false;
    bool export_sct_as_png = true;
    bool export_db_as_json = true;
    bool enable_open_folder = false;
    bool show_credits = false;
    bool show_success_popup = false;
    std::string success_message;
    bool show_error_popup = false;
    std::string error_message;
    float sidebar_width = 600.0f;
    bool dragging_splitter = false;
};

struct SpineViewerState {
    SpineViewerState();
    ~SpineViewerState();
    SpineViewerState(const SpineViewerState&) = delete;
    SpineViewerState& operator=(const SpineViewerState&) = delete;

    SpineCatalog catalog;
    std::array<char, 256> search_buffer{};
    std::string search_query;
    int selected_index = -1;
    std::future<void> build_future;
    std::atomic<bool> building{false};
    std::unique_ptr<SpineViewer> viewer;
    int selected_animation = 0;
    int selected_skin = 0;
    float speed = 1.0f;
    float zoom = 1.0f;
    bool playing = true;
    bool flip_x = false;
    bool flip_y = false;
    Uint64 last_tick = 0;
    std::unordered_set<std::string> expanded_categories;
    std::unordered_set<std::string> collapsed_bones;
    std::vector<int> visible_indices;
    bool edit_mode = false;
    std::string selected_bone;
    bool scroll_to_bone = false;
    bool autoplay = false;
    bool pma_blend = true;
    int bg_preset = 0;
    int active_gizmo = 0;
    bool export_pending = false;
    std::array<char, 128> bone_search_buffer{};
    bool bone_just_reset = false;
    bool link_scale = true;
};

struct DiffViewerState {
    std::unique_ptr<DiffNode> root;
    SelectionState<DiffNode> selection;
    uint64_t diff_revision = 0;
};

struct AppState {
    AppState();
    ~AppState();

    ViewMode view_mode = ViewMode::FileTree;
    FileBrowserState browser;
    TaskState tasks;
    ContextMenuState context_menu;
    UiOptionsState ui;
    SpineViewerState spine;
    DiffViewerState diff;

    void LoadOptionsFromSettings();
    void SaveOptionsToSettings() const;
    void ResetArchiveState();
};
