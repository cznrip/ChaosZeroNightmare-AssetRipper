#define NOMINMAX
#include "gui/App.hpp"
#include "core/AppInfo.hpp"
#include "core/FileTree.hpp"

#include <SDL_image.h>
#include <algorithm>
#include <format>
#include <fstream>

// Nuklear single-TU implementation
#define NK_IMPLEMENTATION
#define NK_SDL_GL3_IMPLEMENTATION
#include "nuklear.h"
#include "nuklear_sdl_gl3.h"

App::App()
    : m_file_browser(m_state, m_preview),
      m_preview_panel(m_state, m_preview, m_exporter, m_image_window),
      m_toolbar(m_state, m_exporter, m_preview, m_file_browser),
      m_spine_panel(m_state, m_exporter),
      m_popups(m_state, m_preview_panel) {}

App::~App() {
    Cleanup();
}

void App::InitSdl() {
    m_state.LoadOptionsFromSettings();

    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);

    m_resize_cursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE));
    m_arrow_cursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    m_win = SDL_CreateWindow(
        AppInfo::WindowTitle,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kInitialWindowWidth, kInitialWindowHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

    m_gl_context = SDL_GL_CreateContext(m_win);
    glewInit();

    m_ctx = nk_sdl_init(m_win);
    InitFonts();
}

void App::InitFonts() {
    struct nk_font_atlas* atlas = nullptr;
    nk_sdl_font_stash_begin(&atlas);

    struct nk_font_config config = nk_font_config(0);
    config.oversample_h = 1;
    config.oversample_v = 1;
    config.pixel_snap = 1;

    constexpr float kFontSize = 18.0f;
    struct nk_font* font = nullptr;
    bool font_loaded = false;

    const char* font_kr = R"(C:\Windows\Fonts\malgun.ttf)";
    if (std::ifstream(font_kr).good()) {
        config.range = nk_font_default_glyph_ranges();
        font = nk_font_atlas_add_from_file(atlas, font_kr, kFontSize, &config);
        config.merge_mode = nk_true;
        config.range = nk_font_korean_glyph_ranges();
        nk_font_atlas_add_from_file(atlas, font_kr, kFontSize, &config);
        font_loaded = true;
    }

    const char* font_cn = R"(C:\Windows\Fonts\msyh.ttc)";
    if (std::ifstream(font_cn).good()) {
        config.merge_mode = font_loaded ? nk_true : nk_false;
        if (!font_loaded) {
            config.range = nk_font_default_glyph_ranges();
            font = nk_font_atlas_add_from_file(atlas, font_cn, kFontSize, &config);
            config.merge_mode = nk_true;
            font_loaded = true;
        }
        config.range = nk_font_chinese_glyph_ranges();
        nk_font_atlas_add_from_file(atlas, font_cn, kFontSize, &config);
    }

    if (!font_loaded) {
        const char* font_base = R"(C:\Windows\Fonts\segoeui.ttf)";
        if (std::ifstream(font_base).good()) {
            config.merge_mode = nk_false;
            config.range = nk_font_default_glyph_ranges();
            font = nk_font_atlas_add_from_file(atlas, font_base, kFontSize, &config);
        }
    }

    nk_sdl_font_stash_end();
    if (font) {
        nk_style_set_font(m_ctx, &font->handle);
    }
}

void App::ProcessEvents() {
    SDL_Event evt{};
    nk_input_begin(m_ctx);
    while (SDL_PollEvent(&evt)) {
        if (evt.type == SDL_QUIT) {
            m_running = false;
        } else if (evt.type == SDL_WINDOWEVENT && evt.window.event == SDL_WINDOWEVENT_CLOSE) {
            const Uint32 win_id = evt.window.windowID;
            if (win_id == SDL_GetWindowID(m_win)) {
                m_running = false;
            } else if (m_image_window.MatchesWindowId(win_id)) {
                m_image_window.Close();
            }
        } else if (evt.type == SDL_KEYDOWN) {
            if (m_state.view_mode == ViewMode::SpineViewer) {
                m_spine_panel.HandleKeyDown(evt.key.keysym.sym);
            } else {
                m_file_browser.HandleKeyDown(evt.key.keysym.sym, m_scroll_to_selected);
            }
        }
        nk_sdl_handle_event(&evt);
    }
    nk_input_end(m_ctx);
}

