#pragma once

#include "gui/AppState.hpp"
#include "services/AssetExporter.hpp"

#include <string>
#include <vector>

/**
 * Renders the Spine catalog tree, interactive OpenGL viewport, and bone editor panel.
 */
class SpinePanel {
public:
    SpinePanel(AppState& state, AssetExporter& exporter);

    void Render(nk_context* ctx, float content_height, int window_width);
    void HandleKeyDown(SDL_Keycode key);

private:
    void LoadSelectedSkeleton();
    void DrawCategoryTree(nk_context* ctx, const SpineCategory& cat, const std::vector<SpineEntry>& entries, const std::string& query_lower, int depth);
    void DrawEntryRow(nk_context* ctx, size_t idx, const SpineEntry& entry, float indent_px, bool dynamic_row);
    void RenderViewport(nk_context* ctx, float panel_height, const std::vector<SpineEntry>& entries);
    void RenderBoneEditor(nk_context* ctx, float editor_width, float panel_height, const std::vector<SpineEntry>& entries);
    int ExportAtlasAndImages(const SpineEntry& entry, const std::filesystem::path& dest_dir);

    AppState& m_state;
    AssetExporter& m_exporter;
};
