#include "parsers/DbParser.hpp"
#include "core/BinaryReader.hpp"
#include "core/Crypto.hpp"
#include "core/Logger.hpp"

#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

    constexpr std::string_view kDbMagic = "PLPcK";
    constexpr uint16_t kExpectedHeaderSize = 0x26;

    struct DbHeader {
        uint8_t version = 0;
        uint16_t header_size = 0;
        uint32_t default_file_count = 0;
        uint32_t hash_table_count = 0;
        uint64_t hash_table_offset = 0;
    };

    std::vector<uint8_t> DecryptDb(std::span<const uint8_t> data) {
        if (data.size() < kDbMagic.size()) return {};

        const auto& key = Core::kDatabaseXorKey;
        for (size_t rot = 0; rot < key.size(); ++rot) {
            bool match = true;
            for (size_t j = 0; j < kDbMagic.size(); ++j) {
                if ((data[j] ^ key[(rot + j) & 0xFFu]) != static_cast<uint8_t>(kDbMagic[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::vector<uint8_t> decrypted(data.size());
                for (size_t j = 0; j < data.size(); ++j) {
                    decrypted[j] = data[j] ^ key[(rot + j) & 0xFFu];
                }
                return decrypted;
            }
        }
        return {};
    }

    bool ParseHeader(Core::BinaryReader& reader, DbHeader& out) noexcept {
        if (!reader.HasBytes(kExpectedHeaderSize) || !reader.MatchMagic(kDbMagic)) {
            return false;
        }
        out.version = *reader.TryRead<uint8_t>();
        out.header_size = *reader.TryRead<uint16_t>();
        if (out.header_size != kExpectedHeaderSize) {
            return false;
        }
        reader.Skip(1 + 8); // unk + unk1
        out.default_file_count = *reader.TryRead<uint32_t>();
        out.hash_table_count = *reader.TryRead<uint32_t>();
        const auto ht_offset = reader.TryReadUInt40HiLo();
        if (!ht_offset) return false;
        out.hash_table_offset = *ht_offset;
        reader.Skip(8); // unk5
        return true;
    }

    std::string_view SpanAsStringView(std::span<const uint8_t> s) noexcept {
        return {reinterpret_cast<const char*>(s.data()), s.size()};
    }

} // namespace

nlohmann::ordered_json DbParser::ParseToJson(std::span<const uint8_t> data) {
    try {
        const std::vector<uint8_t> decrypted = DecryptDb(data);
        if (decrypted.size() < kExpectedHeaderSize) {
            return nlohmann::ordered_json::object();
        }

        Core::BinaryReader reader(decrypted);
        DbHeader header{};
        if (!ParseHeader(reader, header)) {
            return nlohmann::ordered_json::object();
        }

        if (!reader.Seek(static_cast<size_t>(header.hash_table_offset)) || !reader.HasBytes(5)) {
            return nlohmann::ordered_json::object();
        }

        const uint32_t root_size = *reader.TryRead<uint32_t>();
        const uint8_t root_tag = *reader.TryRead<uint8_t>();
        if (root_tag != 1 || root_size != 5u * (header.hash_table_count + 1u)) {
            return nlohmann::ordered_json::object();
        }

        // Zero-copy map from entry name -> byte slice inside decrypted buffer
        std::unordered_map<std::string_view, std::span<const uint8_t>> entries;
        entries.reserve(header.default_file_count + 16);

        for (uint32_t i = 0; i < header.hash_table_count; ++i) {
            const auto entry_offset_opt = reader.TryReadUInt40HiLo();
            if (!entry_offset_opt) break;
            if (*entry_offset_opt == 0) continue;

            uint64_t current_pos = *entry_offset_opt;
            size_t chain_guard = 0;
            constexpr size_t kMaxChainSteps = 100000;

            while (current_pos != 0 && current_pos < decrypted.size() && ++chain_guard <= kMaxChainSteps) {
                Core::BinaryReader chunk_reader(decrypted, static_cast<size_t>(current_pos));
                if (!chunk_reader.HasBytes(15)) break;

                chunk_reader.Skip(4); // entry_size
                chunk_reader.Skip(1); // entry_type
                const uint8_t name_len = *chunk_reader.TryRead<uint8_t>();
                const uint32_t file_size = *chunk_reader.TryRead<uint32_t>();
                const auto next_entry = chunk_reader.TryReadUInt40HiLo();
                if (!next_entry) break;

                const auto name_bytes = chunk_reader.TryReadBytes(name_len);
                const auto file_bytes = chunk_reader.TryReadBytes(file_size);
                if (!name_bytes || !file_bytes) break;

                entries.emplace(SpanAsStringView(*name_bytes), *file_bytes);
                current_pos = *next_entry;
            }
        }

        uint32_t rows = 0;
        uint32_t cols = 0;
        if (const auto it = entries.find("\trows"); it != entries.end() && it->second.size() >= sizeof(uint32_t)) {
            Core::BinaryReader r(it->second);
            rows = *r.TryRead<uint32_t>();
        }
        if (const auto it = entries.find("\tcols"); it != entries.end() && it->second.size() >= sizeof(uint32_t)) {
            Core::BinaryReader r(it->second);
            cols = *r.TryRead<uint32_t>();
        }

        std::vector<std::string> col_names;
        col_names.reserve(cols);
        for (uint32_t col = 0; col < cols; ++col) {
            const std::string key = "\t" + std::to_string(col);
            if (const auto it = entries.find(key); it != entries.end()) {
                col_names.emplace_back(SpanAsStringView(it->second));
            }
        }

        nlohmann::ordered_json root_json = nlohmann::ordered_json::array();
        std::vector<std::string_view> values;
        values.reserve(col_names.size());

        for (uint32_t row = 0; row < rows; ++row) {
            const std::string entry_key = "\t\t" + std::to_string(row);
            const auto entry_it = entries.find(entry_key);
            if (entry_it == entries.end()) continue;

            const auto actual_it = entries.find(SpanAsStringView(entry_it->second));
            if (actual_it == entries.end()) continue;

            values.clear();
            const auto row_bytes = actual_it->second;
            size_t start = 0;
            for (size_t i = 0; i < row_bytes.size(); ++i) {
                if (row_bytes[i] == 0) {
                    values.push_back(SpanAsStringView(row_bytes.subspan(start, i - start)));
                    start = i + 1;
                }
            }

            nlohmann::ordered_json row_obj = nlohmann::ordered_json::object();
            const size_t count = (std::min)(col_names.size(), values.size());
            for (size_t i = 0; i < count; ++i) {
                row_obj[col_names[i]] = values[i];
            }
            root_json.push_back(std::move(row_obj));
        }

        return root_json;
    } catch (const std::exception& e) {
        LogError(std::string("DbParser::ParseToJson exception: ") + e.what());
        return nlohmann::ordered_json::object();
    } catch (...) {
        LogError("DbParser::ParseToJson unknown exception");
        return nlohmann::ordered_json::object();
    }
}

std::string DbParser::ConvertToJson(std::span<const uint8_t> data) {
    try {
        return ParseToJson(data).dump(2, ' ', false);
    } catch (const std::exception& e) {
        LogError(std::string("DbParser::ConvertToJson dump exception: ") + e.what());
        return "{}";
    }
}
