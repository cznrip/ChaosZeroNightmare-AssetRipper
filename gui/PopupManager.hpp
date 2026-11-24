#pragma once

#include "gui/AppState.hpp"
#include "gui/PreviewPanel.hpp"

/**
 * Manages modal popups (Options, Credits, Success/Error feedback) and file context menu.
 */
class PopupManager {
public:
    PopupManager(AppState& state, PreviewPanel& preview_panel);

    void RenderAll(nk_context* ctx, int window_width, int window_height);

private:
    void RenderOptionsPopup(nk_context* ctx, int window_width, int window_height);
    void RenderCreditsPopup(nk_context* ctx, int window_width, int window_height);
    void RenderFeedbackPopups(nk_context* ctx, int window_width, int window_height);
    void RenderContextMenu(nk_context* ctx);

    AppState& m_state;
    PreviewPanel& m_preview_panel;
};
