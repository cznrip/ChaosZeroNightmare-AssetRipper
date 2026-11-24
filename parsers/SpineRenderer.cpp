#define NOMINMAX
#include "parsers/SpineRenderer.hpp"
#include "archive/IArchive.hpp"
#include "core/AssetFormat.hpp"
#include "core/Logger.hpp"
#include "core/StringUtils.hpp"
#include "parsers/ScspParser.hpp"
#include "parsers/SctParser.hpp"
#include "parsers/SpineCatalog.hpp"

#include "json.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

// spine-cpp requires this global hook.
spine::SpineExtension* spine::getDefaultExtension() {
    return new spine::DefaultSpineExtension();
}

// ============================================================================
// SpineTextureLoader
// ============================================================================

void SpineTextureLoader::RegisterTexture(const std::string& name, GLuint texture_id, int width, int height) {
    m_textures[name] = {texture_id, width, height};
}

void SpineTextureLoader::load(spine::AtlasPage& page, const spine::String& path) {
    const std::string name(path.buffer());
    auto it = m_textures.find(name);

    if (it == m_textures.end()) {
        if (name.ends_with(".png")) {
            it = m_textures.find(Core::ReplaceExtension(name, ".sct"));
            if (it == m_textures.end()) {
                it = m_textures.find(Core::ReplaceExtension(name, ".sct2"));
            }
        } else if (name.ends_with(".sct") || name.ends_with(".sct2")) {
            it = m_textures.find(Core::ReplaceExtension(name, ".png"));
        }
    }

    if (it != m_textures.end()) {
        page.setRendererObject(reinterpret_cast<void*>(static_cast<uintptr_t>(it->second.id)));
        page.width = it->second.width;
        page.height = it->second.height;
    }
}

void SpineTextureLoader::unload(void*) {
    // Textures are owned by SpineViewer::m_owned_textures via RAII.
}

void SpineTextureLoader::ClearTextures() {
    m_textures.clear();
}

// ============================================================================
// SpineBatchRenderer
// ============================================================================

namespace {

    constexpr const char* kVertSrc = R"(
        #version 330 core
        layout(location=0) in vec2 aPos;
        layout(location=1) in vec2 aUV;
        layout(location=2) in vec4 aColor;
        uniform mat4 uProj;
        out vec2 vUV;
        out vec4 vColor;
        void main() {
            gl_Position = uProj * vec4(aPos, 0.0, 1.0);
            vUV = aUV;
            vColor = aColor;
        }
    )";

    constexpr const char* kFragSrc = R"(
        #version 330 core
        in vec2 vUV;
        in vec4 vColor;
        uniform sampler2D uTex;
        out vec4 FragColor;
        void main() {
            FragColor = texture(uTex, vUV) * vColor;
        }
    )";

    GLuint CompileShader(GLenum type, const char* src) {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    }

} // namespace

void SpineBatchRenderer::Init() {
    if (m_initialized) return;

    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertSrc);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragSrc);
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    m_shader_program.Reset(prog);

    m_proj_loc = glGetUniformLocation(m_shader_program.Id(), "uProj");
    m_tex_loc = glGetUniformLocation(m_shader_program.Id(), "uTex");

    GLuint vao = 0, vbo = 0, ibo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);
    m_vao.Reset(vao);
    m_vbo.Reset(vbo);
    m_ibo.Reset(ibo);

    glBindVertexArray(m_vao.Id());
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo.Id());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo.Id());

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(4 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    m_initialized = true;
}

void SpineBatchRenderer::Dispose() {
    if (!m_initialized) return;
    m_shader_program.Reset();
    m_vao.Reset();
    m_vbo.Reset();
    m_ibo.Reset();
    m_initialized = false;
}

void SpineBatchRenderer::Begin(const std::array<float, 16>& proj_matrix, bool pma) {
    m_current_proj = proj_matrix;
    m_current_pma = pma;
    m_batch_vertices.clear();
    m_batch_indices.clear();
    m_current_texture = 0;
    m_current_blend = spine::BlendMode_Normal;
}

void SpineBatchRenderer::AddTriangles(
    GLuint texture,
    std::span<const float> vertices,
    int vertex_count,
    std::span<const unsigned short> indices,
    spine::BlendMode blend_mode)
{
    if (texture != m_current_texture || blend_mode != m_current_blend) {
        Flush();
        m_current_texture = texture;
        m_current_blend = blend_mode;
    }

    const auto base_vertex = static_cast<unsigned short>(m_batch_vertices.size());
    for (int i = 0; i < vertex_count; ++i) {
        const size_t off = static_cast<size_t>(i) * 8;
        m_batch_vertices.push_back(Vertex{
            vertices[off + 0], vertices[off + 1],
            vertices[off + 2], vertices[off + 3],
            vertices[off + 4], vertices[off + 5],
            vertices[off + 6], vertices[off + 7]
        });
    }

    for (const unsigned short idx : indices) {
        m_batch_indices.push_back(static_cast<unsigned short>(base_vertex + idx));
    }
}

void SpineBatchRenderer::Flush() {
    if (m_batch_indices.empty()) return;

    glUseProgram(m_shader_program.Id());
    glUniformMatrix4fv(m_proj_loc, 1, GL_FALSE, m_current_proj.data());
    glUniform1i(m_tex_loc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_current_texture);

    glBindVertexArray(m_vao.Id());
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo.Id());
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_batch_vertices.size() * sizeof(Vertex)), m_batch_vertices.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo.Id());
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_batch_indices.size() * sizeof(unsigned short)), m_batch_indices.data(), GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    if (m_current_pma) {
        switch (m_current_blend) {
            case spine::BlendMode_Additive: glBlendFunc(GL_ONE, GL_ONE); break;
            case spine::BlendMode_Multiply: glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case spine::BlendMode_Screen:   glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
            default:                        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        }
    } else {
        switch (m_current_blend) {
            case spine::BlendMode_Additive: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
            case spine::BlendMode_Multiply: glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case spine::BlendMode_Screen:   glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
            default:                        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        }
    }

    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_batch_indices.size()), GL_UNSIGNED_SHORT, nullptr);

    glBindVertexArray(0);
    m_batch_vertices.clear();
    m_batch_indices.clear();
}

