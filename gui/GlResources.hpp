#pragma once

#include <GL/glew.h>
#include <SDL.h>

#include <memory>
#include <utility>

namespace Gfx {

    template <auto DeleteFn>
    class GlHandle {
    public:
        GlHandle() = default;
        explicit GlHandle(GLuint id) noexcept : m_id(id) {}
        ~GlHandle() { Reset(); }

        GlHandle(const GlHandle&) = delete;
        GlHandle& operator=(const GlHandle&) = delete;

        GlHandle(GlHandle&& other) noexcept
            : m_id(std::exchange(other.m_id, 0)) {}

        GlHandle& operator=(GlHandle&& other) noexcept {
            if (this != &other) {
                Reset();
                m_id = std::exchange(other.m_id, 0);
            }
            return *this;
        }

        void Reset(GLuint new_id = 0) noexcept {
            if (m_id != 0) {
                DeleteFn(m_id);
            }
            m_id = new_id;
        }

        [[nodiscard]] GLuint Id() const noexcept { return m_id; }
        [[nodiscard]] explicit operator bool() const noexcept { return m_id != 0; }

    private:
        GLuint m_id = 0;
    };

    inline void DeleteTexture(GLuint id) noexcept { glDeleteTextures(1, &id); }
    inline void DeleteFramebuffer(GLuint id) noexcept { glDeleteFramebuffers(1, &id); }
    inline void DeleteVertexArray(GLuint id) noexcept { glDeleteVertexArrays(1, &id); }
    inline void DeleteBuffer(GLuint id) noexcept { glDeleteBuffers(1, &id); }
    inline void DeleteProgram(GLuint id) noexcept { glDeleteProgram(id); }

    using GlTexture = GlHandle<DeleteTexture>;
    using GlFramebuffer = GlHandle<DeleteFramebuffer>;
    using GlVertexArray = GlHandle<DeleteVertexArray>;
    using GlBuffer = GlHandle<DeleteBuffer>;
    using GlShaderProgram = GlHandle<DeleteProgram>;

    struct SdlSurfaceDeleter {
        void operator()(SDL_Surface* surface) const noexcept {
            if (surface) SDL_FreeSurface(surface);
        }
    };

    struct SdlTextureDeleter {
        void operator()(SDL_Texture* tex) const noexcept {
            if (tex) SDL_DestroyTexture(tex);
        }
    };

    struct SdlRendererDeleter {
        void operator()(SDL_Renderer* renderer) const noexcept {
            if (renderer) SDL_DestroyRenderer(renderer);
        }
    };

    struct SdlWindowDeleter {
        void operator()(SDL_Window* window) const noexcept {
            if (window) SDL_DestroyWindow(window);
        }
    };

    struct SdlCursorDeleter {
        void operator()(SDL_Cursor* cursor) const noexcept {
            if (cursor) SDL_FreeCursor(cursor);
        }
    };

    using SdlSurfacePtr = std::unique_ptr<SDL_Surface, SdlSurfaceDeleter>;
    using SdlTexturePtr = std::unique_ptr<SDL_Texture, SdlTextureDeleter>;
    using SdlRendererPtr = std::unique_ptr<SDL_Renderer, SdlRendererDeleter>;
    using SdlWindowPtr = std::unique_ptr<SDL_Window, SdlWindowDeleter>;
    using SdlCursorPtr = std::unique_ptr<SDL_Cursor, SdlCursorDeleter>;

} // namespace Gfx
