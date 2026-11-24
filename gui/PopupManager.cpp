#include "gui/PopupManager.hpp"
#include "core/AppInfo.hpp"
#include "core/AssetFormat.hpp"
#include "gui/UiStyle.hpp"

PopupManager::PopupManager(AppState& state, PreviewPanel& preview_panel)
    : m_state(state), m_preview_panel(preview_panel) {}

void PopupManager::RenderAll(nk_context* ctx, int window_width, int window_height) {
    RenderContextMenu(ctx);
    RenderOptionsPopup(ctx, window_width, window_height);
    RenderCreditsPopup(ctx, window_width, window_height);
    RenderFeedbackPopups(ctx, window_width, window_height);
}

void PopupManager::RenderOptionsPopup(nk_context* ctx, int window_width, int window_height) {
    if (!m_state.ui.show_options) return;

    constexpr float kWidth = 530.0f;
    constexpr float kHeight = 460.0f;
    const float x = (static_cast<float>(window_width) - kWidth) * 0.5f;
    const float y = (static_cast<float>(window_height) - kHeight) * 0.5f;

    if (nk_begin(ctx, "Export Options", nk_rect(x, y, kWidth, kHeight),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE))
    {
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, "Configure extraction options:", NK_TEXT_LEFT);

        auto draw_option = [&](const char* label, bool& value, const char* desc1, const char* desc2) {
            nk_layout_row_begin(ctx, NK_STATIC, 32, 2);
            nk_layout_row_push(ctx, 380);
            nk_label(ctx, label, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 120);
            const nk_style_button style = UiStyle::MakeToggleStyle(ctx, value);
            if (nk_button_label_styled(ctx, &style, value ? "ON" : "OFF")) {
                value = !value;
                m_state.SaveOptionsToSettings();
            }
            nk_layout_row_end(ctx);

            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, desc1, NK_TEXT_LEFT);
            nk_label(ctx, desc2, NK_TEXT_LEFT);

            nk_layout_row_dynamic(ctx, 10, 1);
            nk_spacing(ctx, 1);
        };

        draw_option(
            "Convert SCT files to PNG",
            m_state.ui.export_sct_as_png,
            "When enabled, .sct/.sct2 files will be",
            "automatically converted to PNG during extraction.");

        draw_option(
            "Convert DB files to JSON",
            m_state.ui.export_db_as_json,
            "When enabled, .db files will be",
            "automatically converted to JSON during extraction.");

        draw_option(
            "Enable Open Folder",
            m_state.ui.enable_open_folder,
            "When enabled, the Open Folder button shows up",
            "letting you browse an unpacked folder directly.");

        nk_layout_row_dynamic(ctx, 25, 1);
        nk_layout_row_dynamic(ctx, 30, 2);
        if (nk_button_label(ctx, "OK")) {
            m_state.SaveOptionsToSettings();
            m_state.ui.show_options = false;
            m_state.tasks.runner.SetStatus("Options saved");
        }
        if (nk_button_label(ctx, "Cancel")) {
            m_state.ui.show_options = false;
        }
    } else {
        m_state.ui.show_options = false;
    }
    nk_end(ctx);
}

void PopupManager::RenderCreditsPopup(nk_context* ctx, int window_width, int window_height) {
    if (!m_state.ui.show_credits) return;

    constexpr float kWidth = 700.0f;
    constexpr float kHeight = 300.0f;
    const float x = (static_cast<float>(window_width) - kWidth) * 0.5f;
    const float y = (static_cast<float>(window_height) - kHeight) * 0.5f;

    if (nk_begin(ctx, "Credits", nk_rect(x, y, kWidth, kHeight),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label(ctx, AppInfo::WindowTitle, NK_TEXT_CENTERED);
        nk_label(ctx, "by @formagGino (github.com/formagGinoo)", NK_TEXT_CENTERED);
        nk_label(ctx, "and @akioukun (github.com/akioukun)", NK_TEXT_CENTERED);

        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "", NK_TEXT_LEFT);
        nk_label(ctx, "made with nuklear, sdl2/opengl, portable-file-dialogs", NK_TEXT_CENTERED);
        nk_label(ctx, "SCT/SCT2 support with astcenc & etcdec", NK_TEXT_CENTERED);
        nk_label(ctx, "thanks to @LukeFZ (github.com/LukeFZ) for DB decryption logic", NK_TEXT_CENTERED);
        nk_label(ctx, "thanks to @lIllIIlI (github.com/lIllIIlI) for SpineViewer logic", NK_TEXT_CENTERED);

        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Close")) {
            m_state.ui.show_credits = false;
        }
    } else {
        m_state.ui.show_credits = false;
    }
    nk_end(ctx);
}

