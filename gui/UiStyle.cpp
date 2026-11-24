#include "gui/UiStyle.hpp"

namespace UiStyle {

    nk_style_button MakeToolbarButtonStyle(const nk_context* ctx) {
        nk_style_button style = ctx->style.button;
        style.rounding = 4.0f;
        style.padding = nk_vec2(10, 8);
        style.normal = nk_style_item_color(nk_rgb(70, 70, 75));
        style.hover = nk_style_item_color(nk_rgb(90, 90, 95));
        return style;
    }

    nk_style_button MakeToggleStyle(const nk_context* ctx, bool is_on) {
        nk_style_button style = ctx->style.button;
        if (is_on) {
            style.normal = nk_style_item_color(nk_rgb(56, 120, 74));
            style.hover = nk_style_item_color(nk_rgb(66, 138, 86));
            style.active = nk_style_item_color(nk_rgb(50, 108, 66));
        } else {
            style.normal = nk_style_item_color(nk_rgb(100, 64, 64));
            style.hover = nk_style_item_color(nk_rgb(120, 74, 74));
            style.active = nk_style_item_color(nk_rgb(88, 56, 56));
        }
        style.text_normal = nk_rgb(240, 240, 240);
        style.text_hover = nk_rgb(255, 255, 255);
        style.text_active = nk_rgb(255, 255, 255);
        return style;
    }

    nk_style_button MakeAccentToggleStyle(const nk_context* ctx, bool is_on, nk_color active_color) {
        nk_style_button style = ctx->style.button;
        style.rounding = 3.0f;
        if (is_on) {
            style.normal = nk_style_item_color(active_color);
            style.hover = nk_style_item_color(nk_rgb(
                static_cast<int>(active_color.r) + 12 > 255 ? 255 : active_color.r + 12,
                static_cast<int>(active_color.g) + 12 > 255 ? 255 : active_color.g + 12,
                static_cast<int>(active_color.b) + 12 > 255 ? 255 : active_color.b + 12));
        } else {
            style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
            style.hover = nk_style_item_color(nk_rgb(75, 75, 80));
        }
        style.text_normal = nk_rgb(220, 220, 220);
        style.text_hover = nk_rgb(255, 255, 255);
        return style;
    }

    nk_style_button MakeTreeButtonStyle(const nk_context* ctx, bool is_selected, int depth, nk_color text_color) {
        nk_color bg = (depth % 2 == 0) ? nk_rgb(35, 35, 38) : nk_rgb(40, 40, 43);
        if (is_selected) bg = nk_rgb(65, 65, 70);

        nk_style_button style = ctx->style.button;
        style.normal = nk_style_item_color(bg);
        style.hover = nk_style_item_color(is_selected ? nk_rgb(85, 85, 95) : nk_rgb(50, 50, 55));
        style.active = nk_style_item_color(nk_rgb(70, 70, 80));
        style.text_normal = text_color;
        style.text_hover = nk_rgb(255, 255, 255);
        style.text_active = nk_rgb(255, 255, 255);
        style.text_alignment = NK_TEXT_LEFT;
        style.padding = nk_vec2(8, 4);
        style.rounding = 3.0f;
        return style;
    }

    nk_style_button MakeTreeExpandStyle(const nk_context* ctx) {
        nk_style_button style = ctx->style.button;
        style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
        style.hover = nk_style_item_color(nk_rgb(80, 80, 85));
        style.text_normal = nk_rgb(200, 200, 200);
        style.text_hover = nk_rgb(255, 255, 255);
        style.rounding = 3.0f;
        return style;
    }

    nk_color GetDiffStatusColor(DiffStatus status, bool is_selected, bool is_folder) {
        if (is_selected) return nk_rgb(255, 255, 255);
        switch (status) {
            case DiffStatus::Added:    return nk_rgb(100, 255, 100);
            case DiffStatus::Modified: return nk_rgb(255, 200, 50);
            case DiffStatus::Removed:  return nk_rgb(255, 100, 100);
            default:                   return is_folder ? nk_rgb(220, 220, 220) : nk_rgb(200, 200, 200);
        }
    }

    bool DrawTreeRow(
        nk_context* ctx,
        int depth,
        bool is_folder,
        bool is_expanded,
        bool is_selected,
        const std::string& label,
        const std::string& info,
        nk_color text_color,
        bool& out_toggle_expand,
        bool* out_right_clicked,
        struct nk_vec2* out_mouse_pos)
    {
        out_toggle_expand = false;
        if (out_right_clicked) *out_right_clicked = false;

        nk_layout_row_begin(ctx, NK_STATIC, 26, 4);

        nk_layout_row_push(ctx, depth * 16.0f + 10.0f);
        nk_spacing(ctx, 1);

        nk_layout_row_push(ctx, 24.0f);
        if (is_folder) {
            const nk_style_button expand_style = MakeTreeExpandStyle(ctx);
            if (nk_button_label_styled(ctx, &expand_style, is_expanded ? "-" : "+")) {
                out_toggle_expand = true;
            }
        } else {
            nk_spacing(ctx, 1);
        }

        nk_layout_row_push(ctx, 370.0f);
        const nk_style_button button_style = MakeTreeButtonStyle(ctx, is_selected, depth, text_color);
        const bool clicked = nk_button_label_styled(ctx, &button_style, label.c_str()) != 0;

        if (out_right_clicked && out_mouse_pos) {
            if (nk_input_is_mouse_hovering_rect(&ctx->input, nk_widget_bounds(ctx)) &&
                nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_RIGHT))
            {
                *out_right_clicked = true;
                *out_mouse_pos = ctx->input.mouse.pos;
            }
        }

        nk_layout_row_push(ctx, 200.0f);
        nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

        nk_layout_row_end(ctx);
        return clicked;
    }

} // namespace UiStyle