void SpineBatchRenderer::End() {
    Flush();
}

// ============================================================================
// SpineViewer
// ============================================================================

SpineViewer::~SpineViewer() {
    Unload();
    m_batch_renderer.Dispose();
    CleanupFbo();
}

void SpineViewer::SetZoom(float z) noexcept {
    m_zoom = std::clamp(z, 0.1f, 10.0f);
}

void SpineViewer::Unload() {
    // Destroy Spine objects in reverse dependency order.
    m_anim_state.reset();
    m_state_data.reset();
    m_skeleton.reset();
    m_skeleton_data.reset();
    m_atlas.reset();

    m_owned_textures.clear();
    m_texture_loader.ClearTextures();
    m_bone_overrides.clear();
    m_hidden_bones.clear();
    m_texture_swaps.clear();
    m_original_json.clear();
    m_error_msg.clear();
    m_selected_bone_index = -1;
    m_bounds_computed = false;
}

GLuint SpineViewer::LoadTextureFromRgba(const unsigned char* data, int width, int height) {
    GLuint raw_tex = 0;
    glGenTextures(1, &raw_tex);
    Gfx::GlTexture tex(raw_tex);

    glBindTexture(GL_TEXTURE_2D, tex.Id());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (m_premultiply_textures) {
        const size_t pixel_count = static_cast<size_t>(width) * height;
        std::vector<unsigned char> pma(pixel_count * 4);
        for (size_t i = 0; i < pixel_count; ++i) {
            const unsigned int a = data[i * 4 + 3];
            pma[i * 4 + 0] = static_cast<unsigned char>((data[i * 4 + 0] * a + 127) / 255);
            pma[i * 4 + 1] = static_cast<unsigned char>((data[i * 4 + 1] * a + 127) / 255);
            pma[i * 4 + 2] = static_cast<unsigned char>((data[i * 4 + 2] * a + 127) / 255);
            pma[i * 4 + 3] = static_cast<unsigned char>(a);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pma.data());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    }

    const GLuint id = tex.Id();
    m_owned_textures.push_back(std::move(tex));
    return id;
}

void SpineViewer::EnsureGizmoTexture() {
    if (m_gizmo_texture) return;
    constexpr std::array<unsigned char, 4> kWhite = {255, 255, 255, 255};
    GLuint raw = 0;
    glGenTextures(1, &raw);
    m_gizmo_texture.Reset(raw);
    glBindTexture(GL_TEXTURE_2D, m_gizmo_texture.Id());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kWhite.data());
}

