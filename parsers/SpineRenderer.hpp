#pragma once

#include "gui/GlResources.hpp"

#include <GL/glew.h>
#include <spine/spine.h>

#include <array>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

class IArchive;
struct SpineEntry;
class SpineCatalog;

// Custom texture loader backed by pre-registered OpenGL textures.
class SpineTextureLoader final : public spine::TextureLoader {
public:
    struct TexInfo {
        GLuint id = 0;
        int width = 0;
        int height = 0;
    };

    void RegisterTexture(const std::string& name, GLuint texture_id, int width, int height);
    void load(spine::AtlasPage& page, const spine::String& path) override;
    void unload(void* texture) override;
    void ClearTextures();

    [[nodiscard]] const std::map<std::string, TexInfo>& GetTextures() const noexcept { return m_textures; }

private:
    std::map<std::string, TexInfo> m_textures;
};

// Minimal GL 3.3 2D batch renderer for Spine attachments and gizmos.
class SpineBatchRenderer {
public:
    SpineBatchRenderer() = default;
    ~SpineBatchRenderer() { Dispose(); }

    SpineBatchRenderer(const SpineBatchRenderer&) = delete;
    SpineBatchRenderer& operator=(const SpineBatchRenderer&) = delete;

    void Init();
    void Dispose();
    void Begin(const std::array<float, 16>& proj_matrix, bool pma = true);
    void AddTriangles(
        GLuint texture,
        std::span<const float> vertices,
        int vertex_count,
        std::span<const unsigned short> indices,
        spine::BlendMode blend_mode);
    void End();

private:
    struct Vertex {
        float x, y, u, v, r, g, b, a;
    };

    void Flush();

    Gfx::GlShaderProgram m_shader_program;
    Gfx::GlVertexArray m_vao;
    Gfx::GlBuffer m_vbo;
    Gfx::GlBuffer m_ibo;
    GLint m_proj_loc = -1;
    GLint m_tex_loc = -1;

    std::vector<Vertex> m_batch_vertices;
    std::vector<unsigned short> m_batch_indices;
    GLuint m_current_texture = 0;
    spine::BlendMode m_current_blend = spine::BlendMode_Normal;
    std::array<float, 16> m_current_proj{};
    bool m_current_pma = true;
    bool m_initialized = false;
};

struct BoneOverride {
    float x = 0.0f, y = 0.0f;
    float rotation = 0.0f;
    float scale_x = 1.0f, scale_y = 1.0f;
    float shear_x = 0.0f, shear_y = 0.0f;
};

class SpineViewer {
public:
    struct BoneInfo {
        std::string name;
        std::string parent_name;
        int depth = 0;
        // Editable values (override if edited, otherwise setup pose)
        float x = 0, y = 0, rotation = 0, scale_x = 1, scale_y = 1, shear_x = 0, shear_y = 0;
        // Original setup pose
        float setup_x = 0, setup_y = 0, setup_rot = 0, setup_sx = 1, setup_sy = 1, setup_shx = 0, setup_shy = 0;
        // Current live animated values
        float anim_x = 0, anim_y = 0, anim_rot = 0, anim_sx = 1, anim_sy = 1, anim_shx = 0, anim_shy = 0;
        bool has_override = false;
        bool hidden = false;
    };

    enum class GizmoHandle { None, Move, ScaleTL, ScaleTR, ScaleBL, ScaleBR, Rotate };

    struct GizmoState {
        float bbox_min_x = 0, bbox_min_y = 0, bbox_max_x = 0, bbox_max_y = 0;
        bool valid = false;
    };

    struct TextureInfo {
        std::string name;
        int width = 0;
        int height = 0;
        GLuint gl_id = 0;
    };

    SpineViewer() = default;
    ~SpineViewer();

    SpineViewer(const SpineViewer&) = delete;
    SpineViewer& operator=(const SpineViewer&) = delete;

    bool LoadSkeleton(const SpineCatalog& catalog, IArchive& archive, const SpineEntry& entry);
    void Unload();
    void Update(float delta_time);
    void Render(int viewport_width, int viewport_height);

    [[nodiscard]] GLuint GetFboTexture() const noexcept { return m_fbo_texture.Id(); }
    [[nodiscard]] bool IsLoaded() const noexcept { return m_skeleton != nullptr; }

    // Animation / skin
    [[nodiscard]] std::vector<std::string> GetAnimationNames() const;
    [[nodiscard]] std::vector<std::string> GetSkinNames() const;
    void SetAnimation(const std::string& name, bool loop = true);
    void SetSkin(const std::string& name) const;
    void NextAnimation();
    [[nodiscard]] int GetCurrentAnimIndex() const noexcept { return m_current_anim_index; }

    // Playback & view
    void SetPlaybackSpeed(float speed) noexcept { m_playback_speed = speed; }
    [[nodiscard]] float GetPlaybackSpeed() const noexcept { return m_playback_speed; }
    void SetPlaying(bool playing) noexcept { m_playing = playing; }
    [[nodiscard]] bool IsPlaying() const noexcept { return m_playing; }
    void SetAutoplayNext(bool autoplay) noexcept { m_autoplay_next = autoplay; }
    [[nodiscard]] bool GetAutoplayNext() const noexcept { return m_autoplay_next; }
    void SetFlipX(bool flip) noexcept { m_flip_x = flip; }
    void SetFlipY(bool flip) noexcept { m_flip_y = flip; }
    [[nodiscard]] bool GetFlipX() const noexcept { return m_flip_x; }
    [[nodiscard]] bool GetFlipY() const noexcept { return m_flip_y; }
    void SetUsePma(bool pma) noexcept { m_use_pma = pma; }
    [[nodiscard]] bool GetUsePma() const noexcept { return m_use_pma; }
    void SetPremultiplyTextures(bool pm) noexcept { m_premultiply_textures = pm; m_reload_textures = true; }
    [[nodiscard]] bool GetPremultiplyTextures() const noexcept { return m_premultiply_textures; }
    [[nodiscard]] bool NeedsTextureReload() const noexcept { return m_reload_textures; }

