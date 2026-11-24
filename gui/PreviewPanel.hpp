#pragma once

#include "gui/AppState.hpp"
#include "gui/ImageWindow.hpp"
#include "gui/PreviewManager.hpp"
#include "services/AssetExporter.hpp"

/**
 * Renders the active preview (image, database table, JSON/text, or error)
 * and coordinates single-file exports through AssetExporter.
 */
class PreviewPanel {
public:
    PreviewPanel(AppState& state, PreviewManager& preview, AssetExporter& exporter, ImageWindow& image_window);

    void Render(nk_context* ctx, float right_width, float content_height);

    void ExportFileAsPng(const Core::FileNode& node);
    void ExportFileAsRaw(const Core::FileNode& node, const std::string& dialog_title, const std::vector<std::string>& filters);
    void ExportFileAsJson(const Core::FileNode& node, const std::string& dialog_title);
    void OpenInWindow(const Core::FileNode& node);

private:
    void RenderImagePreview(nk_context* ctx, const ImagePreview& img, float right_width, float content_height);
    void RenderDatabasePreview(nk_context* ctx, DatabasePreview& db, float right_width, float content_height);
    void RenderTextPreview(nk_context* ctx, const TextPreview& text, float content_height);
    void RenderErrorPreview(nk_context* ctx, const ErrorPreview& err);

    AppState& m_state;
    PreviewManager& m_preview;
    AssetExporter& m_exporter;
    ImageWindow& m_image_window;
};