bool SpineViewer::LoadSkeleton(const SpineCatalog& catalog, IArchive& archive, const SpineEntry& entry) {
    Unload();
    m_batch_renderer.Init();

    catalog.EnsureDetailsLoaded(archive, entry);

    // 1. Read and convert SCSP to JSON
    std::string json_str;
    try {
        const auto scsp_data = archive.GetFileData(*entry.scsp_node);
        if (scsp_data.empty()) {
            m_error_msg = "Failed to read SCSP file";
            LogError("SpineViewer: " + m_error_msg);
            return false;
        }
        json_str = ScspParser::ConvertToJson(scsp_data);
        if (json_str.empty()) {
            m_error_msg = "Failed to convert SCSP to JSON";
            LogError("SpineViewer: " + m_error_msg);
            return false;
        }
        m_original_json = json_str;
    } catch (const std::exception& e) {
        m_error_msg = std::string("SCSP parse error: ") + e.what();
        LogError("SpineViewer: " + m_error_msg);
        return false;
    }

    // 2. Read atlas text
    std::string atlas_str;
    if (entry.atlas_node) {
        if (const auto atlas_data = archive.GetFileData(*entry.atlas_node); !atlas_data.empty()) {
            atlas_str.assign(atlas_data.begin(), atlas_data.end());
        }
    }
    if (atlas_str.empty()) {
        m_error_msg = "No atlas file found for this skeleton";
        LogError("SpineViewer: " + m_error_msg);
        return false;
    }

    // 3. Load texture images and register with texture loader
    for (const auto* img_node : entry.image_nodes) {
        const auto* fi = std::get_if<Core::FileInfo>(&img_node->data);
        if (!fi) continue;

        const auto file_data = archive.GetFileData(*img_node);
        if (file_data.empty()) {
            LogError("SpineViewer: empty texture data for " + img_node->name);
            continue;
        }

        int w = 0, h = 0;
        GLuint tex = 0;
        try {
            if (Core::IsSctExtension(fi->format)) {
                const auto [rgba, width, height] = SctParser::ConvertToRgba(file_data);
                if (rgba.empty()) {
                    LogError("SpineViewer: SCT decode failed for " + img_node->name);
                    continue;
                }
                w = width;
                h = height;
                tex = LoadTextureFromRgba(rgba.data(), w, h);
            } else {
                SDL_RWops* rw = SDL_RWFromConstMem(file_data.data(), static_cast<int>(file_data.size()));
                if (!rw) continue;
                Gfx::SdlSurfacePtr surface(IMG_Load_RW(rw, 1));
                if (!surface) {
                    LogError("SpineViewer: IMG_Load failed for " + img_node->name);
                    continue;
                }
                Gfx::SdlSurfacePtr rgba(SDL_ConvertSurfaceFormat(surface.get(), SDL_PIXELFORMAT_ABGR8888, 0));
                if (!rgba) continue;
                w = rgba->w;
                h = rgba->h;
                tex = LoadTextureFromRgba(static_cast<const unsigned char*>(rgba->pixels), w, h);
            }
        } catch (const std::exception& e) {
            LogError("SpineViewer: texture load exception for " + img_node->name + ": " + e.what());
            continue;
        }

        if (tex != 0) {
            m_texture_loader.RegisterTexture(img_node->name, tex, w, h);
            m_texture_loader.RegisterTexture(Core::ReplaceExtension(img_node->name, ".png"), tex, w, h);
        }
    }

    // 4. Create spine Atlas
    try {
        const std::string atlas_fixed = Core::RewriteAtlasExtensions(atlas_str);
        if (atlas_fixed.size() < 50 || m_owned_textures.empty()) {
            m_error_msg = std::format("Atlas too small ({} bytes) or no textures loaded", atlas_fixed.size());
            LogError("SpineViewer: " + m_error_msg);
            return false;
        }

        m_atlas = std::make_unique<spine::Atlas>(
            atlas_fixed.c_str(), static_cast<int>(atlas_fixed.size()), "", &m_texture_loader, true);
        if (m_atlas->getPages().size() == 0) {
            m_error_msg = "Atlas has no texture pages (textures missing or failed to load)";
            LogError("SpineViewer: " + m_error_msg);
            m_atlas.reset();
            return false;
        }
    } catch (const std::exception& e) {
        m_error_msg = std::string("Failed to create spine Atlas: ") + e.what();
        LogError("SpineViewer: " + m_error_msg);
        return false;
    } catch (...) {
        m_error_msg = "Failed to create spine Atlas (unknown error)";
        LogError("SpineViewer: " + m_error_msg);
        return false;
    }

    // 5. Load skeleton data from JSON
    try {
        spine::SkeletonJson json_loader(m_atlas.get());
        json_loader.setScale(1.0f);
        m_skeleton_data.reset(json_loader.readSkeletonData(json_str.c_str()));
        if (!m_skeleton_data) {
            m_error_msg = "Failed to parse skeleton JSON";
            LogError("SpineViewer: " + m_error_msg);
            return false;
        }
    } catch (const std::exception& e) {
        m_error_msg = std::string("Exception loading skeleton: ") + e.what();
        LogError("SpineViewer: " + m_error_msg);
        return false;
    } catch (...) {
        m_error_msg = "Exception loading skeleton data (unknown)";
        LogError("SpineViewer: " + m_error_msg);
        return false;
    }

    // 6. Create skeleton and animation state
    try {
        m_skeleton = std::make_unique<spine::Skeleton>(m_skeleton_data.get());
        m_skeleton->setToSetupPose();
        m_skeleton->updateWorldTransform();

        m_state_data = std::make_unique<spine::AnimationStateData>(m_skeleton_data.get());
        m_state_data->setDefaultMix(0.2f);
        m_anim_state = std::make_unique<spine::AnimationState>(m_state_data.get());

        if (auto& anims = m_skeleton_data->getAnimations(); anims.size() > 0) {
            m_anim_state->setAnimation(0, anims[0]->getName(), true);
        }
    } catch (const std::exception& e) {
        m_error_msg = std::string("Exception creating skeleton instance: ") + e.what();
        LogError("SpineViewer: " + m_error_msg);
        return false;
    }

    ComputeStableBounds();
    return true;
}

void SpineViewer::ComputeStableBounds() {
    if (!m_skeleton) return;

    spine::Vector<float> vbuf;
    float bx = 0, by = 0, bw = 0, bh = 0;
    m_skeleton->getBounds(bx, by, bw, bh, vbuf);
    if (bw > 10.0f && bh > 10.0f) {
        m_cached_bounds_x = bx;
        m_cached_bounds_y = by;
        m_cached_bounds_w = bw;
        m_cached_bounds_h = bh;
    } else if (m_skeleton_data->getWidth() > 10.0f && m_skeleton_data->getHeight() > 10.0f) {
        m_cached_bounds_w = m_skeleton_data->getWidth();
        m_cached_bounds_h = m_skeleton_data->getHeight();
        m_cached_bounds_x = -m_cached_bounds_w * 0.5f;
        m_cached_bounds_y = -m_cached_bounds_h * 0.5f;
    } else {
        m_cached_bounds_w = 800.0f;
        m_cached_bounds_h = 800.0f;
        m_cached_bounds_x = -400.0f;
        m_cached_bounds_y = -400.0f;
    }

    m_bounds_computed = true;
    ResetView();
}

void SpineViewer::ScreenToWorld(float sx, float sy, int vp_w, int vp_h, float& wx, float& wy) const noexcept {
    const float pad_x = m_cached_bounds_w * 0.1f;
    const float pad_y = m_cached_bounds_h * 0.1f;
    const float left = m_cached_bounds_x - pad_x - m_pan_x;
    const float right = m_cached_bounds_x + m_cached_bounds_w + pad_x - m_pan_x;
    const float bottom = m_cached_bounds_y - pad_y - m_pan_y;
    const float top = m_cached_bounds_y + m_cached_bounds_h + pad_y - m_pan_y;

    const float cx = (left + right) * 0.5f;
    const float cy = (bottom + top) * 0.5f;
    float hw = (right - left) / (2.0f * m_zoom);
    float hh = (top - bottom) / (2.0f * m_zoom);

    const float view_aspect = static_cast<float>(vp_w) / static_cast<float>(vp_h);
    if (const float bounds_aspect = hw / hh; bounds_aspect > view_aspect) {
        hh = hw / view_aspect;
    } else {
        hw = hh * view_aspect;
    }

    wx = cx - hw + (sx / static_cast<float>(vp_w)) * 2.0f * hw;
    wy = cy + hh - (sy / static_cast<float>(vp_h)) * 2.0f * hh;
}

