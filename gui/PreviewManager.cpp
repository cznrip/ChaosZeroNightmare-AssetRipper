#include "gui/PreviewManager.hpp"
#include "parsers/DbParser.hpp"
#include "parsers/ScspParser.hpp"
#include "parsers/SctParser.hpp"

#include "json.hpp"
#include <SDL_image.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {

    constexpr size_t kMaxPreviewLines = 500;
    constexpr size_t kMaxPlainTextChars = 20000;

    TextPreview MakeTextPreview(std::string full_text, bool is_json, size_t max_chars = 0) {
        TextPreview preview;
        preview.is_json = is_json;
        preview.full_text = std::move(full_text);

        std::string_view source = preview.full_text;
        if (max_chars > 0 && source.size() > max_chars) {
            source = source.substr(0, max_chars);
            preview.truncated = true;
        }

        std::istringstream ss{std::string(source)};
        std::string line;
        preview.preview_lines.reserve(64);
        while (std::getline(ss, line)) {
            if (preview.preview_lines.size() >= kMaxPreviewLines) {
                preview.truncated = true;
                break;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            preview.preview_lines.push_back(std::move(line));
        }
        return preview;
    }

    Gfx::GlTexture UploadRgbaTexture(const uint8_t* pixels, int width, int height) {
        GLuint id = 0;
        glGenTextures(1, &id);
        Gfx::GlTexture tex(id);
        glBindTexture(GL_TEXTURE_2D, tex.Id());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        return tex;
    }

    class DatabasePreviewHandler final : public IAssetPreviewHandler {
    public:
        [[nodiscard]] bool CanHandle(Core::AssetType type) const noexcept override {
            return type == Core::AssetType::Database;
        }

        [[nodiscard]] PreviewData Load(IArchive& archive, const Core::FileNode& node) const override {
            const auto file_data = archive.GetFileData(node);
            if (file_data.empty()) return ErrorPreview{"Failed to read DB file"};

            const nlohmann::ordered_json parsed = DbParser::ParseToJson(file_data);
            if (!parsed.is_array() || parsed.empty() || !parsed[0].is_object()) {
                return MakeTextPreview(parsed.dump(2), true);
            }

            DatabasePreview db;
            db.filename = node.name;
            for (const auto& [key, _] : parsed[0].items()) {
                db.column_names.push_back(key);
            }

            db.rows.reserve(parsed.size());
            std::vector<size_t> max_lens(db.column_names.size(), 0);
            for (size_t j = 0; j < db.column_names.size(); ++j) {
                max_lens[j] = db.column_names[j].size();
            }

            for (const auto& row : parsed) {
                if (!row.is_object()) continue;
                std::vector<std::string> row_data;
                row_data.reserve(db.column_names.size());
                for (size_t j = 0; j < db.column_names.size(); ++j) {
                    const auto& col = db.column_names[j];
                    std::string cell;
                    if (row.contains(col)) {
                        const auto& val = row[col];
                        if (val.is_string()) cell = val.get<std::string>();
                        else if (val.is_boolean()) cell = val.get<bool>() ? "true" : "false";
                        else if (val.is_null()) cell = "NULL";
                        else cell = val.dump();
                    }
                    max_lens[j] = (std::max)(max_lens[j], cell.size());
                    row_data.push_back(std::move(cell));
                }
                db.rows.push_back(std::move(row_data));
            }

            // Precompute column widths once at load time (eliminates O(rows * cols) scan every frame).
            db.column_widths.resize(db.column_names.size());
            for (size_t j = 0; j < db.column_names.size(); ++j) {
                db.column_widths[j] = std::clamp(7.5f * static_cast<float>(max_lens[j]), 120.0f, 400.0f);
            }
            return db;
        }
    };

    class ScspPreviewHandler final : public IAssetPreviewHandler {
    public:
        [[nodiscard]] bool CanHandle(Core::AssetType type) const noexcept override {
            return type == Core::AssetType::Scsp;
        }

        [[nodiscard]] PreviewData Load(IArchive& archive, const Core::FileNode& node) const override {
            const auto file_data = archive.GetFileData(node);
            if (file_data.empty()) return ErrorPreview{"Failed to read SCSP file"};

            const nlohmann::ordered_json parsed = ScspParser::ParseToJson(file_data);
            if (parsed.empty()) return ErrorPreview{"Failed to parse SCSP file"};
            return MakeTextPreview(parsed.dump(2), true);
        }
    };

    class JsonPreviewHandler final : public IAssetPreviewHandler {
    public:
        [[nodiscard]] bool CanHandle(Core::AssetType type) const noexcept override {
            return type == Core::AssetType::Json;
        }

        [[nodiscard]] PreviewData Load(IArchive& archive, const Core::FileNode& node) const override {
            const auto file_data = archive.GetFileData(node);
            if (file_data.empty()) return ErrorPreview{"Failed to read JSON file"};

            std::string raw(file_data.begin(), file_data.end());
            try {
                const auto parsed = nlohmann::json::parse(raw);
                return MakeTextPreview(parsed.dump(2), true);
            } catch (...) {
                return MakeTextPreview(std::move(raw), true);
            }
        }
    };

    class TextPreviewHandler final : public IAssetPreviewHandler {
    public:
        [[nodiscard]] bool CanHandle(Core::AssetType type) const noexcept override {
            return type == Core::AssetType::Text || type == Core::AssetType::Atlas;
        }

        [[nodiscard]] PreviewData Load(IArchive& archive, const Core::FileNode& node) const override {
            const auto file_data = archive.GetFileData(node);
            if (file_data.empty()) return ErrorPreview{"Failed to read file"};
            return MakeTextPreview(std::string(file_data.begin(), file_data.end()), false, kMaxPlainTextChars);
        }
    };

    class AnimatedWebPPreviewHandler final : public IAssetPreviewHandler {
    public:
        [[nodiscard]] bool CanHandle(Core::AssetType type) const noexcept override {
            return type == Core::AssetType::AnimatedWebP;
        }

        [[nodiscard]] PreviewData Load(IArchive&, const Core::FileNode&) const override {
            return ErrorPreview{"Animated WebP preview not supported. Use 'Extract' to save the file."};
        }
    };

    class ImagePreviewHandler final : public IAssetPreviewHandler {
    public:
        [[nodiscard]] bool CanHandle(Core::AssetType type) const noexcept override {
            return type == Core::AssetType::Sct || type == Core::AssetType::Image;
        }

        [[nodiscard]] PreviewData Load(IArchive& archive, const Core::FileNode& node) const override {
            const auto* info = std::get_if<Core::FileInfo>(&node.data);
            if (!info) return ErrorPreview{"Not a file"};

            const auto file_data = archive.GetFileData(node);
            if (file_data.empty()) return ErrorPreview{"Failed to read file data"};

            // Fast path for SCT/SCT2: upload decoded RGBA directly to OpenGL without PNG roundtrip!
            if (Core::IsSctExtension(info->format)) {
                const auto [rgba, width, height] = SctParser::ConvertToRgba(file_data);
                if (rgba.empty() || width <= 0 || height <= 0) {
                    return ErrorPreview{"Failed to decode SCT/SCT2 image"};
                }
                return ImagePreview{UploadRgbaTexture(rgba.data(), width, height), width, height};
            }

            SDL_RWops* rw = SDL_RWFromConstMem(file_data.data(), static_cast<int>(file_data.size()));
            if (!rw) return ErrorPreview{"Failed to create memory stream"};

            Gfx::SdlSurfacePtr surface(IMG_Load_RW(rw, 1));
            if (!surface) return ErrorPreview{std::string("Failed to decode image: ") + IMG_GetError()};

            Gfx::SdlSurfacePtr rgba(SDL_ConvertSurfaceFormat(surface.get(), SDL_PIXELFORMAT_ABGR8888, 0));
            if (!rgba) return ErrorPreview{"Failed to convert image format"};

            return ImagePreview{
                UploadRgbaTexture(static_cast<const uint8_t*>(rgba->pixels), rgba->w, rgba->h),
                rgba->w,
                rgba->h
            };
        }
    };

} // namespace

