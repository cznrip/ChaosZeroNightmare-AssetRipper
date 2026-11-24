#include "gui/SpinePanel.hpp"
#include "core/AssetFormat.hpp"
#include "core/FileDialogs.hpp"
#include "core/StringUtils.hpp"
#include "gui/UiStyle.hpp"
#include "parsers/ScspParser.hpp"
#include "parsers/SpineRenderer.hpp"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

    bool CategoryHasSearchMatch(
        const SpineCategory& cat,
        const std::vector<SpineEntry>& entries,
        const std::string& query_lower)
    {
        if (query_lower.empty()) return true;
        for (const size_t idx : cat.entry_indices) {
            if (Core::ToLower(entries[idx].display_name).find(query_lower) != std::string::npos ||
                Core::ToLower(entries[idx].category).find(query_lower) != std::string::npos)
            {
                return true;
            }
        }
        for (const auto& [_, sub] : cat.subcategories) {
            if (CategoryHasSearchMatch(sub, entries, query_lower)) return true;
        }
        return false;
    }

} // namespace

SpinePanel::SpinePanel(AppState& state, AssetExporter& exporter)
    : m_state(state), m_exporter(exporter) {}

void SpinePanel::LoadSelectedSkeleton() {
    auto& sp = m_state.spine;
    if (!m_state.browser.archive) return;
    const auto& entries = sp.catalog.GetEntries();
    if (sp.selected_index < 0 || sp.selected_index >= static_cast<int>(entries.size())) return;

    if (!sp.viewer) sp.viewer = std::make_unique<SpineViewer>();
    sp.viewer->LoadSkeleton(sp.catalog, *m_state.browser.archive, entries[sp.selected_index]);
    sp.viewer->SetFlipX(sp.flip_x);
    sp.viewer->SetFlipY(sp.flip_y);
    sp.selected_animation = 0;
    sp.selected_skin = 0;
    sp.last_tick = 0;
    sp.edit_mode = false;
}

void SpinePanel::HandleKeyDown(SDL_Keycode key) {
    auto& sp = m_state.spine;
    if (m_state.view_mode != ViewMode::SpineViewer || !sp.catalog.IsBuilt() || sp.visible_indices.empty()) {
        return;
    }

    if (key == SDLK_UP || key == SDLK_DOWN) {
        const auto it = std::ranges::find(sp.visible_indices, sp.selected_index);
        int new_idx = sp.selected_index;
        if (key == SDLK_UP) {
            if (it != sp.visible_indices.end() && it != sp.visible_indices.begin()) new_idx = *std::prev(it);
            else if (it == sp.visible_indices.end()) new_idx = sp.visible_indices.back();
        } else {
            if (it != sp.visible_indices.end() && std::next(it) != sp.visible_indices.end()) new_idx = *std::next(it);
            else if (it == sp.visible_indices.end()) new_idx = sp.visible_indices.front();
        }
        if (new_idx != sp.selected_index) {
            sp.selected_index = new_idx;
            LoadSelectedSkeleton();
        }
    } else if (key == SDLK_RETURN && sp.selected_index >= 0) {
        LoadSelectedSkeleton();
    }
}

void SpinePanel::DrawEntryRow(nk_context* ctx, size_t idx, const SpineEntry& entry, float indent_px, bool dynamic_row) {
    auto& sp = m_state.spine;
    sp.visible_indices.push_back(static_cast<int>(idx));

    const bool is_sel = (static_cast<int>(idx) == sp.selected_index);
    nk_style_button btn = ctx->style.button;
    btn.text_alignment = NK_TEXT_LEFT;
    btn.padding = dynamic_row ? nk_vec2(16, 3) : nk_vec2(6, 3);
    btn.rounding = 2.0f;
    btn.normal = nk_style_item_color(is_sel ? nk_rgb(55, 80, 120) : nk_rgb(38, 38, 42));
    btn.hover = nk_style_item_color(is_sel ? nk_rgb(65, 90, 130) : nk_rgb(50, 50, 55));
    btn.text_normal = is_sel ? nk_rgb(255, 255, 255) : nk_rgb(190, 190, 190);
    btn.text_hover = nk_rgb(255, 255, 255);

    if (dynamic_row) {
        nk_layout_row_dynamic(ctx, 24, 1);
    } else {
        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        nk_layout_row_push(ctx, indent_px);
        nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, 300.0f - indent_px);
    }

    if (nk_button_label_styled(ctx, &btn, entry.display_name.c_str())) {
        if (sp.selected_index != static_cast<int>(idx)) {
            sp.selected_index = static_cast<int>(idx);
            LoadSelectedSkeleton();
        }
    }

    if (!dynamic_row) {
        nk_layout_row_end(ctx);
    }
}