std::string SpineViewer::HitTestBone(float screen_x, float screen_y, int vp_w, int vp_h) {
    if (!m_skeleton) return {};

    float wx = 0, wy = 0;
    ScreenToWorld(screen_x, screen_y, vp_w, vp_h, wx, wy);

    auto& bones = m_skeleton->getBones();
    float best_dist = 30.0f * (m_cached_bounds_w / (static_cast<float>(vp_w) * m_zoom));
    std::string best_name;
    int best_idx = -1;

    for (size_t i = 0; i < bones.size(); ++i) {
        const float dx = wx - bones[i]->getWorldX();
        const float dy = wy - bones[i]->getWorldY();
        if (const float dist = std::hypot(dx, dy); dist < best_dist) {
            best_dist = dist;
            best_name = bones[i]->getData().getName().buffer();
            best_idx = static_cast<int>(i);
        }
    }

    m_selected_bone_index = best_idx;
    return best_name;
}

void SpineViewer::Update(float delta_time) {
    if (!m_skeleton || !m_anim_state) return;
    try {
        m_skeleton->setScaleX(m_flip_x ? -1.0f : 1.0f);
        m_skeleton->setScaleY(m_flip_y ? -1.0f : 1.0f);

        if (delta_time > 0.0f) {
            m_anim_state->update(delta_time);
        }
        m_anim_state->apply(*m_skeleton);

        if (m_autoplay_next && delta_time > 0.0f) {
            if (spine::TrackEntry* track = m_anim_state->getCurrent(0);
                track && !track->getLoop() && track->isComplete())
            {
                NextAnimation();
            }
        }

        ApplyBoneOverrides();
        m_skeleton->updateWorldTransform();
    } catch (const std::exception& e) {
        LogError(std::string("SpineViewer::Update exception: ") + e.what());
    } catch (...) {
        LogError("SpineViewer::Update unknown exception");
    }
}

void SpineViewer::ApplyBoneOverrides() {
    if (!m_skeleton) return;
    for (const auto& [name, ovr] : m_bone_overrides) {
        if (spine::Bone* bone = m_skeleton->findBone(spine::String(name.c_str()))) {
            bone->setX(ovr.x);
            bone->setY(ovr.y);
            bone->setRotation(ovr.rotation);
            bone->setScaleX(ovr.scale_x);
            bone->setScaleY(ovr.scale_y);
            bone->setShearX(ovr.shear_x);
            bone->setShearY(ovr.shear_y);
        }
    }
}

void SpineViewer::RenderSelectedBoneGizmo() {
    if (m_selected_bone_index < 0) return;

    const auto [bbox_min_x, bbox_min_y, bbox_max_x, bbox_max_y, valid] = GetSelectedBoneGizmo();
    if (!valid) return;

    EnsureGizmoTexture();
    const GLuint gizmo_tex = m_gizmo_texture.Id();
    constexpr std::array<unsigned short, 6> kQuadIndices = {0, 1, 2, 2, 3, 0};

    const float line_w = (std::max)((bbox_max_x - bbox_min_x) * 0.005f, 0.5f);
    float lr = 0.0f, lg = 0.85f, lb = 1.0f, la = 0.8f;
    if (m_use_pma) { lr *= la; lg *= la; lb *= la; }

    auto draw_line = [&](float x1, float y1, float x2, float y2) {
        const float dx = x2 - x1;
        const float dy = y2 - y1;
        const float len = std::hypot(dx, dy);
        if (len < 0.001f) return;
        const float nx = -dy / len * line_w;
        const float ny = dx / len * line_w;
        const std::array<float, 32> v = {
            x1 - nx, y1 - ny, 0, 0, lr, lg, lb, la,
            x1 + nx, y1 + ny, 1, 0, lr, lg, lb, la,
            x2 + nx, y2 + ny, 1, 1, lr, lg, lb, la,
            x2 - nx, y2 - ny, 0, 1, lr, lg, lb, la,
        };
        m_batch_renderer.AddTriangles(gizmo_tex, v, 4, kQuadIndices, spine::BlendMode_Normal);
    };

    const float x0 = bbox_min_x, y0 = bbox_min_y, x1 = bbox_max_x, y1 = bbox_max_y;
    draw_line(x0, y0, x1, y0);
    draw_line(x1, y0, x1, y1);
    draw_line(x1, y1, x0, y1);
    draw_line(x0, y1, x0, y0);

    const float hs = (std::max)((x1 - x0) * 0.025f, 2.0f);
    float hr = 1.0f, hg = 1.0f, hb = 1.0f, ha = 0.9f;
    if (m_use_pma) { hr *= ha; hg *= ha; hb *= ha; }

    auto draw_handle = [&](float cx, float cy) {
        const std::array<float, 32> v = {
            cx - hs, cy - hs, 0, 0, hr, hg, hb, ha,
            cx + hs, cy - hs, 1, 0, hr, hg, hb, ha,
            cx + hs, cy + hs, 1, 1, hr, hg, hb, ha,
            cx - hs, cy + hs, 0, 1, hr, hg, hb, ha,
        };
        m_batch_renderer.AddTriangles(gizmo_tex, v, 4, kQuadIndices, spine::BlendMode_Normal);
    };

    draw_handle(x0, y0);
    draw_handle(x1, y0);
    draw_handle(x0, y1);
    draw_handle(x1, y1);

    const float rot_x = x1 + hs * 4.0f;
    const float rot_y = y1 + hs * 4.0f;
    draw_line(x1, y1, rot_x, rot_y);

    float rr = 0.9f, rg = 0.5f, rb = 1.0f, ra = 0.9f;
    if (m_use_pma) { rr *= ra; rg *= ra; rb *= ra; }
    const float circ_r = hs * 1.5f;
    constexpr int kSegs = 10;
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    constexpr std::array<unsigned short, 3> kTriIndices = {0, 1, 2};

    for (int s = 0; s < kSegs; ++s) {
        const float a1 = (static_cast<float>(s) / kSegs) * kTwoPi;
        const float a2 = (static_cast<float>(s + 1) / kSegs) * kTwoPi;
        const std::array<float, 24> v = {
            rot_x, rot_y, 0.5f, 0.5f, rr, rg, rb, ra,
            rot_x + std::cos(a1) * circ_r, rot_y + std::sin(a1) * circ_r, 0, 0, rr, rg, rb, ra,
            rot_x + std::cos(a2) * circ_r, rot_y + std::sin(a2) * circ_r, 1, 0, rr, rg, rb, ra,
        };
        m_batch_renderer.AddTriangles(gizmo_tex, v, 3, kTriIndices, spine::BlendMode_Normal);
    }

    if (auto& bones = m_skeleton->getBones(); m_selected_bone_index < static_cast<int>(bones.size())) {
        const float bx = bones[m_selected_bone_index]->getWorldX();
        const float by = bones[m_selected_bone_index]->getWorldY();
        const float cs = hs * 2.0f;
        draw_line(bx - cs, by, bx + cs, by);
        draw_line(bx, by - cs, bx, by + cs);
    }
}

