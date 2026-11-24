#include "gui/PreviewPanel.hpp"
#include "core/AssetFormat.hpp"
#include "core/FileDialogs.hpp"
#include "core/StringUtils.hpp"

#include <SDL.h>
#include <algorithm>
#include <format>
#include <fstream>
#include <string_view>

PreviewPanel::PreviewPanel(
    AppState& state,
    PreviewManager& preview,
    AssetExporter& exporter,
    ImageWindow& image_window)
    : m_state(state), m_preview(preview), m_exporter(exporter), m_image_window(image_window) {}

void PreviewPanel::OpenInWindow(const Core::FileNode& node) {
    if (!m_state.browser.archive) return;
    std::string err;
    if (!m_image_window.Open(*m_state.browser.archive, node, err) && !err.empty()) {
        m_state.tasks.runner.SetStatus(err);
    }
}

void PreviewPanel::ExportFileAsPng(const Core::FileNode& node) {
    if (!m_state.browser.archive) return;
    try {
        const std::string default_name = Core::ReplaceExtension(node.name, ".png");
        const std::string save_path = Core::FileDialogs::SaveFile(
            "Export as PNG", default_name, {"PNG Files", "*.png", "All Files", "*.*"});
        if (save_path.empty()) return;

        const ExportOptions opts{.convert_sct_to_png = true, .convert_db_to_json = false};
        if (m_exporter.ExportSingleFile(*m_state.browser.archive, node, Core::Utf8ToWString(save_path), opts, true)) {
            m_state.tasks.runner.SetStatus("Exported to: " + save_path);
        } else {
            m_state.tasks.runner.SetStatus("Failed to export PNG: " + node.name);
        }
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Export error: " + std::string(e.what()));
    }
}

void PreviewPanel::ExportFileAsRaw(
    const Core::FileNode& node,
    const std::string& dialog_title,
    const std::vector<std::string>& filters)
{
    if (!m_state.browser.archive) return;
    try {
        const std::string save_path = Core::FileDialogs::SaveFile(dialog_title, node.name, filters);
        if (save_path.empty()) return;

        if (m_exporter.ExportSingleFile(*m_state.browser.archive, node, Core::Utf8ToWString(save_path), {}, false)) {
            m_state.tasks.runner.SetStatus("Exported to: " + save_path);
        } else {
            m_state.tasks.runner.SetStatus("Failed to export: " + node.name);
        }
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Export error: " + std::string(e.what()));
    }
}

void PreviewPanel::ExportFileAsJson(const Core::FileNode& node, const std::string& dialog_title) {
    if (!m_state.browser.archive) return;
    try {
        const std::string default_name = Core::ReplaceExtension(node.name, ".json");
        const std::string save_path = Core::FileDialogs::SaveFile(
            dialog_title, default_name, {"JSON Files", "*.json", "All Files", "*.*"});
        if (save_path.empty()) return;

        const ExportOptions opts{.convert_sct_to_png = false, .convert_db_to_json = true};
        if (m_exporter.ExportSingleFile(*m_state.browser.archive, node, Core::Utf8ToWString(save_path), opts, true)) {
            m_state.tasks.runner.SetStatus("Exported JSON to: " + save_path);
        } else {
            m_state.tasks.runner.SetStatus("Failed to convert to JSON: " + node.name);
        }
    } catch (const std::exception& e) {
        m_state.tasks.runner.SetStatus("Export error: " + std::string(e.what()));
    }
}