void SpinePanel::DrawCategoryTree(
    nk_context* ctx,
    const SpineCategory& cat,
    const std::vector<SpineEntry>& entries,
    const std::string& query_lower,
    int depth)
{
    auto& sp = m_state.spine;

    for (const auto& [_, sub] : cat.subcategories) {
        if (!CategoryHasSearchMatch(sub, entries, query_lower)) continue;

        const bool expanded = !query_lower.empty() || sp.expanded_categories.contains(sub.full_path);

        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        const float indent = static_cast<float>(depth) * 16.0f;
        if (indent > 0.0f) {
            nk_layout_row_push(ctx, indent);
            nk_spacing(ctx, 1);
        }

        nk_layout_row_push(ctx, 300.0f - indent);
        nk_style_button cbtn = ctx->style.button;
        cbtn.text_alignment = NK_TEXT_LEFT;
        cbtn.padding = nk_vec2(6, 3);
        cbtn.rounding = 2.0f;
        const int shade = 50 + (depth % 3) * 5;
        cbtn.normal = nk_style_item_color(nk_rgb(shade, shade + 5, shade + 15));
        cbtn.hover = nk_style_item_color(nk_rgb(shade + 10, shade + 15, shade + 25));
        cbtn.text_normal = nk_rgb(180, 200, 230);
        cbtn.text_hover = nk_rgb(220, 230, 255);

        const std::string label = std::format("{} {} ({})", expanded ? "-" : "+", sub.name, sub.total_entries);
        if (nk_button_label_styled(ctx, &cbtn, label.c_str())) {
            if (sp.expanded_categories.contains(sub.full_path)) sp.expanded_categories.erase(sub.full_path);
            else sp.expanded_categories.insert(sub.full_path);
        }
        nk_layout_row_end(ctx);

        if (!expanded) continue;

        const float entry_indent = static_cast<float>(depth + 1) * 16.0f;
        for (const size_t idx : sub.entry_indices) {
            const auto& ent = entries[idx];
            if (!query_lower.empty() &&
                Core::ToLower(ent.display_name).find(query_lower) == std::string::npos &&
                Core::ToLower(ent.category).find(query_lower) == std::string::npos)
            {
                continue;
            }
            DrawEntryRow(ctx, idx, ent, entry_indent, false);
        }

        DrawCategoryTree(ctx, sub, entries, query_lower, depth + 1);
    }

    if (depth == 0) {
        for (const size_t idx : cat.entry_indices) {
            const auto& ent = entries[idx];
            if (!query_lower.empty() && Core::ToLower(ent.display_name).find(query_lower) == std::string::npos) {
                continue;
            }
            DrawEntryRow(ctx, idx, ent, 0.0f, true);
        }
    }
}

int SpinePanel::ExportAtlasAndImages(const SpineEntry& entry, const std::filesystem::path& dest_dir) {
    if (!m_state.browser.archive) return 0;
    const ExportOptions opts{.convert_sct_to_png = true, .convert_db_to_json = false};
    int exported = 0;

    if (entry.atlas_node) {
        const auto dst = dest_dir / Core::Utf8ToWString(entry.atlas_node->name);
        if (m_exporter.ExportSingleFile(*m_state.browser.archive, *entry.atlas_node, dst, opts, true)) {
            ++exported;
        }
    }

    m_state.spine.catalog.EnsureDetailsLoaded(*m_state.browser.archive, entry);
    for (const auto* img : entry.image_nodes) {
        const auto* fi = std::get_if<Core::FileInfo>(&img->data);
        if (!fi) continue;
        const std::string out_name = Core::IsSctExtension(fi->format)
            ? Core::ReplaceExtension(img->name, ".png")
            : img->name;
        const auto dst = dest_dir / Core::Utf8ToWString(out_name);
        if (m_exporter.ExportSingleFile(*m_state.browser.archive, *img, dst, opts, true)) {
            ++exported;
        }
    }
    return exported;
}

