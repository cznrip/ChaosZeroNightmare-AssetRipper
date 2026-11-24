#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "core/BinaryReader.hpp"

namespace Core {

    /**
     * Decompresses a raw LZ4 block into a buffer of up to @p expected_size bytes.
     * Shared by both SctParser and ScspParser.
     */
    inline std::vector<uint8_t> DecompressLz4Block(std::span<const uint8_t> src, size_t expected_size) {
        std::vector<uint8_t> dst;
        dst.reserve(expected_size);

        size_t src_pos = 0;
        while (src_pos < src.size() && dst.size() < expected_size) {
            const uint8_t token = src[src_pos++];

            size_t literal_length = (token >> 4) & 0x0Fu;
            if (literal_length == 15) {
                while (src_pos < src.size()) {
                    const uint8_t extra = src[src_pos++];
                    literal_length += extra;
                    if (extra != 255) break;
                }
            }

            if (literal_length > 0) {
                const size_t avail_src = src.size() - src_pos;
                const size_t avail_dst = expected_size - dst.size();
                const size_t copy_len = (std::min)({literal_length, avail_src, avail_dst});
                dst.insert(dst.end(), src.data() + src_pos, src.data() + src_pos + copy_len);
                src_pos += copy_len;
                if (copy_len < literal_length) break;
            }

            if (src_pos >= src.size() || dst.size() >= expected_size) break;
            if (src_pos + 2 > src.size()) break;

            const uint16_t offset = static_cast<uint16_t>(src[src_pos]) |
                                    static_cast<uint16_t>(static_cast<uint16_t>(src[src_pos + 1]) << 8);
            src_pos += 2;
            if (offset == 0 || offset > dst.size()) break;

            size_t match_length = (token & 0x0Fu);
            if (match_length == 15) {
                while (src_pos < src.size()) {
                    const uint8_t extra = src[src_pos++];
                    match_length += extra;
                    if (extra != 255) break;
                }
            }
            match_length += 4;

            const size_t match_start = dst.size() - offset;
            const size_t copy_end = (std::min)(match_length, expected_size - dst.size());
            for (size_t i = 0; i < copy_end; ++i) {
                dst.push_back(dst[match_start + i]);
            }
        }

        if (dst.size() > expected_size) {
            dst.resize(expected_size);
        }
        return dst;
    }

    /**
     * Decompresses an LZ4 stream prefixed with [int32_t decompressed_size, int32_t compressed_size].
     * When @p strict_compressed_size is true, bounds-checks the compressed payload length against input size.
     */
    inline std::optional<std::vector<uint8_t>> TryDecompressLz4WithHeader(
        std::span<const uint8_t> data,
        bool strict_compressed_size = false) noexcept
    {
        if (data.size() < 8) return std::nullopt;

        BinaryReader reader(data);
        const auto dec_len = reader.TryRead<int32_t>();
        const auto comp_len = reader.TryRead<int32_t>();
        if (!dec_len || !comp_len || *dec_len <= 0 || *comp_len < 0) {
            return std::nullopt;
        }

        // Guard against corrupt headers claiming absurd sizes (e.g. > 512 MB).
        constexpr int32_t kMaxDecompressedSize = 512 * 1024 * 1024;
        if (*dec_len > kMaxDecompressedSize) {
            return std::nullopt;
        }

        std::span<const uint8_t> payload = data.subspan(8);
        if (strict_compressed_size) {
            if (static_cast<size_t>(*comp_len) > payload.size()) {
                return std::nullopt;
            }
            payload = payload.subspan(0, static_cast<size_t>(*comp_len));
        }

        try {
            return DecompressLz4Block(payload, static_cast<size_t>(*dec_len));
        } catch (...) {
            return std::nullopt;
        }
    }

    inline std::vector<uint8_t> DecompressLz4WithHeader(
        std::span<const uint8_t> data,
        bool strict_compressed_size = false)
    {
        auto result = TryDecompressLz4WithHeader(data, strict_compressed_size);
        if (!result) {
            throw std::runtime_error("Invalid or corrupt LZ4 compressed block");
        }
        return std::move(*result);
    }

    /**
     * Normalizes legacy Zstandard frame magic bytes (0x25..0x27) to standard 0x28.
     */
    inline bool NormalizeLegacyZstdFrame(std::span<uint8_t> buffer) noexcept {
        constexpr uint8_t kZstdMagic = 0x28;
        constexpr uint8_t kLegacyFirst = 0x25;
        constexpr uint8_t kLegacyLast = 0x27;

        if (buffer.size() < 4 || buffer[1] != 0xB5 || buffer[2] != 0x2F || buffer[3] != 0xFD) {
            return false;
        }
        if (buffer[0] < kLegacyFirst || buffer[0] > kLegacyLast) {
            return false;
        }
        buffer[0] = kZstdMagic;
        return true;
    }

} // namespace Core