    void SetZoom(float z) noexcept;
    [[nodiscard]] float GetZoom() const noexcept { return m_zoom; }
    void ZoomBy(float factor) noexcept { SetZoom(m_zoom * factor); }
    void Pan(float dx, float dy) noexcept { m_pan_x += dx; m_pan_y += dy; }
    void ResetView() noexcept { m_zoom = 1.0f; m_pan_x = 0.0f; m_pan_y = 0.0f; }
    [[nodiscard]] float GetPanX() const noexcept { return m_pan_x; }
    [[nodiscard]] float GetPanY() const noexcept { return m_pan_y; }
    [[nodiscard]] const std::string& GetError() const noexcept { return m_error_msg; }

    void SetBgColor(float r, float g, float b) noexcept { m_bg_r = r; m_bg_g = g; m_bg_b = b; }
    void GetBgColor(float& r, float& g, float& b) const noexcept { r = m_bg_r; g = m_bg_g; b = m_bg_b; }

    // Bone editing
    [[nodiscard]] std::vector<BoneInfo> GetBoneList() const;
    void SetBoneOverride(const std::string& bone_name, const BoneOverride& ovr);
    void ResetBone(const std::string& bone_name);
    void ResetBoneEdits();
    [[nodiscard]] bool HasBoneOverrides() const noexcept { return !m_bone_overrides.empty(); }
    [[nodiscard]] const std::map<std::string, BoneOverride>& GetBoneOverrides() const noexcept { return m_bone_overrides; }
    void ToggleBoneHidden(const std::string& bone_name);
    [[nodiscard]] bool IsBoneHidden(const std::string& bone_name) const;

    // Selection & gizmo
    std::string HitTestBone(float screen_x, float screen_y, int vp_w, int vp_h);
    [[nodiscard]] GizmoState GetSelectedBoneGizmo() const;
    [[nodiscard]] GizmoHandle HitTestGizmo(float screen_x, float screen_y, int vp_w, int vp_h) const;
    void SetSelectedBoneIndex(int index) noexcept { m_selected_bone_index = index; }
    [[nodiscard]] int GetSelectedBoneIndex() const noexcept { return m_selected_bone_index; }

    // Textures
    [[nodiscard]] std::vector<TextureInfo> GetTextureList() const;
    bool SwapTexture(const std::string& page_name, const std::string& png_path);
    void ResetTextureSwaps() { m_texture_swaps.clear(); }
    [[nodiscard]] bool HasTextureSwaps() const noexcept { return !m_texture_swaps.empty(); }

    // Export
    [[nodiscard]] std::string GetModifiedSkeletonJson() const;

private:
    void EnsureFbo(int width, int height);
    void CleanupFbo();
    GLuint LoadTextureFromRgba(const unsigned char* data, int width, int height);
    void EnsureGizmoTexture();
    void ComputeStableBounds();
    void ScreenToWorld(float sx, float sy, int vp_w, int vp_h, float& wx, float& wy) const noexcept;
    void ApplyBoneOverrides();
    void RenderSelectedBoneGizmo();

    SpineTextureLoader m_texture_loader;
    std::unique_ptr<spine::Atlas> m_atlas;
    std::unique_ptr<spine::SkeletonData> m_skeleton_data;
    std::unique_ptr<spine::Skeleton> m_skeleton;
    std::unique_ptr<spine::AnimationStateData> m_state_data;
    std::unique_ptr<spine::AnimationState> m_anim_state;
    spine::SkeletonClipping m_clipper;

    SpineBatchRenderer m_batch_renderer;

    Gfx::GlFramebuffer m_fbo;
    Gfx::GlTexture m_fbo_texture;
    Gfx::GlTexture m_gizmo_texture;
    int m_fbo_width = 0;
    int m_fbo_height = 0;

    // Reusable per-frame scratch buffers (avoids function-local statics and fixed limits)
    mutable std::vector<float> m_world_vertices;
    std::vector<float> m_mesh_vertices;

    float m_playback_speed = 1.0f;
    bool m_playing = true;
    bool m_flip_x = false;
    bool m_flip_y = false;
    float m_zoom = 1.0f;
    float m_pan_x = 0.0f;
    float m_pan_y = 0.0f;
    bool m_use_pma = true;
    bool m_premultiply_textures = false;
    bool m_reload_textures = false;
    bool m_autoplay_next = false;
    int m_current_anim_index = 0;
    int m_selected_bone_index = -1;
    std::string m_error_msg;
    float m_bg_r = 0.0f, m_bg_g = 0.0f, m_bg_b = 0.0f;

    float m_cached_bounds_x = 0.0f, m_cached_bounds_y = 0.0f;
    float m_cached_bounds_w = 0.0f, m_cached_bounds_h = 0.0f;
    bool m_bounds_computed = false;

    std::map<std::string, BoneOverride> m_bone_overrides;
    std::set<std::string> m_hidden_bones;
    std::map<std::string, std::string> m_texture_swaps;
    std::string m_original_json;

    std::vector<Gfx::GlTexture> m_owned_textures;
};