void SpinePanel::RenderViewport(nk_context* ctx, float panel_height, const std::vector<SpineEntry>& entries) {
    auto& sp = m_state.spine;
    if (!nk_group_begin(ctx, "SpineViewInline", NK_WINDOW_BORDER)) return;

    if (sp.viewer && sp.viewer->IsLoaded()) {
        // Row 1: Animation, Skin, Play/Pause
        nk_layout_row_begin(ctx, NK_STATIC, 28, 8);
        nk_layout_row_push(ctx, 50);
        nk_label(ctx, "Anim:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 160);
        if (const auto anim_names = sp.viewer->GetAnimationNames(); !anim_names.empty()) {
            sp.selected_animation = sp.viewer->GetCurrentAnimIndex();
            if (sp.selected_animation >= static_cast<int>(anim_names.size())) sp.selected_animation = 0;
            if (nk_combo_begin_label(ctx, anim_names[sp.selected_animation].c_str(), nk_vec2(200, 300))) {
                nk_layout_row_dynamic(ctx, 22, 1);
                for (int a = 0; a < static_cast<int>(anim_names.size()); ++a) {
                    if (nk_combo_item_label(ctx, anim_names[a].c_str(), NK_TEXT_LEFT) && a != sp.selected_animation) {
                        sp.selected_animation = a;
                        sp.viewer->SetAnimation(anim_names[a], true);
                    }
                }
                nk_combo_end(ctx);
            }
        }

        nk_layout_row_push(ctx, 45);
        nk_label(ctx, "Skin:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 120);
        if (const auto skin_names = sp.viewer->GetSkinNames(); !skin_names.empty()) {
            if (sp.selected_skin >= static_cast<int>(skin_names.size())) sp.selected_skin = 0;
            if (nk_combo_begin_label(ctx, skin_names[sp.selected_skin].c_str(), nk_vec2(160, 300))) {
                nk_layout_row_dynamic(ctx, 22, 1);
                for (int s = 0; s < static_cast<int>(skin_names.size()); ++s) {
                    if (nk_combo_item_label(ctx, skin_names[s].c_str(), NK_TEXT_LEFT) && s != sp.selected_skin) {
                        sp.selected_skin = s;
                        sp.viewer->SetSkin(skin_names[s]);
                    }
                }
                nk_combo_end(ctx);
            }
        }

        nk_layout_row_push(ctx, 60);
        if (nk_button_label(ctx, sp.playing ? "Pause" : "Play")) {
            sp.playing = !sp.playing;
            sp.viewer->SetPlaying(sp.playing);
            if (sp.playing) sp.last_tick = SDL_GetPerformanceCounter();
        }
        nk_layout_row_end(ctx);

        // Row 2: Speed, Zoom, Flip X/Y, Edit, Reset, Export All
        nk_layout_row_begin(ctx, NK_STATIC, 28, 13);
        nk_layout_row_push(ctx, 50);
        nk_label(ctx, "Speed:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 120);
        nk_slider_float(ctx, 0.1f, &sp.speed, 3.0f, 0.1f);
        nk_layout_row_push(ctx, 40);
        const std::string speed_label = std::format("{:.1f}x", sp.speed);
        nk_label(ctx, speed_label.c_str(), NK_TEXT_LEFT);

        nk_layout_row_push(ctx, 45);
        nk_label(ctx, "Zoom:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 100);
        sp.zoom = sp.viewer->GetZoom();
        nk_slider_float(ctx, 0.1f, &sp.zoom, 5.0f, 0.1f);
        nk_layout_row_push(ctx, 40);
        const std::string zoom_label = std::format("{:.1f}x", sp.zoom);
        nk_label(ctx, zoom_label.c_str(), NK_TEXT_LEFT);
        sp.viewer->SetZoom(sp.zoom);

        nk_layout_row_push(ctx, 60);
        {
            const nk_style_button st = UiStyle::MakeAccentToggleStyle(ctx, sp.flip_x, nk_rgb(56, 120, 74));
            if (nk_button_label_styled(ctx, &st, "Flip X")) {
                sp.flip_x = !sp.flip_x;
                sp.viewer->SetFlipX(sp.flip_x);
            }
        }

        nk_layout_row_push(ctx, 60);
        {
            const nk_style_button st = UiStyle::MakeAccentToggleStyle(ctx, sp.flip_y, nk_rgb(56, 120, 74));
            if (nk_button_label_styled(ctx, &st, "Flip Y")) {
                sp.flip_y = !sp.flip_y;
                sp.viewer->SetFlipY(sp.flip_y);
            }
        }

        nk_layout_row_push(ctx, 45);
        {
            const nk_style_button st = UiStyle::MakeAccentToggleStyle(ctx, sp.edit_mode, nk_rgb(120, 80, 40));
            if (nk_button_label_styled(ctx, &st, "Edit")) {
                sp.edit_mode = !sp.edit_mode;
            }
        }

        nk_layout_row_push(ctx, 55);
        if (nk_button_label(ctx, "Reset")) {
            sp.zoom = 1.0f;
            sp.viewer->ResetView();
        }

        nk_layout_row_push(ctx, 100);
        if (sp.selected_index >= 0 && sp.selected_index < static_cast<int>(entries.size())) {
            if (nk_button_label(ctx, "Export All")) {
                const auto& entry = entries[sp.selected_index];
                try {
                    if (const std::string dest = Core::FileDialogs::SelectFolder(Core::DialogSlot::Extract, "Select destination folder"); !dest.empty()) {
                        const std::filesystem::path dest_dir(Core::Utf8ToWString(dest));
                        int exported = 0;
                        const auto scsp_data = m_state.browser.archive->GetFileData(*entry.scsp_node);
                        if (const auto parsed = ScspParser::ParseToJson(scsp_data); !parsed.empty()) {
                            std::ofstream out(dest_dir / Core::Utf8ToWString(entry.display_name + ".json"));
                            out << parsed.dump(2);
                            ++exported;
                        }
                        exported += ExportAtlasAndImages(entry, dest_dir);
                        m_state.tasks.runner.SetStatus(std::format("Exported {} files for '{}'", exported, entry.display_name));
                    }
                } catch (const std::exception& e) {
                    m_state.tasks.runner.SetStatus("Export error: " + std::string(e.what()));
                }
            }
        }
        nk_layout_row_end(ctx);

        // Row 3: Auto, Next, PMA, BG
        nk_layout_row_begin(ctx, NK_STATIC, 24, 7);
        nk_layout_row_push(ctx, 80);
        {
            const nk_style_button st = UiStyle::MakeAccentToggleStyle(ctx, sp.autoplay, nk_rgb(56, 120, 74));
            if (nk_button_label_styled(ctx, &st, sp.autoplay ? "Auto: ON" : "Auto: OFF")) {
                sp.autoplay = !sp.autoplay;
                sp.viewer->SetAutoplayNext(sp.autoplay);
            }
        }
        nk_layout_row_push(ctx, 50);
        if (nk_button_label(ctx, "Next")) {
            sp.viewer->NextAnimation();
            sp.selected_animation = sp.viewer->GetCurrentAnimIndex();
        }
        nk_layout_row_push(ctx, 20);
        nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, 80);
        {
            const nk_style_button st = UiStyle::MakeAccentToggleStyle(ctx, sp.pma_blend, nk_rgb(70, 90, 120));
            if (nk_button_label_styled(ctx, &st, sp.pma_blend ? "PMA: ON" : "PMA: OFF")) {
                sp.pma_blend = !sp.pma_blend;
                sp.viewer->SetUsePma(sp.pma_blend);
                sp.viewer->SetPremultiplyTextures(false);
                LoadSelectedSkeleton();
            }
        }
        nk_layout_row_push(ctx, 10);
        nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, 80);
        {
            static constexpr std::array<const char*, 4> kBgLabels = {"BG: None", "BG: Dark", "BG: Gray", "BG: White"};
            static constexpr std::array<std::array<float, 3>, 4> kBgColors = {{
                {0.0f, 0.0f, 0.0f}, {0.12f, 0.12f, 0.14f}, {0.35f, 0.35f, 0.38f}, {1.0f, 1.0f, 1.0f}
            }};
            const nk_style_button st = UiStyle::MakeAccentToggleStyle(ctx, false, nk_rgb(60, 60, 65));
            if (nk_button_label_styled(ctx, &st, kBgLabels[sp.bg_preset])) {
                sp.bg_preset = (sp.bg_preset + 1) % 4;
                const auto& c = kBgColors[sp.bg_preset];
                sp.viewer->SetBgColor(c[0], c[1], c[2]);
            }
        }
        nk_layout_row_end(ctx);

        // Viewport canvas & mouse interaction
        const float vp_h = (std::max)(100.0f, panel_height - 120.0f);
        nk_layout_row_dynamic(ctx, vp_h, 1);
        const struct nk_rect vb = nk_widget_bounds(ctx);
        const int vw = static_cast<int>(vb.w);
        const int vh = static_cast<int>(vb.h);

        nk_input* inp = &ctx->input;
        const bool popup_active = (ctx->current && ctx->current->popup.win);
        if (!popup_active && nk_input_is_mouse_hovering_rect(inp, vb)) {
            const float scr = inp->mouse.scroll_delta.y;
            const float mdx = inp->mouse.delta.x;
            const float mdy = inp->mouse.delta.y;
            const Uint32 km = SDL_GetModState();

            if (scr != 0.0f && !(km & KMOD_CTRL)) {
                sp.viewer->ZoomBy(scr > 0.0f ? 1.15f : 1.0f / 1.15f);
                sp.zoom = sp.viewer->GetZoom();
            }

            if (nk_input_is_mouse_down(inp, NK_BUTTON_MIDDLE) ||
                nk_input_is_mouse_down(inp, NK_BUTTON_RIGHT) ||
                (nk_input_is_mouse_down(inp, NK_BUTTON_LEFT) && (!sp.edit_mode || (km & KMOD_SHIFT))))
            {
                if (mdx != 0.0f || mdy != 0.0f) {
                    const float s = sp.viewer->GetZoom() > 0.0f ? (1.0f / sp.viewer->GetZoom()) : 1.0f;
                    sp.viewer->Pan(mdx * s, -mdy * s);
                }
            }

            if (sp.edit_mode && !sp.selected_bone.empty() && scr != 0.0f && (km & KMOD_CTRL)) {
                for (const auto& b : sp.viewer->GetBoneList()) {
                    if (b.name == sp.selected_bone) {
                        sp.viewer->SetBoneOverride(b.name, BoneOverride{
                            b.x, b.y, b.rotation,
                            b.scale_x + scr * 0.05f, b.scale_y + scr * 0.05f,
                            b.shear_x, b.shear_y
                        });
                        break;
                    }
                }
            }

            if (sp.edit_mode) {
                auto ag = static_cast<SpineViewer::GizmoHandle>(sp.active_gizmo);
                const float lx = inp->mouse.pos.x - vb.x;
                const float ly = inp->mouse.pos.y - vb.y;
                if (nk_input_is_mouse_pressed(inp, NK_BUTTON_LEFT) && !(km & KMOD_SHIFT)) {
                    ag = sp.viewer->HitTestGizmo(lx, ly, vw, vh);
                    if (ag == SpineViewer::GizmoHandle::None) {
                        sp.selected_bone = sp.viewer->HitTestBone(lx, ly, vw, vh);
                        if (!sp.selected_bone.empty()) {
                            ag = SpineViewer::GizmoHandle::Move;
                            sp.scroll_to_bone = true;
                        }
                    }
                }
                if (!nk_input_is_mouse_down(inp, NK_BUTTON_LEFT)) ag = SpineViewer::GizmoHandle::None;
                if (ag != SpineViewer::GizmoHandle::None && !sp.selected_bone.empty() &&
                    nk_input_is_mouse_down(inp, NK_BUTTON_LEFT) && (mdx != 0.0f || mdy != 0.0f))
                {
                    const float s = sp.viewer->GetZoom() > 0.0f ? (1.0f / sp.viewer->GetZoom()) : 1.0f;
                    for (const auto& b : sp.viewer->GetBoneList()) {
                        if (b.name != sp.selected_bone) continue;
                        BoneOverride o{b.x, b.y, b.rotation, b.scale_x, b.scale_y, b.shear_x, b.shear_y};
                        if (ag == SpineViewer::GizmoHandle::Move) {
                            o.x += mdx * s;
                            o.y -= mdy * s;
                        } else if (ag == SpineViewer::GizmoHandle::Rotate) {
                            o.rotation += mdx * 0.5f;
                        } else {
                            o.scale_x += mdx * 0.005f;
                            o.scale_y -= mdy * 0.005f;
                        }
                        sp.viewer->SetBoneOverride(b.name, o);
                        break;
                    }
                }
                sp.active_gizmo = static_cast<int>(ag);
            }
        }

        if (vw > 0 && vh > 0) {
            sp.viewer->Render(vw, vh);
            if (const GLuint ft = sp.viewer->GetFboTexture(); ft != 0) {
                const struct nk_image fimg = nk_image_id(static_cast<int>(ft));
                nk_draw_image(nk_window_get_canvas(ctx), vb, &fimg, nk_rgb(255, 255, 255));
            }
        }
    } else if (sp.viewer && !sp.viewer->GetError().empty()) {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label_colored(ctx, "Error:", NK_TEXT_CENTERED, nk_rgb(255, 100, 100));
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, sp.viewer->GetError().c_str(), NK_TEXT_CENTERED);
    } else {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label(ctx, "Select a skeleton from the list", NK_TEXT_CENTERED);
    }

    nk_group_end(ctx);
}

void SpinePanel::RenderBoneEditor(
    nk_context* ctx,
    float editor_width,
    float panel_height,
    const std::vector<SpineEntry>& entries)
{
    auto& sp = m_state.spine;
    if (!nk_group_begin(ctx, "BoneEditor", NK_WINDOW_BORDER)) return;

    nk_layout_row_dynamic(ctx, 24, 3);
    if (nk_button_label(ctx, "Reset All")) {
        sp.viewer->ResetBoneEdits();
        sp.selected_bone.clear();
    }
    if (nk_button_label(ctx, "Export Modified")) {
        sp.export_pending = true;
    }
    if (sp.viewer->GetTextureList().empty()) {
        nk_spacing(ctx, 1);
    }

    if (sp.export_pending) {
        sp.export_pending = false;
        try {
            if (const std::string dest = Core::FileDialogs::SelectFolder(Core::DialogSlot::Extract, "Select destination folder");
                !dest.empty() && sp.selected_index >= 0 && sp.selected_index < static_cast<int>(entries.size()))
            {
                const std::filesystem::path dest_dir(Core::Utf8ToWString(dest));
                const auto& entry = entries[sp.selected_index];
                int exported = 0;
                if (const std::string mod_json = sp.viewer->GetModifiedSkeletonJson(); !mod_json.empty()) {
                    std::ofstream out(dest_dir / Core::Utf8ToWString(entry.display_name + "_modified.json"));
                    out << mod_json;
                    ++exported;
                }
                exported += ExportAtlasAndImages(entry, dest_dir);
                m_state.tasks.runner.SetStatus(std::format("Exported {} modified files", exported));
            }
        } catch (const std::exception& e) {
            m_state.tasks.runner.SetStatus("Export error: " + std::string(e.what()));
        }
    }

    nk_layout_row_begin(ctx, NK_STATIC, 20, 2);
    nk_layout_row_push(ctx, 50);
    nk_label(ctx, "Filter:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, editor_width - 70);
    nk_edit_string_zero_terminated(
        ctx, NK_EDIT_FIELD, sp.bone_search_buffer.data(), static_cast<int>(sp.bone_search_buffer.size()), nk_filter_default);
    nk_layout_row_end(ctx);
    const std::string bone_query = Core::ToLower(sp.bone_search_buffer.data());

    nk_layout_row_dynamic(ctx, panel_height - 105.0f, 1);
    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(2, 0));
    nk_style_push_vec2(ctx, &ctx->style.window.group_padding, nk_vec2(2, 2));

    if (nk_group_begin(ctx, "BoneList", NK_WINDOW_BORDER)) {
        const auto bones = sp.viewer->GetBoneList();
        float scroll_target_y = -1.0f;

        // O(1) parent lookup map (replaces O(N^2) nested scan per frame)
        std::unordered_set<std::string_view> has_children;
        std::unordered_map<std::string_view, std::string_view> parent_map;
        parent_map.reserve(bones.size());
        for (const auto& b : bones) {
            parent_map.emplace(b.name, b.parent_name);
            if (!b.parent_name.empty()) has_children.insert(b.parent_name);
        }

        for (size_t bi_idx = 0; bi_idx < bones.size(); ++bi_idx) {
            const auto& bi = bones[bi_idx];

            if (!bone_query.empty()) {
                if (Core::ToLower(bi.name).find(bone_query) == std::string::npos) continue;
            } else {
                bool ancestor_collapsed = false;
                std::string_view check = bi.parent_name;
                for (int safety = 0; !check.empty() && safety < 64; ++safety) {
                    if (sp.collapsed_bones.contains(std::string(check))) {
                        ancestor_collapsed = true;
                        break;
                    }
                    const auto it = parent_map.find(check);
                    if (it == parent_map.end()) break;
                    check = it->second;
                }
                if (ancestor_collapsed) continue;
            }

            const bool is_sel = (bi.name == sp.selected_bone);
            const bool is_parent = has_children.contains(bi.name);
            const bool is_collapsed = sp.collapsed_bones.contains(bi.name);

            if (const float indent_px = static_cast<float>(bi.depth) * 10.0f; indent_px > 0.0f) {
                nk_layout_row_begin(ctx, NK_STATIC, 16, 4);
                nk_layout_row_push(ctx, indent_px);
                const struct nk_rect sp_bounds = nk_widget_bounds(ctx);
                nk_command_buffer* canvas = nk_window_get_canvas(ctx);
                const float line_x = sp_bounds.x + indent_px - 6.0f;
                nk_stroke_line(canvas, line_x, sp_bounds.y, line_x, sp_bounds.y + sp_bounds.h, 1.0f, nk_rgb(60, 65, 75));
                nk_stroke_line(canvas, line_x, sp_bounds.y + sp_bounds.h * 0.5f, sp_bounds.x + indent_px, sp_bounds.y + sp_bounds.h * 0.5f, 1.0f, nk_rgb(60, 65, 75));
                nk_spacing(ctx, 1);
                nk_layout_row_push(ctx, (std::max)(40.0f, editor_width - indent_px - 60.0f));
            } else {
                nk_layout_row_begin(ctx, NK_STATIC, 16, 3);
                nk_layout_row_push(ctx, (std::max)(40.0f, editor_width - 60.0f));
            }

            if (is_sel && sp.scroll_to_bone) {
                scroll_target_y = nk_widget_bounds(ctx).y;
            }

            nk_style_button bone_btn = ctx->style.button;
            bone_btn.text_alignment = NK_TEXT_LEFT;
            bone_btn.padding = nk_vec2(3, 0);
            bone_btn.rounding = 1.0f;
            bone_btn.border = 0;
            bone_btn.normal = nk_style_item_color(is_sel ? nk_rgb(50, 70, 110) : nk_rgb(35, 35, 40));
            bone_btn.hover = nk_style_item_color(nk_rgb(55, 65, 80));
            bone_btn.active = bone_btn.hover;
            bone_btn.text_normal = bi.hidden         ? nk_rgb(100, 100, 100)
                                 : bi.has_override   ? nk_rgb(255, 200, 80)
                                 : is_sel            ? nk_rgb(100, 200, 255)
                                                     : nk_rgb(180, 180, 180);
            bone_btn.text_hover = nk_rgb(255, 255, 255);

            const std::string bone_label = is_parent ? ((is_collapsed ? "+ " : "- ") + bi.name) : bi.name;
            if (nk_button_label_styled(ctx, &bone_btn, bone_label.c_str())) {
                if (is_parent && bi.name == sp.selected_bone) {
                    if (is_collapsed) sp.collapsed_bones.erase(bi.name);
                    else sp.collapsed_bones.insert(bi.name);
                }
                sp.selected_bone = bi.name;
                sp.viewer->SetSelectedBoneIndex(static_cast<int>(bi_idx));
                sp.bone_just_reset = false;
            }

            nk_layout_row_push(ctx, 24);
            {
                nk_style_button hb = ctx->style.button;
                hb.rounding = 1.0f;
                hb.border = 0;
                hb.padding = nk_vec2(0, 0);
                hb.normal = nk_style_item_color(bi.hidden ? nk_rgb(120, 50, 50) : nk_rgb(45, 45, 50));
                hb.hover = nk_style_item_color(nk_rgb(80, 60, 60));
                hb.text_normal = nk_rgb(200, 200, 200);
                if (nk_button_label_styled(ctx, &hb, bi.hidden ? "H" : "V")) {
                    sp.viewer->ToggleBoneHidden(bi.name);
                }
            }

            nk_layout_row_push(ctx, 24);
            {
                nk_style_button rb = ctx->style.button;
                rb.padding = nk_vec2(0, 0);
                rb.border = 0;
                rb.rounding = 1.0f;
                if (nk_button_label_styled(ctx, &rb, "R")) {
                    sp.viewer->ResetBone(bi.name);
                    sp.bone_just_reset = true;
                }
            }
            nk_layout_row_end(ctx);

            if (is_sel) {
                if (!bi.has_override && !sp.bone_just_reset) {
                    sp.viewer->SetBoneOverride(bi.name, BoneOverride{
                        bi.setup_x, bi.setup_y, bi.setup_rot,
                        bi.setup_sx, bi.setup_sy, bi.setup_shx, bi.setup_shy
                    });
                }
                if (bi.has_override) sp.bone_just_reset = false;

                static constexpr std::array<const char*, 7> kLabels = {"X", "Y", "Rot", "SclX", "SclY", "ShrX", "ShrY"};
                static constexpr std::array<float, 7> kPxStep = {0.5f, 0.5f, 0.5f, 0.01f, 0.01f, 0.1f, 0.1f};
                static constexpr std::array<float, 7> kSteps = {1.0f, 1.0f, 1.0f, 0.05f, 0.05f, 0.5f, 0.5f};

                const std::array<float, 7> anim = {bi.anim_x, bi.anim_y, bi.anim_rot, bi.anim_sx, bi.anim_sy, bi.anim_shx, bi.anim_shy};
                const std::array<float, 7> setup = {bi.setup_x, bi.setup_y, bi.setup_rot, bi.setup_sx, bi.setup_sy, bi.setup_shx, bi.setup_shy};
                std::array<float, 7> vals = {bi.x, bi.y, bi.rotation, bi.scale_x, bi.scale_y, bi.shear_x, bi.shear_y};

                nk_layout_row_dynamic(ctx, 16, 2);
                nk_label_colored(ctx, "Editing:", NK_TEXT_LEFT, nk_rgb(255, 200, 80));
                {
                    nk_style_button lb = UiStyle::MakeAccentToggleStyle(ctx, sp.link_scale, nk_rgb(56, 120, 74));
                    lb.rounding = 1.0f;
                    lb.padding = nk_vec2(2, 0);
                    lb.border = 0;
                    if (nk_button_label_styled(ctx, &lb, sp.link_scale ? "Scale: Linked" : "Scale: Free")) {
                        sp.link_scale = !sp.link_scale;
                    }
                }

                const float old_sx = vals[3], old_sy = vals[4];
                for (size_t f = 0; f < 7; ++f) {
                    const float range = (std::max)((std::max)(std::abs(vals[f]), std::abs(setup[f])) * 3.0f, 10.0f);
                    nk_layout_row_dynamic(ctx, 18, 1);
                    vals[f] = nk_propertyf(ctx, kLabels[f], -range, vals[f], range, kSteps[f], kPxStep[f]);
                }

                if (sp.link_scale) {
                    const float dsx = vals[3] - old_sx;
                    const float dsy = vals[4] - old_sy;
                    if (dsx != 0.0f && dsy == 0.0f) vals[4] += dsx;
                    if (dsy != 0.0f && dsx == 0.0f) vals[3] += dsy;
                }

                sp.viewer->SetBoneOverride(bi.name, BoneOverride{
                    vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6]
                });

                auto draw_value_grid = [&](const char* heading, nk_color head_col, nk_color val_col, const std::array<float, 7>& arr) {
                    nk_layout_row_dynamic(ctx, 13, 1);
                    nk_label_colored(ctx, heading, NK_TEXT_LEFT, head_col);
                    nk_layout_row_dynamic(ctx, 13, 4);
                    for (size_t f = 0; f < 7; ++f) {
                        const std::string lbl = std::format("{}:{:.1f}", kLabels[f], arr[f]);
                        nk_label_colored(ctx, lbl.c_str(), NK_TEXT_LEFT, val_col);
                        if (f == 3) nk_layout_row_dynamic(ctx, 13, 4);
                    }
                };

                draw_value_grid("Animated:", nk_rgb(100, 180, 255), nk_rgb(80, 150, 220), anim);
                draw_value_grid("Setup:", nk_rgb(100, 200, 100), nk_rgb(80, 170, 80), setup);

                nk_layout_row_dynamic(ctx, 2, 1);
                nk_spacing(ctx, 1);
            }
        }

        if (sp.scroll_to_bone && scroll_target_y >= 0.0f) {
            nk_uint scx = 0, scy = 0;
            nk_group_get_scroll(ctx, "BoneList", &scx, &scy);
            const struct nk_rect content = nk_window_get_content_region(ctx);
            const float rel_y = scroll_target_y - content.y + static_cast<float>(scy);
            const float new_scroll = (std::max)(0.0f, rel_y - content.h * 0.3f);
            nk_group_set_scroll(ctx, "BoneList", scx, static_cast<nk_uint>(new_scroll));
        }
        sp.scroll_to_bone = false;

        nk_group_end(ctx);
    }

    nk_style_pop_vec2(ctx);
    nk_style_pop_vec2(ctx);
    nk_group_end(ctx);
}

