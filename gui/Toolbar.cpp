#include "gui/Toolbar.hpp"
#include "archive/ArchiveFactory.hpp"
#include "core/FileDialogs.hpp"
#include "core/FileTree.hpp"
#include "core/StringUtils.hpp"
#include "gui/UiStyle.hpp"

#include <future>
#include <vector>

Toolbar::Toolbar(
    AppState& state,
    AssetExporter& exporter,
    PreviewManager& preview,
    FileBrowserPanel& file_browser)
    : m_state(state), m_exporter(exporter), m_preview(preview), m_file_browser(file_browser) {}

void Toolbar::OpenPackDialog() {
    try {
        const std::string selected_path = Core::FileDialogs::OpenFile(
            Core::DialogSlot::Open,
            "Select an archive or manifest file",
            {"Pack / Manifest Files", "*.pack;*.ssra", "All Files", "*.*"});
        if (selected_path.empty()) return;

        m_preview.Clear();
        m_state.ResetArchiveState();
        m_state.browser.archive = ArchiveFactory::Open(Core::Utf8ToWString(selected_path));
        if (m_state.browser.archive->GetType() == IArchive::PackType::Unknown) {
            m_state.tasks.runner.SetStatus("Error: Invalid or unknown file.");
            m_state.browser.archive.reset();
        } else {
            m_state.tasks.runner.SetStatus("Loaded. Click 'Scan Tree' to analyze contents.");
        }
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Error opening file: " + std::string(e.what()));
    }
}

void Toolbar::OpenFolderDialog() {
    try {
        const std::string selected_path = Core::FileDialogs::SelectFolder(
            Core::DialogSlot::Open, "Select a folder to view");
        if (selected_path.empty()) return;

        m_preview.Clear();
        m_state.ResetArchiveState();
        m_state.browser.archive = ArchiveFactory::Open(Core::Utf8ToWString(selected_path));
        if (m_state.browser.archive->GetType() == IArchive::PackType::Unknown) {
            m_state.tasks.runner.SetStatus("Error: Invalid or unknown folder.");
            m_state.browser.archive.reset();
        } else {
            m_state.tasks.runner.SetStatus("Folder Loaded. Click 'Scan Tree' to build the view.");
        }
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Error opening folder: " + std::string(e.what()));
    }
}

void Toolbar::StartScan() {
    if (!m_state.browser.archive) return;

    m_state.tasks.scan_complete.store(false, std::memory_order_relaxed);
    m_state.browser.selection.Clear();
    m_preview.Clear();

    IArchive* archive = m_state.browser.archive.get();
    m_state.tasks.runner.Start(
        Services::TaskKind::Scan,
        "Scanning...",
        [archive](Core::ProgressReporter reporter) {
            archive->Scan(std::move(reporter));
        });
}

void Toolbar::ToggleSpineViewer() {
    if (m_state.view_mode != ViewMode::SpineViewer) {
        if (!m_state.spine.catalog.IsBuilt() && !m_state.spine.building.load()) {
            m_state.spine.building.store(true);
            IArchive* archive = m_state.browser.archive.get();
            m_state.spine.build_future = std::async(std::launch::async, [this, archive]() {
                try {
                    m_state.spine.catalog.Build(*archive, archive->GetFileTree());
                } catch (...) {}
                m_state.spine.building.store(false);
            });
        }
        m_file_browser.SetSpineViewerMode();
    } else {
        m_file_browser.SetFileTreeMode();
    }
}

void Toolbar::StartExtractAll() {
    if (!m_state.browser.archive) return;
    try {
        const std::string dest_str = Core::FileDialogs::SelectFolder(
            Core::DialogSlot::Extract, "Select destination folder");
        if (dest_str.empty()) return;

        const std::filesystem::path dest_path(Core::Utf8ToWString(dest_str));
        const ExportOptions opts{
            .convert_sct_to_png = m_state.ui.export_sct_as_png,
            .convert_db_to_json = m_state.ui.export_db_as_json
        };
        IArchive* archive = m_state.browser.archive.get();
        const AssetExporter* exporter = &m_exporter;

        m_state.tasks.runner.Start(
            Services::TaskKind::Extract,
            "Extracting all files...",
            [archive, exporter, dest_path, opts](Core::ProgressReporter reporter) {
                exporter->ExtractAll(*archive, dest_path, opts, reporter);
            });
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Error starting extraction: " + std::string(e.what()));
    }
}

