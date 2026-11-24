#include "converters/AssetConverter.hpp"
#include "core/Logger.hpp"
#include "parsers/DbParser.hpp"
#include "parsers/ScspParser.hpp"
#include "parsers/SctParser.hpp"

namespace {

    class SctToPngConverter final : public IAssetConverter {
    public:
        [[nodiscard]] bool Supports(Core::AssetType type, const ExportOptions& options) const noexcept override {
            return type == Core::AssetType::Sct && options.convert_sct_to_png;
        }

        [[nodiscard]] std::string TargetExtension(std::string_view) const override {
            return ".png";
        }

        [[nodiscard]] std::optional<std::vector<uint8_t>> Convert(
            std::span<const uint8_t> raw_data,
            std::string_view filename) const override
        {
            LogInfo(std::string("Converting SCT to PNG: ") + std::string(filename));
            auto png = SctParser::ConvertToPng(raw_data);
            if (png.empty()) return std::nullopt;
            return png;
        }
    };

    class AtlasRewriteConverter final : public IAssetConverter {
    public:
        [[nodiscard]] bool Supports(Core::AssetType type, const ExportOptions& options) const noexcept override {
            return type == Core::AssetType::Atlas && options.convert_sct_to_png;
        }

        [[nodiscard]] std::string TargetExtension(std::string_view original_ext) const override {
            return std::string(original_ext);
        }

        [[nodiscard]] std::optional<std::vector<uint8_t>> Convert(
            std::span<const uint8_t> raw_data,
            std::string_view filename) const override
        {
            LogInfo(std::string("Rewriting atlas texture refs: ") + std::string(filename));
            const std::string rewritten = Core::RewriteAtlasExtensions(
                std::string_view(reinterpret_cast<const char*>(raw_data.data()), raw_data.size()));
            return std::vector<uint8_t>(rewritten.begin(), rewritten.end());
        }
    };

    class DbToJsonConverter final : public IAssetConverter {
    public:
        [[nodiscard]] bool Supports(Core::AssetType type, const ExportOptions& options) const noexcept override {
            return type == Core::AssetType::Database && options.convert_db_to_json;
        }

        [[nodiscard]] std::string TargetExtension(std::string_view) const override {
            return ".json";
        }

        [[nodiscard]] std::optional<std::vector<uint8_t>> Convert(
            std::span<const uint8_t> raw_data,
            std::string_view filename) const override
        {
            LogInfo(std::string("Converting DB to JSON: ") + std::string(filename));
            const std::string json_str = DbParser::ConvertToJson(raw_data);
            if (json_str.empty() || json_str == "{}") return std::nullopt;
            return std::vector<uint8_t>(json_str.begin(), json_str.end());
        }
    };

    class ScspToJsonConverter final : public IAssetConverter {
    public:
        [[nodiscard]] bool Supports(Core::AssetType type, const ExportOptions&) const noexcept override {
            return type == Core::AssetType::Scsp;
        }

        [[nodiscard]] std::string TargetExtension(std::string_view) const override {
            return ".json";
        }

        [[nodiscard]] std::optional<std::vector<uint8_t>> Convert(
            std::span<const uint8_t> raw_data,
            std::string_view filename) const override
        {
            LogInfo(std::string("Converting SCSP to JSON: ") + std::string(filename));
            const std::string json_str = ScspParser::ConvertToJson(raw_data);
            if (json_str.empty() || json_str == "{}") return std::nullopt;
            return std::vector<uint8_t>(json_str.begin(), json_str.end());
        }
    };

} // namespace

AssetConverter::AssetConverter() {
    Register(std::make_unique<SctToPngConverter>());
    Register(std::make_unique<AtlasRewriteConverter>());
    Register(std::make_unique<DbToJsonConverter>());
    Register(std::make_unique<ScspToJsonConverter>());
}

void AssetConverter::Register(std::unique_ptr<IAssetConverter> converter) {
    if (converter) {
        m_converters.push_back(std::move(converter));
    }
}

const IAssetConverter* AssetConverter::FindConverter(Core::AssetType type, const ExportOptions& options) const noexcept {
    for (const auto& converter : m_converters) {
        if (converter->Supports(type, options)) {
            return converter.get();
        }
    }
    return nullptr;
}

std::string AssetConverter::ResolveExtension(std::string_view original_ext, const ExportOptions& options) const {
    const Core::AssetType type = Core::ClassifyExtension(original_ext);
    if (const IAssetConverter* conv = FindConverter(type, options)) {
        return conv->TargetExtension(original_ext);
    }
    return std::string(original_ext);
}

ConvertedAsset AssetConverter::Convert(
    std::span<const uint8_t> raw_data,
    std::string_view filename,
    std::string_view original_ext,
    const ExportOptions& options) const
{
    ConvertedAsset result;
    result.extension = std::string(original_ext);

    const Core::AssetType type = Core::ClassifyExtension(original_ext);
    if (const IAssetConverter* conv = FindConverter(type, options)) {
        try {
            if (auto converted = conv->Convert(raw_data, filename)) {
                result.data = std::move(*converted);
                result.extension = conv->TargetExtension(original_ext);
                result.converted = true;
                return result;
            }
        } catch (const std::exception& e) {
            LogError(std::string("Asset conversion failed for ") + std::string(filename) + ": " + e.what());
        }
    }

    result.data.assign(raw_data.begin(), raw_data.end());
    return result;
}