void SpinePanel::Render(nk_context* ctx, float content_height, int window_width) {
    auto& sp = m_state.spine;

    if (sp.viewer && sp.viewer->IsLoaded()) {
        float dt = 0.0f;
        if (sp.playing) {
            const Uint64 now = SDL_GetPerformanceCounter();
            if (sp.last_tick > 0) {
                dt = static_cast<float>(now - sp.last_tick) / static_cast<float>(SDL_GetPerformanceFrequency());
                dt *= sp.speed;
            }
            sp.last_tick = now;
        } else {
            sp.last_tick = 0;
        }
        sp.viewer->Update(dt);
    }

    if (sp.building.load()) {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label(ctx, "Building Spine catalog...", NK_TEXT_CENTERED);
        return;
    }

    if (!sp.catalog.IsBuilt()) return;

    const auto& entries = sp.catalog.GetEntries();
    const auto& root_cat = sp.catalog.GetRootCategory();

    nk_layout_row_begin(ctx, NK_STATIC, 28, 3);
    nk_layout_row_push(ctx, 60);
    nk_label(ctx, "Search:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 250);
    nk_edit_string_zero_terminated(
        ctx, NK_EDIT_FIELD, sp.search_buffer.data(), static_cast<int>(sp.search_buffer.size()), nk_filter_default);
    sp.search_query = sp.search_buffer.data();
    nk_layout_row_push(ctx, 200);
    const std::string stats = std::format("{} skeletons", entries.size());
    nk_label_colored(ctx, stats.c_str(), NK_TEXT_LEFT, nk_rgb(150, 200, 255));
    nk_layout_row_end(ctx);

    const auto sw = static_cast<float>(window_width);
    const float sh = content_height - 30.0f;
    const float list_w = sw * 0.22f;
    const float editor_w = sp.edit_mode ? sw * 0.30f : 0.0f;
    const float viewer_w = sw - list_w - editor_w - 50.0f;

    nk_layout_row_begin(ctx, NK_STATIC, sh, sp.edit_mode ? 3 : 2);

    sp.visible_indices.clear();
    nk_layout_row_push(ctx, list_w);
    if (nk_group_begin(ctx, "SpineListInline", NK_WINDOW_BORDER)) {
        const std::string query_lower = Core::ToLower(sp.search_query);
        DrawCategoryTree(ctx, root_cat, entries, query_lower, 0);
        nk_group_end(ctx);
    }

    nk_layout_row_push(ctx, viewer_w);
    RenderViewport(ctx, sh, entries);

    if (sp.edit_mode && sp.viewer && sp.viewer->IsLoaded()) {
        nk_layout_row_push(ctx, editor_w);
        RenderBoneEditor(ctx, editor_w, sh, entries);
    }

    nk_layout_row_end(ctx);
}