void SpineViewer::Render(int viewport_width, int viewport_height) {
    if (!m_skeleton || viewport_width <= 0 || viewport_height <= 0) return;

    EnsureFbo(viewport_width, viewport_height);

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    std::array<GLint, 4> prev_viewport{};
    glGetIntegerv(GL_VIEWPORT, prev_viewport.data());
    GLboolean prev_blend = GL_FALSE;
    glGetBooleanv(GL_BLEND, &prev_blend);
    GLboolean prev_depth = GL_FALSE;
    glGetBooleanv(GL_DEPTH_TEST, &prev_depth);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo.Id());
    glViewport(0, 0, viewport_width, viewport_height);

    glClearColor(m_bg_r, m_bg_g, m_bg_b, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);

    float bx = m_cached_bounds_x;
    float by = m_cached_bounds_y;
    float bw = m_cached_bounds_w;
    float bh = m_cached_bounds_h;
    if (bw <= 0.0f || bh <= 0.0f) {
        bw = 800.0f; bh = 800.0f; bx = -400.0f; by = -400.0f;
    }

    const float pad_x = bw * 0.1f;
    const float pad_y = bh * 0.1f;
    float left = bx - pad_x;
    float right = bx + bw + pad_x;
    float bottom = by - pad_y;
    float top = by + bh + pad_y;

    {
        const float cx = (left + right) * 0.5f - m_pan_x;
        const float cy = (bottom + top) * 0.5f - m_pan_y;
        const float hw = (right - left) / (2.0f * m_zoom);
        const float hh = (top - bottom) / (2.0f * m_zoom);
        left = cx - hw; right = cx + hw;
        bottom = cy - hh; top = cy + hh;
    }

    const float view_aspect = static_cast<float>(viewport_width) / static_cast<float>(viewport_height);
    if (const float bounds_aspect = (right - left) / (top - bottom); bounds_aspect > view_aspect) {
        const float extra = (right - left) / view_aspect - (top - bottom);
        bottom -= extra * 0.5f;
        top += extra * 0.5f;
    } else {
        const float extra = (top - bottom) * view_aspect - (right - left);
        left -= extra * 0.5f;
        right += extra * 0.5f;
    }

    std::array<float, 16> proj{};
    proj[0] = 2.0f / (right - left);
    proj[5] = -2.0f / (top - bottom);
    proj[10] = -1.0f;
    proj[12] = -(right + left) / (right - left);
    proj[13] = (top + bottom) / (top - bottom);
    proj[15] = 1.0f;

    m_batch_renderer.Begin(proj, m_use_pma);

    try {
        auto& draw_order = m_skeleton->getDrawOrder();
        constexpr std::array<unsigned short, 6> kRegionIndices = {0, 1, 2, 2, 3, 0};

        for (size_t i = 0; i < draw_order.size(); ++i) {
            spine::Slot* slot = draw_order[i];
            spine::Attachment* attachment = slot->getAttachment();
            if (!attachment) continue;

            if (!m_hidden_bones.empty()) {
                bool bone_hidden = false;
                for (spine::Bone* b = &slot->getBone(); b != nullptr; b = b->getParent()) {
                    if (m_hidden_bones.contains(b->getData().getName().buffer())) {
                        bone_hidden = true;
                        break;
                    }
                }
                if (bone_hidden) continue;
            }

            const spine::Color skel_col = m_skeleton->getColor();
            const spine::Color slot_col = slot->getColor();
            const float tint_r = skel_col.r * slot_col.r;
            const float tint_g = skel_col.g * slot_col.g;
            const float tint_b = skel_col.b * slot_col.b;
            const float tint_a = skel_col.a * slot_col.a;
            if (tint_a <= 0.0f) continue;

            const spine::BlendMode blend_mode = slot->getData().getBlendMode();

            if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
                auto* region = static_cast<spine::RegionAttachment*>(attachment);
                auto* atlas_region = static_cast<spine::AtlasRegion*>(region->getRendererObject());
                if (!atlas_region || !atlas_region->page || !atlas_region->page->getRendererObject()) continue;

                const auto texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(atlas_region->page->getRendererObject()));
                m_world_vertices.resize(8);
                region->computeWorldVertices(slot->getBone(), m_world_vertices.data(), 0, 2);

                spine::Vector<float>& uvs = region->getUVs();
                const spine::Color att_col = region->getColor();
                const float a = tint_a * att_col.a;
                const float pm = m_use_pma ? a : 1.0f;
                const float r = tint_r * att_col.r * pm;
                const float g = tint_g * att_col.g * pm;
                const float b = tint_b * att_col.b * pm;

                std::array<float, 32> region_verts{};
                for (int j = 0; j < 4; ++j) {
                    region_verts[j * 8 + 0] = m_world_vertices[j * 2 + 0];
                    region_verts[j * 8 + 1] = m_world_vertices[j * 2 + 1];
                    region_verts[j * 8 + 2] = uvs[j * 2 + 0];
                    region_verts[j * 8 + 3] = uvs[j * 2 + 1];
                    region_verts[j * 8 + 4] = r;
                    region_verts[j * 8 + 5] = g;
                    region_verts[j * 8 + 6] = b;
                    region_verts[j * 8 + 7] = a;
                }
                m_batch_renderer.AddTriangles(texture, region_verts, 4, kRegionIndices, blend_mode);
            } else if (attachment->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
                auto* mesh = static_cast<spine::MeshAttachment*>(attachment);
                auto* atlas_region = static_cast<spine::AtlasRegion*>(mesh->getRendererObject());
                if (!atlas_region || !atlas_region->page || !atlas_region->page->getRendererObject()) continue;

                const auto texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(atlas_region->page->getRendererObject()));
                auto& mesh_indices = mesh->getTriangles();
                auto& uvs = mesh->getUVs();
                const size_t world_len = mesh->getWorldVerticesLength();
                const int mesh_vert_count = static_cast<int>(world_len / 2);

                m_world_vertices.resize(world_len);
                mesh->computeWorldVertices(*slot, 0, world_len, m_world_vertices.data(), 0, 2);

                const spine::Color att_col = mesh->getColor();
                const float a = tint_a * att_col.a;
                const float pm = m_use_pma ? a : 1.0f;
                const float r = tint_r * att_col.r * pm;
                const float g = tint_g * att_col.g * pm;
                const float b = tint_b * att_col.b * pm;

                m_mesh_vertices.resize(static_cast<size_t>(mesh_vert_count) * 8);
                for (int j = 0; j < mesh_vert_count; ++j) {
                    const size_t off = static_cast<size_t>(j) * 8;
                    m_mesh_vertices[off + 0] = m_world_vertices[j * 2 + 0];
                    m_mesh_vertices[off + 1] = m_world_vertices[j * 2 + 1];
                    m_mesh_vertices[off + 2] = uvs[j * 2 + 0];
                    m_mesh_vertices[off + 3] = uvs[j * 2 + 1];
                    m_mesh_vertices[off + 4] = r;
                    m_mesh_vertices[off + 5] = g;
                    m_mesh_vertices[off + 6] = b;
                    m_mesh_vertices[off + 7] = a;
                }

                m_batch_renderer.AddTriangles(
                    texture,
                    m_mesh_vertices,
                    mesh_vert_count,
                    std::span<const unsigned short>(mesh_indices.buffer(), mesh_indices.size()),
                    blend_mode);
            } else if (attachment->getRTTI().isExactly(spine::ClippingAttachment::rtti)) {
                auto* clip = static_cast<spine::ClippingAttachment*>(attachment);
                m_clipper.clipStart(*slot, clip);
                continue;
            }

            m_clipper.clipEnd(*slot);
        }
        m_clipper.clipEnd();
    } catch (const std::exception& e) {
        LogError(std::string("SpineViewer::Render exception: ") + e.what());
    } catch (...) {
        LogError("SpineViewer::Render unknown exception");
    }

    RenderSelectedBoneGizmo();
    m_batch_renderer.End();

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (!prev_blend) glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
}

