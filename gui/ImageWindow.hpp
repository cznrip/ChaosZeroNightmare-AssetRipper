#pragma once

#include "archive/IArchive.hpp"
#include "core/Core.hpp"
#include "gui/GlResources.hpp"

#include <SDL.h>
#include <string>

/**
 * RAII manager for the detached SDL image preview window.
 */
class ImageWindow {
public:
    ImageWindow() = default;
    ~ImageWindow() { Close(); }

    ImageWindow(const ImageWindow&) = delete;
    ImageWindow& operator=(const ImageWindow&) = delete;

    bool Open(IArchive& archive, const Core::FileNode& node, std::string& out_error);
    void Render();
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return m_window != nullptr; }
    [[nodiscard]] bool MatchesWindowId(Uint32 window_id) const noexcept;

private:
    // Destruction order matters: texture -> renderer -> window (reverse of member declaration).
    Gfx::SdlWindowPtr m_window;
    Gfx::SdlRendererPtr m_renderer;
    Gfx::SdlTexturePtr m_texture;
    std::string m_title;
};
