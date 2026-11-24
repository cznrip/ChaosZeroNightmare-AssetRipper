#pragma once

#include "gui/AppState.hpp"
#include "gui/PreviewManager.hpp"

#include <string>
#include <unordered_set>

/**
 * Renders both the standard archive file tree and the diff tree with
 * cached search filtering (eliminating O(N) recursive searches every frame).
 */
class FileBrowserPanel {
public:
    FileBrowserPanel(AppState& state, PreviewManager& preview);

    void Render(nk_context* ctx, bool tree_scanned, bool& scroll_to_selected);
    void HandleKeyDown(SDL_Keycode key, bool& scroll_to_selected);

    void SetFileTreeMode();
    void SetSpineViewerMode();
    bool SetDiffViewerMode();
    void ExportFileMapJson();

private:
    void EnsureFileFilterCache();
    void EnsureDiffFilterCache();

    void HandleFileNodeClick(const Core::FileNode* node, bool is_folder);
    void HandleDiffNodeClick(const DiffNode* node, bool is_folder);

    void DrawFileNode(nk_context* ctx, const Core::FileNode& node, int depth = 0);
    void DrawDiffNode(nk_context* ctx, const DiffNode& node, int depth = 0);

    bool LoadDiffTreeFromFileMap();

    AppState& m_state;
    PreviewManager& m_preview;

    // Cached search filter sets (rebuilt only when query or tree revision changes)
    std::string m_cached_file_query;
    uint64_t m_cached_file_revision = 0;
    std::unordered_set<const Core::FileNode*> m_file_visible_set;
    std::unordered_set<const Core::FileNode*> m_file_direct_match_set;

    std::string m_cached_diff_query;
    uint64_t m_cached_diff_revision = 0;
    std::unordered_set<const DiffNode*> m_diff_visible_set;
};