PreviewManager::PreviewManager() {
    Register(std::make_unique<DatabasePreviewHandler>());
    Register(std::make_unique<ScspPreviewHandler>());
    Register(std::make_unique<JsonPreviewHandler>());
    Register(std::make_unique<TextPreviewHandler>());
    Register(std::make_unique<AnimatedWebPPreviewHandler>());
    Register(std::make_unique<ImagePreviewHandler>());
}

void PreviewManager::Register(std::unique_ptr<IAssetPreviewHandler> handler) {
    if (handler) m_handlers.push_back(std::move(handler));
}

void PreviewManager::Clear() noexcept {
    m_data = std::monostate{};
    m_current_node = nullptr;
}

void PreviewManager::SetError(std::string message) {
    m_data = ErrorPreview{std::move(message)};
    m_current_node = nullptr;
}

void PreviewManager::Load(IArchive& archive, const Core::FileNode& node) {
    Clear();
    m_current_node = &node;

    const auto* info = std::get_if<Core::FileInfo>(&node.data);
    if (!info) {
        m_data = ErrorPreview{"Not a file"};
        return;
    }

    const Core::AssetType type = Core::ClassifyExtension(info->format);
    try {
        for (const auto& handler : m_handlers) {
            if (handler->CanHandle(type)) {
                m_data = handler->Load(archive, node);
                return;
            }
        }
        m_data = ErrorPreview{"Preview not available for " + info->format + " files"};
    } catch (const std::exception& e) {
        m_data = ErrorPreview{std::string("Error: ") + e.what()};
    } catch (...) {
        m_data = ErrorPreview{"Unknown error occurred"};
    }
}

