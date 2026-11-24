#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Core {

    /**
     * Bounds-checked, alignment-safe little-endian binary reader over an immutable byte span.
     */
    class BinaryReader {
    public:
        explicit constexpr BinaryReader(std::span<const uint8_t> data, size_t offset = 0) noexcept
            : m_data(data), m_pos(offset <= data.size() ? offset : data.size()) {}

        [[nodiscard]] constexpr size_t Position() const noexcept { return m_pos; }
        [[nodiscard]] constexpr size_t Size() const noexcept { return m_data.size(); }
        [[nodiscard]] constexpr size_t Remaining() const noexcept { return m_data.size() - m_pos; }
        [[nodiscard]] constexpr bool HasBytes(size_t count) const noexcept { return count <= Remaining(); }
        [[nodiscard]] constexpr bool AtEnd() const noexcept { return m_pos >= m_data.size(); }
        [[nodiscard]] constexpr std::span<const uint8_t> Data() const noexcept { return m_data; }

        bool Seek(size_t offset) noexcept {
            if (offset > m_data.size()) return false;
            m_pos = offset;
            return true;
        }

        bool Skip(size_t count) noexcept {
            if (!HasBytes(count)) {
                m_pos = m_data.size();
                return false;
            }
            m_pos += count;
            return true;
        }

        template <typename T>
        requires std::is_trivially_copyable_v<T>
        [[nodiscard]] std::optional<T> TryRead() noexcept {
            if (!HasBytes(sizeof(T))) return std::nullopt;
            T value{};
            std::memcpy(&value, m_data.data() + m_pos, sizeof(T));
            m_pos += sizeof(T);
            return value;
        }

        template <typename T>
        requires std::is_trivially_copyable_v<T>
        [[nodiscard]] T Read() {
            auto value = TryRead<T>();
            if (!value) {
                throw std::out_of_range("BinaryReader: unexpected end of buffer");
            }
            return *value;
        }

        template <typename T>
        requires std::is_trivially_copyable_v<T>
        [[nodiscard]] std::optional<T> PeekAt(size_t offset) const noexcept {
            if (offset > m_data.size() || sizeof(T) > m_data.size() - offset) {
                return std::nullopt;
            }
            T value{};
            std::memcpy(&value, m_data.data() + offset, sizeof(T));
            return value;
        }

        /** Reads a 40-bit unsigned integer stored as [uint8_t hi, uint32_t lo_le] (used in .db format). */
        [[nodiscard]] std::optional<uint64_t> TryReadUInt40HiLo() noexcept {
            if (!HasBytes(5)) return std::nullopt;
            const auto hi = *TryRead<uint8_t>();
            const auto lo = *TryRead<uint32_t>();
            return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
        }

        [[nodiscard]] std::span<const uint8_t> ReadBytes(size_t count) {
            if (!HasBytes(count)) {
                throw std::out_of_range("BinaryReader: insufficient bytes for ReadBytes");
            }
            auto slice = m_data.subspan(m_pos, count);
            m_pos += count;
            return slice;
        }

        [[nodiscard]] std::optional<std::span<const uint8_t>> TryReadBytes(size_t count) noexcept {
            if (!HasBytes(count)) return std::nullopt;
            auto slice = m_data.subspan(m_pos, count);
            m_pos += count;
            return slice;
        }

        [[nodiscard]] bool MatchMagic(std::string_view magic) noexcept {
            if (!HasBytes(magic.size())) return false;
            const bool match = std::equal(
                magic.begin(), magic.end(),
                reinterpret_cast<const char*>(m_data.data() + m_pos));
            if (match) m_pos += magic.size();
            return match;
        }

        [[nodiscard]] bool MatchesAt(size_t offset, std::string_view magic) const noexcept {
            if (offset > m_data.size() || magic.size() > m_data.size() - offset) return false;
            return std::equal(
                magic.begin(), magic.end(),
                reinterpret_cast<const char*>(m_data.data() + offset));
        }

        [[nodiscard]] std::string ReadFixedString(size_t length) {
            const auto bytes = ReadBytes(length);
            return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        template <typename T>
        requires std::is_trivially_copyable_v<T>
        [[nodiscard]] std::vector<T> ReadArray(size_t count) {
            std::vector<T> out;
            const size_t max_possible = Remaining() / sizeof(T);
            const size_t actual = (std::min)(count, max_possible);
            out.reserve(actual);
            for (size_t i = 0; i < actual; ++i) {
                out.push_back(*TryRead<T>());
            }
            return out;
        }

        [[nodiscard]] static std::string ReadCString(std::span<const uint8_t> table, size_t offset) {
            if (offset >= table.size()) return {};
            const auto sub = table.subspan(offset);
            const auto it = std::ranges::find(sub, uint8_t{0});
            const size_t len = static_cast<size_t>(std::distance(sub.begin(), it));
            return std::string(reinterpret_cast<const char*>(sub.data()), len);
        }

    private:
        std::span<const uint8_t> m_data;
        size_t m_pos = 0;
    };

} // namespace Core
