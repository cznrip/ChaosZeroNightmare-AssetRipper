#pragma once

#include "gui/AppState.hpp"
#include "gui/FileBrowserPanel.hpp"
#include "gui/GlResources.hpp"
#include "gui/ImageWindow.hpp"
#include "gui/PopupManager.hpp"
#include "gui/PreviewManager.hpp"
#include "gui/PreviewPanel.hpp"
#include "gui/SpinePanel.hpp"
#include "gui/Toolbar.hpp"
#include "services/AssetExporter.hpp"

#include <GL/glew.h>
#include <SDL.h>

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run();

private:
    void InitSdl();
    void InitFonts();
    void ProcessEvents();
    void CheckTasks();
    void Render();
    void RenderSplitter(float& left_width, float right_width, float content_height);
    void RenderStatusBar();
    void Cleanup();

    SDL_Window* m_win = nullptr;
    SDL_GLContext m_gl_context = nullptr;
    nk_context* m_ctx = nullptr;
    Gfx::SdlCursorPtr m_resize_cursor;
    Gfx::SdlCursorPtr m_arrow_cursor;

    bool m_running = false;
    bool m_scroll_to_selected = false;
    int m_window_width = 0;
    int m_window_height = 0;

    // Owned application state, services, and OOP UI panels (no global g_state)
    AppState m_state;
    AssetExporter m_exporter;
    PreviewManager m_preview;
    ImageWindow m_image_window;
    FileBrowserPanel m_file_browser;
    PreviewPanel m_preview_panel;
    Toolbar m_toolbar;
    SpinePanel m_spine_panel;
    PopupManager m_popups;
};
