#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Core {

    inline constexpr uint32_t kDataPackInitial = 0x24D1C;
    inline constexpr uint32_t kDataPackMult = 0x41C64E6D;
    inline constexpr size_t kDataPackKeySize = 0x81;

    consteval std::array<uint8_t, kDataPackKeySize> MakeDataPackXorKey() noexcept {
        std::array<uint8_t, kDataPackKeySize> key{};
        uint32_t current = kDataPackInitial;
        for (size_t i = 0; i < kDataPackKeySize; ++i) {
            current = (current * kDataPackMult) & 0x7FFFFFFFu;
            key[i] = static_cast<uint8_t>((current >> 16) & 0xFFu);
        }
        return key;
    }

    inline constexpr auto kDataPackXorKey = MakeDataPackXorKey();

    inline void XorDataPackBuffer(std::span<uint8_t> buffer, uint64_t file_offset) noexcept {
        for (size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] ^= kDataPackXorKey[(file_offset + i) % kDataPackKeySize];
        }
    }

    namespace Detail {
        consteval uint8_t HexNibble(char c) noexcept {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            return 0;
        }

        consteval std::array<uint8_t, 256> ParseHexKey256(std::string_view hex) noexcept {
            std::array<uint8_t, 256> key{};
            for (size_t i = 0; i < 256; ++i) {
                key[i] = static_cast<uint8_t>((HexNibble(hex[i * 2]) << 4) | HexNibble(hex[i * 2 + 1]));
            }
            return key;
        }

        inline constexpr std::string_view kDbKeyHex =
            "91AE4ED4644F585162EC1BD5EF24ADDBAF838242AEF51E97804B134FFD8CE5BB"
            "4F6E3E6451147CDF56C318E5E964C999C0D95CC860822E6B418BE465D79A036D"
            "BF67AB3DA72AB1023A4561F444E5CE858D23EA10FEB4899151AD7E43FF3E2419"
            "A97B4DD3AF4EF5C829E5AF4ACE9436F6B6B6382E9DFD26642099011A4899089C"
            "9D4B9F80BBB00A4CC73255CE1F78646E91C9C12313F5D840DC51457010D37D19"
            "615BB69888B42B19E749F993C00337E9332F89B320C173A5653848788798A771"
            "739E72DBC84C7946597149BDDAE4E3BD1A17856C85A555CFA24F6352D005933B"
            "50042BE0BA4C708DE8EBB52059B2059C9BFE90D8923DF74B43911BBC00BB6BFA";
    } // namespace Detail

    inline constexpr auto kDatabaseXorKey = Detail::ParseHexKey256(Detail::kDbKeyHex);

} // namespace Core