void SpineViewer::EnsureFbo(int width, int height) {
    if (m_fbo && m_fbo_width == width && m_fbo_height == height) return;
    CleanupFbo();

    m_fbo_width = width;
    m_fbo_height = height;

    GLuint fbo_id = 0, tex_id = 0;
    glGenFramebuffers(1, &fbo_id);
    glGenTextures(1, &tex_id);
    m_fbo.Reset(fbo_id);
    m_fbo_texture.Reset(tex_id);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo.Id());
    glBindTexture(GL_TEXTURE_2D, m_fbo_texture.Id());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fbo_texture.Id(), 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SpineViewer::CleanupFbo() {
    m_fbo_texture.Reset();
    m_fbo.Reset();
    m_fbo_width = 0;
    m_fbo_height = 0;
}

std::vector<std::string> SpineViewer::GetAnimationNames() const {
    std::vector<std::string> names;
    if (!m_skeleton_data) return names;
    auto& anims = m_skeleton_data->getAnimations();
    names.reserve(anims.size());
    for (size_t i = 0; i < anims.size(); ++i) {
        names.emplace_back(anims[i]->getName().buffer());
    }
    return names;
}

std::vector<std::string> SpineViewer::GetSkinNames() const {
    std::vector<std::string> names;
    if (!m_skeleton_data) return names;
    auto& skins = m_skeleton_data->getSkins();
    names.reserve(skins.size());
    for (size_t i = 0; i < skins.size(); ++i) {
        names.emplace_back(skins[i]->getName().buffer());
    }
    return names;
}