Gfx::SdlSurfacePtr PreviewManager::LoadSurface(
    IArchive& archive,
    const Core::FileNode& node,
    std::string& out_error)
{
    const auto* info = std::get_if<Core::FileInfo>(&node.data);
    if (!info) {
        out_error = "Not a file";
        return nullptr;
    }

    const auto file_data = archive.GetFileData(node);
    if (file_data.empty()) {
        out_error = "Failed to read file data";
        return nullptr;
    }

    if (Core::IsSctExtension(info->format)) {
        try {
            const auto [rgba, width, height] = SctParser::ConvertToRgba(file_data);
            if (rgba.empty() || width <= 0 || height <= 0) {
                out_error = "Failed to decode SCT/SCT2 file";
                return nullptr;
            }
            Gfx::SdlSurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
                0, width, height, 32, SDL_PIXELFORMAT_ABGR8888));
            if (!surface) {
                out_error = std::string("Failed to allocate SDL surface: ") + SDL_GetError();
                return nullptr;
            }
            const size_t row_bytes = static_cast<size_t>(width) * 4;
            for (int y = 0; y < height; ++y) {
                std::copy_n(
                    rgba.data() + static_cast<size_t>(y) * row_bytes,
                    row_bytes,
                    static_cast<uint8_t*>(surface->pixels) + static_cast<size_t>(y) * surface->pitch);
            }
            return surface;
        } catch (const std::exception& e) {
            out_error = std::string("SCT parsing error: ") + e.what();
            return nullptr;
        }
    }

    SDL_RWops* rw = SDL_RWFromConstMem(file_data.data(), static_cast<int>(file_data.size()));
    if (!rw) {
        out_error = "Failed to create memory stream";
        return nullptr;
    }

    Gfx::SdlSurfacePtr surface(IMG_Load_RW(rw, 1));
    if (!surface) {
        out_error = std::string("Failed to decode image: ") + IMG_GetError();
        return nullptr;
    }
    return surface;
}