void PopupManager::RenderFeedbackPopups(nk_context* ctx, int window_width, int window_height) {
    if (m_state.ui.show_success_popup) {
        constexpr float kWidth = 260.0f;
        constexpr float kHeight = 120.0f;
        const float x = (static_cast<float>(window_width) - kWidth) * 0.5f;
        const float y = (static_cast<float>(window_height) - kHeight) * 0.5f;

        if (nk_begin(ctx, "Success", nk_rect(x, y, kWidth, kHeight),
                     NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE))
        {
            nk_layout_row_dynamic(ctx, 30, 1);
            nk_label(ctx, m_state.ui.success_message.c_str(), NK_TEXT_CENTERED);
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "OK")) {
                m_state.ui.show_success_popup = false;
            }
        } else {
            m_state.ui.show_success_popup = false;
        }
        nk_end(ctx);
    }

    if (m_state.ui.show_error_popup) {
        constexpr float kWidth = 400.0f;
        constexpr float kHeight = 140.0f;
        const float x = (static_cast<float>(window_width) - kWidth) * 0.5f;
        const float y = (static_cast<float>(window_height) - kHeight) * 0.5f;

        if (nk_begin(ctx, "Error", nk_rect(x, y, kWidth, kHeight),
                     NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE))
        {
            nk_layout_row_dynamic(ctx, 45, 1);
            nk_label_wrap(ctx, m_state.ui.error_message.c_str());
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "OK")) {
                m_state.ui.show_error_popup = false;
            }
        } else {
            m_state.ui.show_error_popup = false;
        }
        nk_end(ctx);
    }
}

void PopupManager::RenderContextMenu(nk_context* ctx) {
    if (!m_state.context_menu.visible || !m_state.context_menu.node) return;

    const Core::FileNode& node = *m_state.context_menu.node;
    if (nk_begin(ctx, "Context Menu",
                 nk_rect(m_state.context_menu.position.x, m_state.context_menu.position.y, 180.0f, 200.0f),
                 NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
    {
        if (const auto* info = std::get_if<Core::FileInfo>(&node.data)) {
            nk_layout_row_dynamic(ctx, 25, 1);
            const Core::AssetType type = Core::ClassifyExtension(info->format);

            if (type == Core::AssetType::Database) {
                if (nk_button_label(ctx, "Export as JSON")) {
                    m_preview_panel.ExportFileAsJson(node, "Export DB as JSON");
                    m_state.context_menu.visible = false;
                }
            } else if (type == Core::AssetType::Scsp) {
                if (nk_button_label(ctx, "Export as JSON")) {
                    m_preview_panel.ExportFileAsJson(node, "Export SCSP as JSON");
                    m_state.context_menu.visible = false;
                }
            } else if (type == Core::AssetType::Sct) {
                if (nk_button_label(ctx, "Export as PNG")) {
                    m_preview_panel.ExportFileAsPng(node);
                    m_state.context_menu.visible = false;
                }
                if (nk_button_label(ctx, "Export as SCT")) {
                    m_preview_panel.ExportFileAsRaw(node, "Export as SCT", {"SCT Files", "*.sct;*.sct2", "All Files", "*.*"});
                    m_state.context_menu.visible = false;
                }
                if (nk_button_label(ctx, "Open Preview Window")) {
                    m_preview_panel.OpenInWindow(node);
                    m_state.context_menu.visible = false;
                }
            } else if (type == Core::AssetType::Image) {
                if (nk_button_label(ctx, "Export as PNG")) {
                    m_preview_panel.ExportFileAsPng(node);
                    m_state.context_menu.visible = false;
                }
                if (nk_button_label(ctx, "Open Preview Window")) {
                    m_preview_panel.OpenInWindow(node);
                    m_state.context_menu.visible = false;
                }
            }

            if (nk_button_label(ctx, "Extract Raw")) {
                m_preview_panel.ExportFileAsRaw(node, "Extract File", {"All Files", "*.*"});
                m_state.context_menu.visible = false;
            }
        }

        if (nk_button_label(ctx, "Close")) {
            m_state.context_menu.visible = false;
        }
    } else {
        m_state.context_menu.visible = false;
    }
    nk_end(ctx);
}
