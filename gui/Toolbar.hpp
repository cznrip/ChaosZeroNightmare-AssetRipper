#pragma once

#include "gui/AppState.hpp"
#include "gui/FileBrowserPanel.hpp"
#include "gui/PreviewManager.hpp"
#include "services/AssetExporter.hpp"

class Toolbar {
public:
    Toolbar(
        AppState& state,
        AssetExporter& exporter,
        PreviewManager& preview,
        FileBrowserPanel& file_browser);

    void Render(nk_context* ctx);

private:
    void OpenPackDialog();
    void OpenFolderDialog();
    void StartScan();
    void StartExtractAll();
    void StartExtractSelected();
    void ToggleSpineViewer();

    AppState& m_state;
    AssetExporter& m_exporter;
    PreviewManager& m_preview;
    FileBrowserPanel& m_file_browser;
};
