#include "gui/ImageWindow.hpp"
#include "gui/PreviewManager.hpp"

#include <algorithm>
#include <format>

bool ImageWindow::Open(IArchive& archive, const Core::FileNode& node, std::string& out_error) {
    Close();

    try {
        Gfx::SdlSurfacePtr surface = PreviewManager::LoadSurface(archive, node, out_error);
        if (!surface) return false;

        const int orig_w = surface->w;
        const int orig_h = surface->h;

        SDL_DisplayMode display_mode{};
        SDL_GetCurrentDisplayMode(0, &display_mode);
        const int max_w = static_cast<int>(display_mode.w * 0.9f);
        const int max_h = static_cast<int>(display_mode.h * 0.9f);

        int win_w = orig_w;
        int win_h = orig_h;
        if (win_w > max_w || win_h > max_h) {
            const float scale = (std::min)(
                static_cast<float>(max_w) / static_cast<float>(orig_w),
                static_cast<float>(max_h) / static_cast<float>(orig_h));
            win_w = static_cast<int>(orig_w * scale);
            win_h = static_cast<int>(orig_h * scale);
        }

        m_title = std::format("{} ({}x{})", node.name, orig_w, orig_h);
        m_window.reset(SDL_CreateWindow(
            m_title.c_str(),
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_w, win_h, SDL_WINDOW_SHOWN));
        if (!m_window) {
            out_error = "Failed to create preview window";
            return false;
        }

        m_renderer.reset(SDL_CreateRenderer(
            m_window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
        if (!m_renderer) {
            Close();
            out_error = "Failed to create renderer";
            return false;
        }

        m_texture.reset(SDL_CreateTextureFromSurface(m_renderer.get(), surface.get()));
        if (!m_texture) {
            Close();
            out_error = "Failed to create texture";
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        Close();
        out_error = std::string("Error opening image window: ") + e.what();
        return false;
    }
}

void ImageWindow::Render() {
    if (!m_window || !m_renderer || !m_texture) return;
    SDL_RenderClear(m_renderer.get());
    SDL_RenderCopy(m_renderer.get(), m_texture.get(), nullptr, nullptr);
    SDL_RenderPresent(m_renderer.get());
}

void ImageWindow::Close() noexcept {
    m_texture.reset();
    m_renderer.reset();
    m_window.reset();
    m_title.clear();
}

bool ImageWindow::MatchesWindowId(Uint32 window_id) const noexcept {
    return m_window != nullptr && SDL_GetWindowID(m_window.get()) == window_id;
}