void SpineViewer::SetAnimation(const std::string& name, bool loop) {
    if (!m_anim_state || !m_skeleton_data) return;
    m_anim_state->setAnimation(0, spine::String(name.c_str()), loop);
    auto& anims = m_skeleton_data->getAnimations();
    for (size_t i = 0; i < anims.size(); ++i) {
        if (std::string_view(anims[i]->getName().buffer()) == name) {
            m_current_anim_index = static_cast<int>(i);
            break;
        }
    }
}

void SpineViewer::NextAnimation() {
    if (!m_anim_state || !m_skeleton_data) return;
    auto& anims = m_skeleton_data->getAnimations();
    if (anims.size() == 0) return;
    m_current_anim_index = (m_current_anim_index + 1) % static_cast<int>(anims.size());
    m_anim_state->setAnimation(0, anims[m_current_anim_index]->getName(), !m_autoplay_next);
}

void SpineViewer::SetSkin(const std::string& name) const {
    if (!m_skeleton) return;
    m_skeleton->setSkin(spine::String(name.c_str()));
    m_skeleton->setSlotsToSetupPose();
}

std::vector<SpineViewer::BoneInfo> SpineViewer::GetBoneList() const {
    std::vector<BoneInfo> list;
    if (!m_skeleton) return list;

    auto& bones = m_skeleton->getBones();
    list.reserve(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        BoneInfo bi;
        spine::BoneData& data = bones[i]->getData();
        bi.name = data.getName().buffer();

        if (data.getParent()) {
            bi.parent_name = data.getParent()->getName().buffer();
            int depth = 0;
            for (spine::BoneData* p = data.getParent(); p != nullptr; p = p->getParent()) {
                ++depth;
            }
            bi.depth = depth;
        }

        bi.setup_x = data.getX();
        bi.setup_y = data.getY();
        bi.setup_rot = data.getRotation();
        bi.setup_sx = data.getScaleX();
        bi.setup_sy = data.getScaleY();
        bi.setup_shx = data.getShearX();
        bi.setup_shy = data.getShearY();

        bi.anim_x = bones[i]->getX();
        bi.anim_y = bones[i]->getY();
        bi.anim_rot = bones[i]->getRotation();
        bi.anim_sx = bones[i]->getScaleX();
        bi.anim_sy = bones[i]->getScaleY();
        bi.anim_shx = bones[i]->getShearX();
        bi.anim_shy = bones[i]->getShearY();

        if (const auto it = m_bone_overrides.find(bi.name); it != m_bone_overrides.end()) {
            bi.has_override = true;
            bi.x = it->second.x;
            bi.y = it->second.y;
            bi.rotation = it->second.rotation;
            bi.scale_x = it->second.scale_x;
            bi.scale_y = it->second.scale_y;
            bi.shear_x = it->second.shear_x;
            bi.shear_y = it->second.shear_y;
        } else {
            bi.x = bi.setup_x;
            bi.y = bi.setup_y;
            bi.rotation = bi.setup_rot;
            bi.scale_x = bi.setup_sx;
            bi.scale_y = bi.setup_sy;
            bi.shear_x = bi.setup_shx;
            bi.shear_y = bi.setup_shy;
        }

        bi.hidden = m_hidden_bones.contains(bi.name);
        list.push_back(std::move(bi));
    }
    return list;
}

void SpineViewer::SetBoneOverride(const std::string& bone_name, const BoneOverride& ovr) {
    m_bone_overrides[bone_name] = ovr;
}

void SpineViewer::ResetBone(const std::string& bone_name) {
    if (!m_skeleton) {
        m_bone_overrides.erase(bone_name);
        return;
    }
    if (spine::Bone* bone = m_skeleton->findBone(spine::String(bone_name.c_str()))) {
        auto& data = bone->getData();
        m_bone_overrides[bone_name] = BoneOverride{
            data.getX(), data.getY(), data.getRotation(),
            data.getScaleX(), data.getScaleY(), data.getShearX(), data.getShearY()
        };
        return;
    }
    m_bone_overrides.erase(bone_name);
}

void SpineViewer::ResetBoneEdits() {
    m_bone_overrides.clear();
    m_hidden_bones.clear();
}

SpineViewer::GizmoState SpineViewer::GetSelectedBoneGizmo() const {
    GizmoState gs;
    if (!m_skeleton || m_selected_bone_index < 0) return gs;

    auto& bones = m_skeleton->getBones();
    if (m_selected_bone_index >= static_cast<int>(bones.size())) return gs;
    spine::Bone* sel_bone = bones[m_selected_bone_index];

    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    bool found = false;

    auto& draw_order = m_skeleton->getDrawOrder();
    for (size_t i = 0; i < draw_order.size(); ++i) {
        spine::Slot* slot = draw_order[i];
        if (&slot->getBone() != sel_bone) continue;
        spine::Attachment* att = slot->getAttachment();
        if (!att) continue;

        int vert_count = 0;
        if (att->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
            auto* reg = static_cast<spine::RegionAttachment*>(att);
            m_world_vertices.resize(8);
            reg->computeWorldVertices(slot->getBone(), m_world_vertices.data(), 0, 2);
            vert_count = 4;
        } else if (att->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
            auto* mesh = static_cast<spine::MeshAttachment*>(att);
            const size_t world_len = mesh->getWorldVerticesLength();
            m_world_vertices.resize(world_len);
            mesh->computeWorldVertices(*slot, 0, world_len, m_world_vertices.data(), 0, 2);
            vert_count = static_cast<int>(world_len / 2);
        }

        for (int v = 0; v < vert_count; ++v) {
            const float vx = m_world_vertices[v * 2];
            const float vy = m_world_vertices[v * 2 + 1];
            min_x = (std::min)(min_x, vx);
            min_y = (std::min)(min_y, vy);
            max_x = (std::max)(max_x, vx);
            max_y = (std::max)(max_y, vy);
            found = true;
        }
    }

    if (!found) {
        const float bx = sel_bone->getWorldX();
        const float by = sel_bone->getWorldY();
        const float sz = m_cached_bounds_w * 0.05f;
        min_x = bx - sz; min_y = by - sz;
        max_x = bx + sz; max_y = by + sz;
    }

    gs.bbox_min_x = min_x; gs.bbox_min_y = min_y;
    gs.bbox_max_x = max_x; gs.bbox_max_y = max_y;
    gs.valid = true;
    return gs;
}