void PreviewPanel::RenderImagePreview(
    nk_context* ctx,
    const ImagePreview& img,
    float right_width,
    float content_height)
{
    const Core::FileNode* node = m_preview.CurrentNode();

    nk_layout_row_dynamic(ctx, 30, 1);
    if (node) {
        const std::string title = "Preview: " + node->name;
        nk_label(ctx, title.c_str(), NK_TEXT_CENTERED);
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    const std::string dims = std::format("{} x {}", img.width, img.height);
    nk_label_colored(ctx, dims.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

    if (node && node->IsFile()) {
        const auto& info = std::get<Core::FileInfo>(node->data);
        nk_layout_row_dynamic(ctx, 25, 1);
        const std::string size_str = "Size: " + Core::FormatSize(info.size);
        nk_label_colored(ctx, size_str.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

        if (Core::IsPreviewableImageExtension(info.format)) {
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "Open in Window")) {
                OpenInWindow(*node);
            }
        }
    }

    const float max_w = (std::max)(1.0f, right_width - 40.0f);
    const float max_h = (std::max)(1.0f, content_height - 180.0f);
    const float scale = (std::min)({1.0f, max_w / static_cast<float>(img.width), max_h / static_cast<float>(img.height)});
    const float display_w = static_cast<float>(img.width) * scale;
    const float display_h = static_cast<float>(img.height) * scale;

    nk_layout_row_begin(ctx, NK_STATIC, display_h, 1);
    nk_layout_row_push(ctx, display_w);
    nk_command_buffer* canvas = nk_window_get_canvas(ctx);
    const struct nk_rect bounds = nk_widget_bounds(ctx);
    const struct nk_image nk_img = nk_image_id(static_cast<int>(img.texture.Id()));
    nk_draw_image(canvas, bounds, &nk_img, nk_rgb(255, 255, 255));
    nk_layout_row_end(ctx);
}

void PreviewPanel::RenderDatabasePreview(
    nk_context* ctx,
    DatabasePreview& db,
    float right_width,
    float content_height)
{
    const Core::FileNode* node = m_preview.CurrentNode();

    if (node) {
        nk_layout_row_dynamic(ctx, 30, 1);
        const std::string title = "Database Preview: " + db.filename;
        nk_label_colored(ctx, title.c_str(), NK_TEXT_CENTERED, nk_rgb(150, 200, 255));
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    const std::string stats = std::format("{} rows x {} columns", db.rows.size(), db.column_names.size());
    nk_label_colored(ctx, stats.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

    if (node) {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Export as JSON")) {
            ExportFileAsJson(*node, "Export DB as JSON");
        }
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label_colored(ctx, "Preview:", NK_TEXT_LEFT, nk_rgb(200, 200, 200));

    nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
    nk_layout_row_push(ctx, 80);
    nk_label(ctx, "Search:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, right_width - 100);
    nk_edit_string_zero_terminated(
        ctx, NK_EDIT_FIELD, db.search_buffer.data(), static_cast<int>(db.search_buffer.size()), nk_filter_default);
    nk_layout_row_end(ctx);

    const float table_height = content_height - 230.0f;
    nk_layout_row_dynamic(ctx, table_height, 1);

    const std::string group_id = "DBPreviewTable_" + db.filename;
    if (nk_group_begin(ctx, group_id.c_str(), NK_WINDOW_BORDER)) {
        constexpr float kIndexColWidth = 60.0f;
        const int col_count = static_cast<int>(db.column_names.size()) + 1;

        nk_layout_row_begin(ctx, NK_STATIC, 40, col_count);
        nk_layout_row_push(ctx, kIndexColWidth);
        nk_fill_rect(&ctx->current->buffer, nk_widget_bounds(ctx), 0, nk_rgb(60, 70, 90));
        nk_label_colored(ctx, "#", NK_TEXT_CENTERED, nk_rgb(220, 230, 255));

        for (size_t j = 0; j < db.column_names.size(); ++j) {
            nk_layout_row_push(ctx, db.column_widths[j]);
            nk_fill_rect(&ctx->current->buffer, nk_widget_bounds(ctx), 0, nk_rgb(60, 70, 90));
            nk_label_colored(ctx, db.column_names[j].c_str(), NK_TEXT_CENTERED, nk_rgb(220, 230, 255));
        }
        nk_layout_row_end(ctx);

        const std::string_view query(db.search_buffer.data());
        int visible_index = 1;
        int rows_shown = 0;
        for (const auto& row : db.rows) {
            if (rows_shown > 200) break;

            bool match = query.empty();
            if (!match) {
                for (const auto& cell : row) {
                    if (cell.find(query) != std::string::npos) {
                        match = true;
                        break;
                    }
                }
            }
            if (!match) continue;

            nk_layout_row_begin(ctx, NK_STATIC, 30, col_count);
            nk_layout_row_push(ctx, kIndexColWidth);
            const std::string idx_str = std::to_string(visible_index++);
            nk_label_colored(ctx, idx_str.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

            for (size_t j = 0; j < db.column_names.size(); ++j) {
                nk_layout_row_push(ctx, db.column_widths[j]);
                const char* val = (j < row.size()) ? row[j].c_str() : "";
                nk_label_colored(ctx, val, NK_TEXT_LEFT, nk_rgb(240, 240, 240));
            }
            nk_layout_row_end(ctx);
            ++rows_shown;
        }
        nk_group_end(ctx);
    }
}

void PreviewPanel::RenderTextPreview(nk_context* ctx, const TextPreview& text, float content_height) {
    const Core::FileNode* node = m_preview.CurrentNode();

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, text.is_json ? "JSON Preview" : "Text Viewer", NK_TEXT_CENTERED);

    nk_layout_row_begin(ctx, NK_STATIC, 30, 2);
    nk_layout_row_push(ctx, 120);
    if (nk_button_label(ctx, "Copy All")) {
        SDL_SetClipboardText(text.full_text.c_str());
    }
    nk_layout_row_push(ctx, 120);
    if (nk_button_label(ctx, "Save As...")) {
        try {
            const std::string base_name = node ? node->name : "output";
            std::string data_to_save = text.full_text;
            std::string default_name;
            std::string title;
            std::vector<std::string> filters;

            if (text.is_json) {
                default_name = Core::ReplaceExtension(base_name, ".json");
                title = "Save JSON";
                filters = {"JSON Files", "*.json", "All Files", "*.*"};
            } else {
                const bool is_atlas = (node && node->IsFile() &&
                                       Core::IsAtlasExtension(std::get<Core::FileInfo>(node->data).format)) ||
                                      (data_to_save.find("format: ") != std::string::npos &&
                                       data_to_save.find("filter: ") != std::string::npos);
                if (is_atlas) {
                    data_to_save = Core::RewriteAtlasExtensions(data_to_save);
                    default_name = Core::ReplaceExtension(base_name, ".atlas");
                    title = "Save Atlas";
                    filters = {"Atlas Files", "*.atlas", "Text Files", "*.txt", "All Files", "*.*"};
                } else {
                    default_name = Core::ReplaceExtension(base_name, ".txt");
                    title = "Save Text";
                    filters = {"Text Files", "*.txt", "All Files", "*.*"};
                }
            }

            if (const std::string save_path = Core::FileDialogs::SaveFile(title, default_name, filters); !save_path.empty()) {
                if (std::ofstream out(Core::Utf8ToWString(save_path), std::ios::binary); out.is_open()) {
                    out.write(data_to_save.data(), static_cast<std::streamsize>(data_to_save.size()));
                    m_state.tasks.runner.SetStatus("Saved to: " + save_path);
                }
            }
        } catch (const std::exception& e) {
            m_state.tasks.runner.SetStatus("Save error: " + std::string(e.what()));
        }
    }
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, content_height - 130.0f, 1);
    std::string group_id = text.is_json ? "JsonPreview" : "TextPreview";
    if (node) group_id += "_" + node->name;

    if (nk_group_begin(ctx, group_id.c_str(), NK_WINDOW_BORDER)) {
        for (const auto& line : text.preview_lines) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, line.c_str(), NK_TEXT_LEFT, nk_rgb(220, 220, 220));
        }
        if (text.truncated) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, "... (preview limit reached)", NK_TEXT_LEFT, nk_rgb(255, 100, 100));
        }
        nk_group_end(ctx);
    }
}

void PreviewPanel::RenderErrorPreview(nk_context* ctx, const ErrorPreview& err) {
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label_colored(ctx, "Error:", NK_TEXT_CENTERED, nk_rgb(255, 100, 100));
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, err.message.c_str(), NK_TEXT_CENTERED, nk_rgb(255, 255, 255));
}

void PreviewPanel::Render(nk_context* ctx, float right_width, float content_height) {
    if (!nk_group_begin(ctx, "Preview", NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        return;
    }

    std::visit([&](auto& content) {
        using T = std::decay_t<decltype(content)>;
        if constexpr (std::is_same_v<T, ImagePreview>) {
            RenderImagePreview(ctx, content, right_width, content_height);
        } else if constexpr (std::is_same_v<T, DatabasePreview>) {
            RenderDatabasePreview(ctx, content, right_width, content_height);
        } else if constexpr (std::is_same_v<T, TextPreview>) {
            RenderTextPreview(ctx, content, content_height);
        } else if constexpr (std::is_same_v<T, ErrorPreview>) {
            RenderErrorPreview(ctx, content);
        }
    }, m_preview.MutableData());

    nk_group_end(ctx);
}