void Toolbar::StartExtractSelected() {
    if (!m_state.browser.archive) return;
    try {
        const std::string dest_str = Core::FileDialogs::SelectFolder(
            Core::DialogSlot::Extract, "Select destination folder");
        if (dest_str.empty()) return;

        const std::filesystem::path dest_path(Core::Utf8ToWString(dest_str));
        std::vector<const Core::FileNode*> nodes;

        if (m_state.view_mode == ViewMode::DiffTree) {
            const auto& sel = m_state.diff.selection;
            nodes.reserve(sel.selected_nodes.size() + 1);
            for (const auto* n : sel.selected_nodes) {
                if (n) {
                    if (const Core::FileNode* fn = Core::FindNodeByPath(m_state.browser.archive->GetFileTree(), n->full_path)) {
                        nodes.push_back(fn);
                    }
                }
            }
            if (nodes.empty() && sel.selected_node) {
                if (const Core::FileNode* fn = Core::FindNodeByPath(m_state.browser.archive->GetFileTree(), sel.selected_node->full_path)) {
                    nodes.push_back(fn);
                }
            }
        } else {
            const auto& sel = m_state.browser.selection;
            nodes.reserve(sel.selected_nodes.size() + 1);
            for (const auto* n : sel.selected_nodes) {
                if (n) nodes.push_back(n);
            }
            if (nodes.empty() && sel.selected_node) {
                nodes.push_back(sel.selected_node);
            }
        }

        const ExportOptions opts{
            .convert_sct_to_png = m_state.ui.export_sct_as_png,
            .convert_db_to_json = m_state.ui.export_db_as_json
        };
        IArchive* archive = m_state.browser.archive.get();
        const AssetExporter* exporter = &m_exporter;
        const size_t count = nodes.size();

        m_state.tasks.runner.Start(
            Services::TaskKind::Extract,
            "Extracting " + std::to_string(count) + " item(s)...",
            [archive, exporter, dest_path, nodes = std::move(nodes), opts](Core::ProgressReporter reporter) {
                exporter->ExtractNodes(*archive, nodes, dest_path, opts, reporter);
            });
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Error starting extraction: " + std::string(e.what()));
    }
}

void Toolbar::Render(nk_context* ctx) {
    const bool pack_loaded = (m_state.browser.archive != nullptr);
    const bool tree_scanned = pack_loaded && m_state.tasks.scan_complete.load();
    const bool running = m_state.tasks.runner.IsRunning();
    const bool in_diff = (m_state.view_mode == ViewMode::DiffTree);
    const bool in_spine = (m_state.view_mode == ViewMode::SpineViewer);

    const bool has_extract_selection = in_diff
        ? (!m_state.diff.selection.selected_nodes.empty() || m_state.diff.selection.selected_node != nullptr)
        : (!m_state.browser.selection.selected_nodes.empty() || m_state.browser.selection.selected_node != nullptr);

    nk_layout_row_dynamic(ctx, 38, m_state.ui.enable_open_folder ? 10 : 9);
    const nk_style_button btn_style = UiStyle::MakeToolbarButtonStyle(ctx);

    auto toolbar_button = [&](const char* label, bool enabled) -> bool {
        if (enabled) {
            return nk_button_label_styled(ctx, &btn_style, label) != 0;
        }
        nk_widget_disable_begin(ctx);
        nk_button_label_styled(ctx, &btn_style, label);
        nk_widget_disable_end(ctx);
        return false;
    };

    const bool can_open = !running && !pack_loaded;
    if (toolbar_button("Open Pack", can_open)) OpenPackDialog();
    if (m_state.ui.enable_open_folder && toolbar_button("Open Folder", can_open)) OpenFolderDialog();
    if (toolbar_button("Scan Tree", pack_loaded && !tree_scanned && !running)) StartScan();
    if (toolbar_button(in_spine ? "File Tree" : "Spine Viewer", tree_scanned && !running)) ToggleSpineViewer();
    if (toolbar_button(in_diff ? "File Tree" : "Diff Viewer", tree_scanned && !running)) {
        if (in_diff) m_file_browser.SetFileTreeMode();
        else m_file_browser.SetDiffViewerMode();
    }
    if (toolbar_button("Extract All", tree_scanned && !in_diff && !in_spine && !running)) StartExtractAll();
    if (toolbar_button("Extract Selected", tree_scanned && !in_spine && has_extract_selection && !running)) StartExtractSelected();
    if (toolbar_button("Export filemap JSON", tree_scanned && !running)) m_file_browser.ExportFileMapJson();
    if (toolbar_button("Options", !running)) m_state.ui.show_options = true;
    if (toolbar_button("Credits", true)) m_state.ui.show_credits = true;
}
