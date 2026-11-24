#include "parsers/SctParser.hpp"
#include "core/BinaryReader.hpp"
#include "core/Compression.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define ETCDEC_IMPLEMENTATION
#include "etcdec.h"
#define ASTCENC_API
#include <astcenc.h>

namespace {

    constexpr int32_t kSct2Signature = 844383059;
    constexpr uint16_t kSctSignatureWord = 17235;
    constexpr uint8_t kSctSignatureByte = 84;
    constexpr int kMaxTextureDim = 16384;

    enum class ContainerFormat {
        Unknown,
        Sct,
        Sct2
    };

    enum class TextureFormat {
        Unknown,
        L8,
        Rgb565Le,
        Rgb,
        Etc2Rgba8,
        Astc4x4,
        Astc6x6,
        Astc8x8,
        RgbaRaw
    };

    struct SctHeader {
        int pixel_format = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t texture_width = 0;
        uint16_t texture_height = 0;
        size_t data_offset = 0;
        bool compressed = false;
        uint8_t flags = 0;
        bool has_alpha = false;
        bool raw_data = false;
    };

    /**
     * RAII wrapper for astcenc_context* cached thread-locally per block dimension
     * so bulk texture extraction avoids allocating/destroying a decoder context per image.
     */
    class AstcContext {
    public:
        AstcContext() = default;
        explicit AstcContext(astcenc_context* ctx) noexcept : m_ctx(ctx) {}
        ~AstcContext() { Reset(); }

        AstcContext(const AstcContext&) = delete;
        AstcContext& operator=(const AstcContext&) = delete;

        AstcContext(AstcContext&& other) noexcept
            : m_ctx(std::exchange(other.m_ctx, nullptr)) {}

        AstcContext& operator=(AstcContext&& other) noexcept {
            if (this != &other) {
                Reset();
                m_ctx = std::exchange(other.m_ctx, nullptr);
            }
            return *this;
        }

        void Reset() noexcept {
            if (m_ctx) {
                astcenc_context_free(m_ctx);
                m_ctx = nullptr;
            }
        }

        [[nodiscard]] astcenc_context* Get() const noexcept { return m_ctx; }

    private:
        astcenc_context* m_ctx = nullptr;
    };

    astcenc_context* GetCachedAstcContext(int block_w, int block_h) {
        thread_local std::map<std::pair<int, int>, AstcContext> cache;
        const auto key = std::make_pair(block_w, block_h);

        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second.Get();
        }

        astcenc_config config{};
        if (astcenc_config_init(
                ASTCENC_PRF_LDR,
                static_cast<unsigned int>(block_w),
                static_cast<unsigned int>(block_h),
                1,
                ASTCENC_PRE_FASTEST,
                0,
                &config) != ASTCENC_SUCCESS)
        {
            LogError("SctParser: astcenc_config_init failed");
            return nullptr;
        }

        astcenc_context* raw_ctx = nullptr;
        if (astcenc_context_alloc(&config, 1, &raw_ctx) != ASTCENC_SUCCESS || !raw_ctx) {
            LogError("SctParser: astcenc_context_alloc failed");
            return nullptr;
        }