void App::CheckTasks() {
    if (const auto completed = m_state.tasks.runner.PollCompletion()) {
        if (completed->succeeded) {
            if (completed->kind == Services::TaskKind::Scan && m_state.browser.archive) {
                m_state.tasks.scan_complete.store(true, std::memory_order_relaxed);
                ++m_state.browser.tree_revision;
                m_state.tasks.runner.SetStatus(std::format(
                    "Scan complete. {} files found.",
                    Core::NodeFileCount(m_state.browser.archive->GetFileTree())));
            } else if (completed->kind == Services::TaskKind::Extract) {
                m_state.tasks.runner.SetStatus("Extraction complete.");
            }
        }
    }
}

void App::RenderSplitter(float& left_width, float right_width, float content_height) {
    (void)left_width;
    nk_layout_row_push(m_ctx, 8.0f);
    const struct nk_rect bounds = nk_widget_bounds(m_ctx);
    nk_input* in = &m_ctx->input;

    const bool hovering = nk_input_is_mouse_hovering_rect(in, bounds) != 0;
    const bool mouse_down = nk_input_is_mouse_down(in, NK_BUTTON_LEFT) != 0;

    if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, bounds, nk_true)) {
        m_state.ui.dragging_splitter = true;
    }
    if (!mouse_down) {
        m_state.ui.dragging_splitter = false;
    }

    SDL_SetCursor((m_state.ui.dragging_splitter || hovering) ? m_resize_cursor.get() : m_arrow_cursor.get());

    if (m_state.ui.dragging_splitter) {
        m_state.ui.sidebar_width += in->mouse.delta.x;
    }

    nk_fill_rect(&m_ctx->current->buffer, bounds, 0, nk_rgb(40, 40, 45));
    nk_stroke_line(&m_ctx->current->buffer, bounds.x + 4, bounds.y + 10, bounds.x + 4, bounds.y + bounds.h - 10, 1.0f, nk_rgb(100, 100, 100));

    nk_layout_row_push(m_ctx, right_width - 8.0f);
    m_preview_panel.Render(m_ctx, right_width, content_height);
}

void App::RenderStatusBar() {
    nk_layout_row_dynamic(m_ctx, 4, 1);
    nk_spacing(m_ctx, 1);

    if (m_state.tasks.runner.IsRunning()) {
        nk_layout_row_begin(m_ctx, NK_STATIC, 22, 2);
        nk_layout_row_push(m_ctx, 220);
        const float cur_progress = std::clamp(m_state.tasks.runner.Progress(), 0.0f, 1.0f);
        nk_size prog_val = static_cast<nk_size>(cur_progress * 1000.0f);
        nk_progress(m_ctx, &prog_val, 1000, NK_FIXED);

        nk_layout_row_push(m_ctx, static_cast<float>(m_window_width) - 260.0f);
        const std::string status_text = std::format(
            "{} ({}%)", m_state.tasks.runner.Status(), static_cast<int>(cur_progress * 100.0f));
        nk_label_colored(m_ctx, status_text.c_str(), NK_TEXT_LEFT, nk_rgb(100, 200, 255));
        nk_layout_row_end(m_ctx);
        return;
    }

    nk_layout_row_dynamic(m_ctx, 22, 1);
    const Core::FileNode* sel = m_state.browser.selection.selected_node;
    if (m_state.view_mode == ViewMode::FileTree && sel && sel->IsFile()) {
        const auto& info = std::get<Core::FileInfo>(sel->data);
        const std::string details = std::format(
            "Selected: {} | Size: {} B | Offset: 0x{:X} | Format: {}",
            sel->name, info.size, info.offset, info.format);
        nk_label_colored(m_ctx, details.c_str(), NK_TEXT_LEFT, nk_rgb(180, 180, 180));
    } else {
        nk_label_colored(m_ctx, m_state.tasks.runner.Status().c_str(), NK_TEXT_LEFT, nk_rgb(160, 160, 160));
    }
}

