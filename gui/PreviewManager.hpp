#pragma once

#include "archive/IArchive.hpp"
#include "core/AssetFormat.hpp"
#include "core/Core.hpp"
#include "gui/GlResources.hpp"

#include <array>
#include <memory>
#include <string>
#include <variant>
#include <vector>

struct ImagePreview {
    Gfx::GlTexture texture;
    int width = 0;
    int height = 0;
};

struct DatabasePreview {
    std::string filename;
    std::vector<std::string> column_names;
    std::vector<std::vector<std::string>> rows;
    std::vector<float> column_widths; // precomputed once on load
    std::array<char, 128> search_buffer{};
};

struct TextPreview {
    std::string full_text;
    std::vector<std::string> preview_lines; // pre-split once on load (up to 500 lines)
    bool is_json = false;
    bool truncated = false;
};

struct ErrorPreview {
    std::string message;
};

using PreviewData = std::variant<
    std::monostate,
    ImagePreview,
    DatabasePreview,
    TextPreview,
    ErrorPreview>;

/**
 * Strategy interface for loading an asset into a typed PreviewData payload.
 */
class IAssetPreviewHandler {
public:
    virtual ~IAssetPreviewHandler() = default;

    [[nodiscard]] virtual bool CanHandle(Core::AssetType type) const noexcept = 0;
    [[nodiscard]] virtual PreviewData Load(IArchive& archive, const Core::FileNode& node) const = 0;
};

/**
 * Manages registered preview strategies and owns the currently loaded preview state.
 */
class PreviewManager {
public:
    PreviewManager();

    void Register(std::unique_ptr<IAssetPreviewHandler> handler);
    void Load(IArchive& archive, const Core::FileNode& node);
    void SetError(std::string message);
    void Clear() noexcept;

    [[nodiscard]] bool HasContent() const noexcept {
        return !std::holds_alternative<std::monostate>(m_data);
    }

    [[nodiscard]] const PreviewData& Data() const noexcept { return m_data; }
    [[nodiscard]] PreviewData& MutableData() noexcept { return m_data; }
    [[nodiscard]] const Core::FileNode* CurrentNode() const noexcept { return m_current_node; }

    [[nodiscard]] static Gfx::SdlSurfacePtr LoadSurface(
        IArchive& archive,
        const Core::FileNode& node,
        std::string& out_error);

private:
    std::vector<std::unique_ptr<IAssetPreviewHandler>> m_handlers;
    PreviewData m_data;
    const Core::FileNode* m_current_node = nullptr;
};