        auto [it, _] = cache.emplace(key, AstcContext(raw_ctx));
        return it->second.Get();
    }

    ContainerFormat DetectContainerFormat(std::span<const uint8_t> data) noexcept {
        Core::BinaryReader reader(data);
        if (const auto sig = reader.PeekAt<int32_t>(0); sig && *sig == kSct2Signature) {
            return ContainerFormat::Sct2;
        }
        if (data.size() >= 3) {
            if (const auto word = reader.PeekAt<uint16_t>(0); word && *word == kSctSignatureWord && data[2] == kSctSignatureByte) {
                return ContainerFormat::Sct;
            }
        }
        return ContainerFormat::Unknown;
    }

    SctHeader ParseSctHeader(std::span<const uint8_t> data) {
        if (data.size() < 9) {
            throw std::runtime_error("File too small to contain a valid SCT header");
        }
        Core::BinaryReader reader(data);
        SctHeader header{};
        reader.Skip(4);
        header.pixel_format = reader.Read<uint8_t>();
        header.width = reader.Read<uint16_t>();
        header.height = reader.Read<uint16_t>();
        header.texture_width = header.width;
        header.texture_height = header.height;
        header.data_offset = 9;
        header.compressed = true;
        return header;
    }

    SctHeader ParseSct2Header(std::span<const uint8_t> data) {
        if (data.size() < 34) {
            throw std::runtime_error("File too small to contain a valid SCT2 header");
        }
        Core::BinaryReader reader(data);
        SctHeader header{};
        reader.Skip(12); // signature(4) + total_size(4) + unk(4)
        const int32_t offset = reader.Read<int32_t>();
        if (offset < 0 || static_cast<size_t>(offset) > data.size()) {
            throw std::runtime_error("Invalid SCT2 data offset");
        }
        header.data_offset = static_cast<size_t>(offset);
        reader.Skip(4);
        header.pixel_format = reader.Read<int32_t>();
        header.width = reader.Read<uint16_t>();
        header.height = reader.Read<uint16_t>();
        header.texture_width = reader.Read<uint16_t>();
        header.texture_height = reader.Read<uint16_t>();
        header.flags = reader.Read<uint8_t>();

        header.has_alpha = (header.flags & 0x01u) != 0;
        header.raw_data = (header.flags & 0x10u) != 0;
        header.compressed = (header.flags & 0x80u) != 0;
        return header;
    }

    TextureFormat ClassifyTextureFormat(int format_code) noexcept {
        switch (format_code) {
            case 4:   return TextureFormat::Rgb565Le;
            case 6:   return TextureFormat::Rgb;
            case 16:  return TextureFormat::Rgb565Le;
            case 19:  return TextureFormat::Etc2Rgba8;
            case 40:  return TextureFormat::Astc4x4;
            case 44:  return TextureFormat::Astc6x6;
            case 47:  return TextureFormat::Astc8x8;
            case 102: return TextureFormat::L8;
            default:
                if (format_code >= 17 && format_code <= 26) {
                    return TextureFormat::RgbaRaw;
                }
                return TextureFormat::Unknown;
        }
    }

    /**
     * Attempts heuristic LZ4 decompression once and returns the decompressed payload
     * directly if decompression makes sense, avoiding double decompression.
     */
    std::optional<std::vector<uint8_t>> TryHeuristicLz4Decompress(
        std::span<const uint8_t> image_data,
        int width,
        int height,
        int pixel_format)
    {
        if (image_data.size() < 8) return std::nullopt;

        const double expected_size = (pixel_format == 40)
            ? static_cast<double>(((width + 3) / 4) * ((height + 3) / 4) * 16)
            : static_cast<double>(width * height * 2);

        if (expected_size <= 0.0) return std::nullopt;

        const double size_ratio = static_cast<double>(image_data.size()) / expected_size;
        if (size_ratio >= 0.95) return std::nullopt;

        auto decompressed = Core::TryDecompressLz4WithHeader(image_data, false);
        if (!decompressed || decompressed->empty()) return std::nullopt;

        const double decomp_ratio = static_cast<double>(decompressed->size()) / expected_size;
        if (decomp_ratio > size_ratio) {
            return decompressed;
        }
        return std::nullopt;
    }

    std::vector<uint8_t> DecodeRgb565LeToRgba(std::span<const uint8_t> data) {
        const size_t pixel_count = data.size() / 2;
        std::vector<uint8_t> rgba(pixel_count * 4);
        Core::BinaryReader reader(data);
        for (size_t i = 0; i < pixel_count; ++i) {
            const uint16_t pixel = *reader.TryRead<uint16_t>();
            rgba[i * 4 + 0] = static_cast<uint8_t>(((pixel >> 11) & 0x1Fu) << 3);
            rgba[i * 4 + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3Fu) << 2);
            rgba[i * 4 + 2] = static_cast<uint8_t>((pixel & 0x1Fu) << 3);
            rgba[i * 4 + 3] = 255;
        }
        return rgba;
    }

    std::vector<uint8_t> DecodeL8ToRgba(std::span<const uint8_t> data) {
        std::vector<uint8_t> rgba(data.size() * 4);
        for (size_t i = 0; i < data.size(); ++i) {
            const uint8_t gray = data[i];
            rgba[i * 4 + 0] = gray;
            rgba[i * 4 + 1] = gray;
            rgba[i * 4 + 2] = gray;
            rgba[i * 4 + 3] = 255;
        }
        return rgba;
    }

    std::vector<uint8_t> DecodeAstc(
        std::span<const uint8_t> compressed_data,
        int width,
        int height,
        int block_w,
        int block_h)
    {
        if (width <= 0 || height <= 0 || width > kMaxTextureDim || height > kMaxTextureDim) {
            LogError("ASTC decode: invalid dimensions");
            return {};
        }

        const int blocks_x = (width + block_w - 1) / block_w;
        const int blocks_y = (height + block_h - 1) / block_h;
        const size_t expected_size = static_cast<size_t>(blocks_x) * static_cast<size_t>(blocks_y) * 16;
        if (compressed_data.size() < expected_size) {
            LogError(std::format("ASTC data too small: expected {}, got {}", expected_size, compressed_data.size()));
            return {};
        }

        astcenc_context* ctx = GetCachedAstcContext(block_w, block_h);
        if (!ctx) {
            return std::vector<uint8_t>(static_cast<size_t>(width) * height * 4, 128);
        }

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        void* data_ptr = rgba.data();

        astcenc_image image{};
        image.dim_x = static_cast<unsigned int>(width);
        image.dim_y = static_cast<unsigned int>(height);
        image.dim_z = 1;
        image.data_type = ASTCENC_TYPE_U8;
        image.data = &data_ptr;

        // Decode directly into RGBA order (avoids redundant BGRA->RGBA swap pass).
        constexpr astcenc_swizzle kRgbaSwizzle = {
            ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A
        };

        const astcenc_error status = astcenc_decompress_image(
            ctx, compressed_data.data(), compressed_data.size(), &image, &kRgbaSwizzle, 0);

        if (status != ASTCENC_SUCCESS) {
            LogError(std::format("astcenc_decompress_image failed ({})", static_cast<int>(status)));
            std::ranges::fill(rgba, uint8_t{128});
        }

        return rgba;
    }

    std::vector<uint8_t> DecodeEtc2Rgba8(std::span<const uint8_t> compressed_data, int width, int height) {
        constexpr int kBlockW = 4;
        constexpr int kBlockH = 4;
        constexpr int kBlockBytes = 16;

        const int num_blocks_x = (width + kBlockW - 1) / kBlockW;
        const int num_blocks_y = (height + kBlockH - 1) / kBlockH;
        const size_t expected_size = static_cast<size_t>(num_blocks_x) * num_blocks_y * kBlockBytes;

        if (compressed_data.size() < expected_size) {
            return std::vector<uint8_t>(static_cast<size_t>(width) * height * 4, 128);
        }

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        std::array<uint8_t, kBlockW * kBlockH * 4> block_pixels{};
        const uint8_t* src_ptr = compressed_data.data();

        for (int by = 0; by < num_blocks_y; ++by) {
            for (int bx = 0; bx < num_blocks_x; ++bx) {
                etcdec_eac_rgba(src_ptr, block_pixels.data(), kBlockBytes);
                src_ptr += kBlockBytes;

                for (int y = 0; y < kBlockH; ++y) {
                    const int final_y = by * kBlockH + y;
                    if (final_y >= height) continue;
                    for (int x = 0; x < kBlockW; ++x) {
                        const int final_x = bx * kBlockW + x;
                        if (final_x >= width) continue;
                        const size_t block_idx = static_cast<size_t>((y * kBlockW + x) * 4);
                        const size_t final_idx = static_cast<size_t>((final_y * width + final_x) * 4);
                        std::copy_n(block_pixels.data() + block_idx, 4, rgba.data() + final_idx);
                    }
                }
            }
        }
        return rgba;
    }

} // namespace