void App::Render() {
    SDL_GetWindowSize(m_win, &m_window_width, &m_window_height);

    m_popups.RenderAll(m_ctx, m_window_width, m_window_height);

    if (nk_begin(m_ctx, "Main", nk_rect(0, 0, static_cast<float>(m_window_width), static_cast<float>(m_window_height)), NK_WINDOW_NO_SCROLLBAR)) {
        m_toolbar.Render(m_ctx);

        const bool tree_scanned = (m_state.browser.archive != nullptr) && m_state.tasks.scan_complete.load();
        const float content_height = static_cast<float>(m_window_height) - 130.0f;

        if (m_state.view_mode != ViewMode::SpineViewer) {
            nk_layout_row_begin(m_ctx, NK_STATIC, 30, 2);
            nk_layout_row_push(m_ctx, 80);
            nk_label(m_ctx, "Search:", NK_TEXT_LEFT);
            nk_layout_row_push(m_ctx, 300);
            nk_edit_string_zero_terminated(
                m_ctx, NK_EDIT_FIELD,
                m_state.browser.search_buffer.data(),
                static_cast<int>(m_state.browser.search_buffer.size()),
                nk_filter_default);
            m_state.browser.search_query = m_state.browser.search_buffer.data();
            nk_layout_row_end(m_ctx);

            const bool show_preview = m_preview.HasContent();
            const float max_sidebar = static_cast<float>(m_window_width) * 0.7f;
            m_state.ui.sidebar_width = std::clamp(m_state.ui.sidebar_width, 300.0f, (std::max)(300.0f, max_sidebar));

            float left_width = show_preview ? m_state.ui.sidebar_width : (static_cast<float>(m_window_width) - 20.0f);
            const float right_width = static_cast<float>(m_window_width) - left_width - 30.0f;

            nk_layout_row_begin(m_ctx, NK_STATIC, content_height, show_preview ? 3 : 1);
            nk_layout_row_push(m_ctx, left_width);
            m_file_browser.Render(m_ctx, tree_scanned, m_scroll_to_selected);

            if (show_preview) {
                RenderSplitter(left_width, right_width, content_height);
            }
            nk_layout_row_end(m_ctx);
        } else {
            m_spine_panel.Render(m_ctx, content_height, m_window_width);
        }

        RenderStatusBar();
    }
    nk_end(m_ctx);

    glViewport(0, 0, m_window_width, m_window_height);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
    m_image_window.Render();
    SDL_GL_SwapWindow(m_win);
}

void App::Cleanup() {
    m_state.tasks.runner.Wait();
    m_preview.Clear();
    m_state.ResetArchiveState();
    m_image_window.Close();
    m_resize_cursor.reset();
    m_arrow_cursor.reset();

    if (m_ctx) {
        nk_sdl_shutdown();
        m_ctx = nullptr;
    }
    if (m_gl_context) {
        SDL_GL_DeleteContext(m_gl_context);
        m_gl_context = nullptr;
    }
    if (m_win) {
        SDL_DestroyWindow(m_win);
        m_win = nullptr;
    }
    IMG_Quit();
    SDL_Quit();
}

int App::Run() {
    InitSdl();
    m_running = true;

    while (m_running) {
        m_scroll_to_selected = false;
        ProcessEvents();
        CheckTasks();
        Render();
    }
    return 0;
}