SpineViewer::GizmoHandle SpineViewer::HitTestGizmo(float screen_x, float screen_y, int vp_w, int vp_h) const {
    const GizmoState gs = GetSelectedBoneGizmo();
    if (!gs.valid) return GizmoHandle::None;

    float wx = 0, wy = 0;
    ScreenToWorld(screen_x, screen_y, vp_w, vp_h, wx, wy);

    const float handle_sz = (std::max)((gs.bbox_max_x - gs.bbox_min_x) * 0.08f, 3.0f);
    auto in_handle = [&](float hx, float hy) {
        return std::abs(wx - hx) < handle_sz && std::abs(wy - hy) < handle_sz;
    };

    if (in_handle(gs.bbox_min_x, gs.bbox_max_y)) return GizmoHandle::ScaleTL;
    if (in_handle(gs.bbox_max_x, gs.bbox_max_y)) return GizmoHandle::ScaleTR;
    if (in_handle(gs.bbox_min_x, gs.bbox_min_y)) return GizmoHandle::ScaleBL;
    if (in_handle(gs.bbox_max_x, gs.bbox_min_y)) return GizmoHandle::ScaleBR;

    const float rot_x = gs.bbox_max_x + handle_sz * 3.0f;
    const float rot_y = gs.bbox_max_y + handle_sz * 3.0f;
    if (std::abs(wx - rot_x) < handle_sz * 1.5f && std::abs(wy - rot_y) < handle_sz * 1.5f) {
        return GizmoHandle::Rotate;
    }

    if (wx >= gs.bbox_min_x && wx <= gs.bbox_max_x && wy >= gs.bbox_min_y && wy <= gs.bbox_max_y) {
        return GizmoHandle::Move;
    }
    return GizmoHandle::None;
}

void SpineViewer::ToggleBoneHidden(const std::string& bone_name) {
    if (const auto it = m_hidden_bones.find(bone_name); it != m_hidden_bones.end()) {
        m_hidden_bones.erase(it);
    } else {
        m_hidden_bones.insert(bone_name);
    }
}

bool SpineViewer::IsBoneHidden(const std::string& bone_name) const {
    return m_hidden_bones.contains(bone_name);
}

std::vector<SpineViewer::TextureInfo> SpineViewer::GetTextureList() const {
    std::vector<TextureInfo> list;
    if (!m_atlas) return list;
    auto& pages = m_atlas->getPages();
    list.reserve(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        list.push_back(TextureInfo{
            pages[i]->name.buffer(),
            pages[i]->width,
            pages[i]->height,
            static_cast<GLuint>(reinterpret_cast<uintptr_t>(pages[i]->getRendererObject()))
        });
    }
    return list;
}

bool SpineViewer::SwapTexture(const std::string& page_name, const std::string& png_path) {
    if (!m_atlas) return false;

    SDL_RWops* rw = SDL_RWFromFile(png_path.c_str(), "rb");
    if (!rw) { LogError("SwapTexture: cannot open " + png_path); return false; }

    Gfx::SdlSurfacePtr surface(IMG_Load_RW(rw, 1));
    if (!surface) { LogError("SwapTexture: IMG_Load failed for " + png_path); return false; }

    Gfx::SdlSurfacePtr rgba(SDL_ConvertSurfaceFormat(surface.get(), SDL_PIXELFORMAT_ABGR8888, 0));
    if (!rgba) return false;

    const GLuint new_tex = LoadTextureFromRgba(static_cast<const unsigned char*>(rgba->pixels), rgba->w, rgba->h);
    if (new_tex == 0) return false;

    auto& pages = m_atlas->getPages();
    for (size_t i = 0; i < pages.size(); ++i) {
        if (std::string_view(pages[i]->name.buffer()) == page_name) {
            pages[i]->setRendererObject(reinterpret_cast<void*>(static_cast<uintptr_t>(new_tex)));
            pages[i]->width = rgba->w;
            pages[i]->height = rgba->h;
            m_texture_swaps[page_name] = png_path;
            LogInfo(std::format("SwapTexture: replaced '{}' with {}", page_name, png_path));
            return true;
        }
    }

    LogError("SwapTexture: page '" + page_name + "' not found");
    return false;
}

std::string SpineViewer::GetModifiedSkeletonJson() const {
    if (m_original_json.empty()) return {};
    if (m_bone_overrides.empty()) return m_original_json;

    try {
        auto j = nlohmann::ordered_json::parse(m_original_json);
        if (j.contains("bones") && j["bones"].is_array()) {
            for (auto& bone : j["bones"]) {
                if (!bone.contains("name")) continue;
                const auto name = bone["name"].get<std::string>();
                if (const auto it = m_bone_overrides.find(name); it != m_bone_overrides.end()) {
                    bone["x"] = it->second.x;
                    bone["y"] = it->second.y;
                    bone["rotation"] = it->second.rotation;
                    bone["scaleX"] = it->second.scale_x;
                    bone["scaleY"] = it->second.scale_y;
                    bone["shearX"] = it->second.shear_x;
                    bone["shearY"] = it->second.shear_y;
                }
            }
        }
        return j.dump(2);
    } catch (const std::exception& e) {
        LogError(std::string("GetModifiedSkeletonJson: ") + e.what());
        return m_original_json;
    }
}