SctParser::RgbaImage SctParser::ConvertToRgba(std::span<const uint8_t> data) {
    try {
        const ContainerFormat container = DetectContainerFormat(data);
        if (container == ContainerFormat::Unknown) return {};

        SctHeader header{};
        std::vector<uint8_t> decompressed_storage;
        std::span<const uint8_t> payload;

        if (container == ContainerFormat::Sct2) {
            header = ParseSct2Header(data);
            payload = data.subspan(header.data_offset);

            if (header.raw_data || header.has_alpha) {
                if (auto decomp = TryHeuristicLz4Decompress(payload, header.width, header.height, header.pixel_format)) {
                    decompressed_storage = std::move(*decomp);
                    payload = decompressed_storage;
                }
            } else if (header.pixel_format == 40 || header.compressed) {
                decompressed_storage = Core::DecompressLz4WithHeader(payload, false);
                payload = decompressed_storage;
            }
        } else {
            header = ParseSctHeader(data);
            payload = data.subspan(header.data_offset);
            decompressed_storage = Core::DecompressLz4WithHeader(payload, false);
            payload = decompressed_storage;
        }

        const int width = header.width;
        const int height = header.height;
        if (width <= 0 || height <= 0 || width > kMaxTextureDim || height > kMaxTextureDim) {
            LogError("SctParser::ConvertToRgba: invalid dimensions");
            return {};
        }

        std::vector<uint8_t> rgba;
        switch (ClassifyTextureFormat(header.pixel_format)) {
            case TextureFormat::L8:
                rgba = DecodeL8ToRgba(payload);
                break;
            case TextureFormat::Rgb565Le:
                rgba = DecodeRgb565LeToRgba(payload);
                break;
            case TextureFormat::Etc2Rgba8:
                rgba = DecodeEtc2Rgba8(payload, width, height);
                break;
            case TextureFormat::Astc4x4:
                rgba = DecodeAstc(payload, width, height, 4, 4);
                break;
            case TextureFormat::Astc6x6:
                rgba = DecodeAstc(payload, width, height, 6, 6);
                break;
            case TextureFormat::Astc8x8:
                rgba = DecodeAstc(payload, width, height, 8, 8);
                break;
            default:
                rgba.assign(payload.begin(), payload.end());
                break;
        }

        if (rgba.size() < static_cast<size_t>(width) * height * 4) {
            LogError("SctParser::ConvertToRgba: RGBA buffer invalid size");
            return {};
        }

        return RgbaImage{std::move(rgba), width, height};
    } catch (const std::exception& e) {
        LogError(std::string("SctParser::ConvertToRgba exception: ") + e.what());
        return {};
    }
}

std::vector<uint8_t> SctParser::ConvertToPng(std::span<const uint8_t> data) {
    auto [image, width, height] = ConvertToRgba(data);
    if (image.empty()) return {};

    std::vector<uint8_t> png_data;
    auto write_func = [](void* context, void* buf, const int size) {
        auto* vec = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<const uint8_t*>(buf);
        vec->insert(vec->end(), bytes, bytes + size);
    };

    if (stbi_write_png_to_func(write_func, &png_data, width, height, 4, image.data(), width * 4) == 0) {
        LogError("SctParser::ConvertToPng: PNG encoding failed");
        return {};
    }
    return png_data;
}
