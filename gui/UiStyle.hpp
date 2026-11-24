#pragma once

#include "services/FileMap.hpp"

#ifndef NK_NUKLEAR_H_
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#endif
#include "nuklear.h"

#include <string>

namespace UiStyle {

    [[nodiscard]] nk_style_button MakeToolbarButtonStyle(const nk_context* ctx);
    [[nodiscard]] nk_style_button MakeToggleStyle(const nk_context* ctx, bool is_on);
    [[nodiscard]] nk_style_button MakeAccentToggleStyle(const nk_context* ctx, bool is_on, nk_color active_color);
    [[nodiscard]] nk_style_button MakeTreeButtonStyle(const nk_context* ctx, bool is_selected, int depth, nk_color text_color);
    [[nodiscard]] nk_style_button MakeTreeExpandStyle(const nk_context* ctx);
    [[nodiscard]] nk_color GetDiffStatusColor(DiffStatus status, bool is_selected, bool is_folder);

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
        bool* out_right_clicked = nullptr,
        struct nk_vec2* out_mouse_pos = nullptr);

} // namespace UiStyle
