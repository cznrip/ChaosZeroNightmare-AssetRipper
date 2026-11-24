#include "parsers/ScspParser.hpp"
#include "core/BinaryReader.hpp"
#include "core/Compression.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

    using json = nlohmann::ordered_json;

    constexpr uint32_t kInvalidStringOffset = 0xFFFFFFFFu;
    constexpr uint32_t kLegacySkinVersionThreshold = 0x7530u;
    constexpr size_t kHeaderMinSize = 0x6Au;
    constexpr size_t kBonesSectionStart = 0x08u + 0x62u;

    enum class AttachmentType : int16_t {
        Region = 0,
        BoundingBox = 1,
        Mesh = 2,
        LinkedMesh = 3,
        Path = 4,
        Point = 5,
        Clipping = 6
    };

    enum class TimelineType : uint16_t {
        Rotate = 0,
        Translate = 1,
        Scale = 2,
        Shear = 3,
        Attachment = 4,
        Color = 5,
        Deform = 6,
        Event = 7,
        DrawOrder = 8,
        IkConstraint = 9,
        TransformConstraint = 10,
        PathPosition = 11,
        PathSpacing = 12,
        PathMix = 13,
        TwoColor = 14
    };

    constexpr std::array<std::string_view, 5> kTransformModes = {
        "normal", "onlyTranslation", "noRotationOrReflection", "noScale", "noScaleOrReflection"
    };
    constexpr std::array<std::string_view, 4> kBlendModes = {
        "normal", "additive", "multiply", "screen"
    };
    constexpr std::array<std::string_view, 2> kPositionModes = {"fixed", "percent"};
    constexpr std::array<std::string_view, 3> kSpacingModes = {"length", "fixed", "percent"};
    constexpr std::array<std::string_view, 3> kRotateModes = {"tangent", "chain", "chainScale"};

    double RoundFloat(double value, int decimals = 6) noexcept {
        if (const double rounded = std::round(value); std::abs(value - rounded) < 1e-5) {
            return rounded;
        }
        const double multiplier = std::pow(10.0, decimals);
        return std::round(value * multiplier) / multiplier;
    }

    uint8_t FloatColorToByte(float channel) noexcept {
        return static_cast<uint8_t>(std::round(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
    }

    std::string RgbaToHex(float r, float g, float b, float a) {
        return std::format("{:02X}{:02X}{:02X}{:02X}",
            FloatColorToByte(r), FloatColorToByte(g), FloatColorToByte(b), FloatColorToByte(a));
    }

    std::string RgbToHex(float r, float g, float b) {
        return std::format("{:02X}{:02X}{:02X}",
            FloatColorToByte(r), FloatColorToByte(g), FloatColorToByte(b));
    }

    bool ExtractBezierFromSpineBlock(std::span<const float> block, float& cx1, float& cy1, float& cx2, float& cy2) noexcept {
        if (block.size() < 19) return false;

        const float x0 = block[1], y0 = block[2];
        const float x1 = block[3], y1 = block[4];
        const float x2 = block[5], y2 = block[6];

        const float ddfx = x1 - 2.0f * x0;
        const float dddfx = x2 - 3.0f * x1 + 3.0f * x0;
        const float ddfy = y1 - 2.0f * y0;
        const float dddfy = y2 - 3.0f * y1 + 3.0f * y0;

        constexpr float kStep = 1.0f / 10.0f;
        constexpr float kA = 3.0f * kStep * kStep;
        constexpr float kB = 6.0f * kStep * kStep * kStep;

        const float ux = (dddfx / kB - 1.0f) / 3.0f;
        const float vx = (ddfx - dddfx) / (2.0f * kA);
        cx1 = std::clamp(-vx - ux, 0.0f, 1.0f);
        cx2 = std::clamp(-vx - 2.0f * ux, 0.0f, 1.0f);

        const float uy = (dddfy / kB - 1.0f) / 3.0f;
        const float vy = (ddfy - dddfy) / (2.0f * kA);
        cy1 = std::clamp(-vy - uy, 0.0f, 1.0f);
        cy2 = std::clamp(-vy - 2.0f * uy, 0.0f, 1.0f);
        return true;
    }

    void AppendCurve(size_t frame_index, std::span<const float> curves, json& frame) {
        constexpr size_t kBezierStride = 19;
        const size_t start = frame_index * kBezierStride;
        if (start + kBezierStride > curves.size()) return;

        const auto block = curves.subspan(start, kBezierStride);
        if (block[0] == 1.0f) {
            frame["curve"] = "stepped";
        } else if (block[0] == 2.0f) {
            float cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
            if (ExtractBezierFromSpineBlock(block, cx1, cy1, cx2, cy2)) {
                frame["curve"] = RoundFloat(cx1);
                frame["c2"] = RoundFloat(cy1);
                frame["c3"] = RoundFloat(cx2);
                frame["c4"] = RoundFloat(cy2);
            }
        }
    }

    json BuildVerticesJson(const std::vector<int16_t>& bones, const std::vector<float>& verts) {
        if (bones.empty()) {
            return verts;
        }
        json out = json::array();
        size_t bi = 0;
        size_t vf = 0;
        while (bi < bones.size()) {
            const int16_t count = bones[bi++];
            out.push_back(count);
            for (int16_t k = 0; k < count && bi < bones.size() && vf + 2 < verts.size(); ++k) {
                out.push_back(bones[bi++]);
                out.push_back(verts[vf++]);
                out.push_back(verts[vf++]);
                out.push_back(verts[vf++]);
            }
        }
        return out;
    }

    /**
     * Stateful binary decoder converting a decompressed SCSP payload into Spine JSON.
     */
    class ScspDecoder {
    public:
        explicit ScspDecoder(std::span<const uint8_t> decompressed)
            : m_reader(decompressed) {}

        [[nodiscard]] ScspParser::HeaderInfo DecodeHeaderOnly() {
            ParseHeader();
            return ScspParser::HeaderInfo{
                .width = m_header.width,
                .height = m_header.height,
                .version = m_header.version,
                .images_path = m_header.images_path,
                .audio_path = m_header.audio_path,
                .hash = m_header.hash
            };
        }

        [[nodiscard]] json DecodeFull() {
            if (m_reader.Size() == 0) return json::object();

            ParseHeader();
            m_reader.Seek(kBonesSectionStart);

            json skeleton;
            skeleton["spine"] = m_header.version.empty() ? "3.8.79" : m_header.version;
            skeleton["x"] = 0.0f;
            skeleton["y"] = 0.0f;
            if (m_header.width != 0.0f) skeleton["width"] = m_header.width;
            if (m_header.height != 0.0f) skeleton["height"] = m_header.height;
            if (!m_header.hash.empty()) skeleton["hash"] = m_header.hash;
            if (!m_header.images_path.empty()) skeleton["images"] = m_header.images_path;
            if (!m_header.audio_path.empty()) skeleton["audio"] = m_header.audio_path;

            json result;
            result["skeleton"] = std::move(skeleton);
            result["bones"] = ParseBones();
            result["ik"] = ParseIkConstraints();
            result["slots"] = ParseSlots();
            result["transform"] = ParseTransformConstraints();
            result["path"] = ParsePathConstraints();
            result["skins"] = ParseSkins();
            result["events"] = ParseEvents();
            result["animations"] = ParseAnimations();
            return result;
        }

    private:
        struct Header {
            uint32_t string_offset = 0;
            uint32_t string_length = 0;
            uint32_t hdr_version = 0;
            float width = 0.0f;
            float height = 0.0f;
            std::string hash;
            std::string version;
            std::string images_path;
            std::string audio_path;
        };

        struct VertexAttachmentData {
            std::vector<int16_t> bones;
            std::vector<float> vertices;
            uint16_t vertex_count = 0;
            uint32_t world_vertices_len = 0;
            std::string path;
        };

        struct AttachmentMeta {
            bool weighted = false;
            std::vector<float> setup;
        };

        using AttachmentMetaKey = std::tuple<std::string, int, std::string>;

        [[nodiscard]] std::string ReadStringAt(uint32_t rel_offset) const {
            if (rel_offset == kInvalidStringOffset || rel_offset >= m_string_table.size()) {
                return {};
            }
            return Core::BinaryReader::ReadCString(m_string_table, rel_offset);
        }

        [[nodiscard]] std::string ReadStringRef() {
            const auto rel = m_reader.TryRead<uint32_t>();
            return rel ? ReadStringAt(*rel) : std::string{};
        }

        template <typename MapT>
        static std::string LookupOrDefault(const MapT& map, typename MapT::key_type key, std::string fallback) {
            if (const auto it = map.find(key); it != map.end()) {
                return it->second;
            }
            return fallback;
        }

        void ParseHeader() {
            if (m_reader.Size() < kHeaderMinSize) {
                throw std::runtime_error("Buffer too small for SCSP header");
            }

            m_header.string_offset = *m_reader.PeekAt<uint32_t>(0x00);
            m_header.string_length = *m_reader.PeekAt<uint32_t>(0x04);

            const size_t strings_base = static_cast<size_t>(m_header.string_offset) + 8;
            if (strings_base > m_reader.Size() || m_header.string_length > m_reader.Size() - strings_base) {
                throw std::runtime_error("SCSP string table exceeds buffer");
            }
            m_string_table = m_reader.Data().subspan(strings_base, m_header.string_length);

            if (!m_reader.MatchesAt(0x08, "scsp")) {
                throw std::runtime_error("Invalid SCSP magic");
            }

            m_header.hdr_version = *m_reader.PeekAt<uint32_t>(0x08 + 0x04);
            m_header.width = *m_reader.PeekAt<float>(0x08 + 0x0E);
            m_header.height = *m_reader.PeekAt<float>(0x08 + 0x12);

            m_header.hash = ReadStringAt(*m_reader.PeekAt<uint32_t>(0x08 + 0x4A));
            m_header.version = ReadStringAt(*m_reader.PeekAt<uint32_t>(0x08 + 0x4E));
            m_header.images_path = ReadStringAt(*m_reader.PeekAt<uint32_t>(0x08 + 0x5A));
            m_header.audio_path = ReadStringAt(*m_reader.PeekAt<uint32_t>(0x08 + 0x5E));

            if (m_header.version.ends_with(".scsp")) {
                m_header.version.resize(m_header.version.size() - 5);
            }
        }

        VertexAttachmentData ReadVertexAttachment() {
            VertexAttachmentData out;
            const auto bone_count = m_reader.TryRead<uint16_t>();
            if (!bone_count) return out;
            out.bones = m_reader.ReadArray<int16_t>(*bone_count);

            const auto vert_count = m_reader.TryRead<uint16_t>();
            if (!vert_count) return out;
            out.vertex_count = *vert_count;
            out.vertices = m_reader.ReadArray<float>(*vert_count);

            if (!m_reader.HasBytes(8)) return out;
            out.world_vertices_len = *m_reader.TryRead<uint32_t>();
            out.path = ReadStringRef();
            return out;
        }

        json ParseBones() {
            json bones = json::array();
            const auto count = m_reader.TryRead<uint16_t>();
            if (!count) return bones;

            for (uint16_t i = 0; i < *count; ++i) {
                if (!m_reader.HasBytes(43)) break;

                const auto index = *m_reader.TryRead<int16_t>();
                const std::string name = ReadStringRef();
                const auto parent = *m_reader.TryRead<int16_t>();
                const auto length = *m_reader.TryRead<float>();
                const auto x = *m_reader.TryRead<float>();
                const auto y = *m_reader.TryRead<float>();
                const auto rot = *m_reader.TryRead<float>();
                const auto sx = *m_reader.TryRead<float>();
                const auto sy = *m_reader.TryRead<float>();
                const auto shx = *m_reader.TryRead<float>();
                const auto shy = *m_reader.TryRead<float>();
                const auto tmode = *m_reader.TryRead<uint16_t>();
                const bool skin = (*m_reader.TryRead<uint8_t>()) != 0;

                if (name.empty()) continue;
                m_bone_names[index] = name;

                json bone;
                bone["name"] = name;
                if (parent >= 0) {
                    if (const auto it = m_bone_names.find(parent); it != m_bone_names.end()) {
                        bone["parent"] = it->second;
                    }
                }
                if (length != 0.0f) bone["length"] = length;
                if (x != 0.0f) bone["x"] = x;
                if (y != 0.0f) bone["y"] = y;
                if (rot != 0.0f) bone["rotation"] = rot;
                if (sx != 1.0f) bone["scaleX"] = sx;
                if (sy != 1.0f) bone["scaleY"] = sy;
                if (shx != 0.0f) bone["shearX"] = shx;
                if (shy != 0.0f) bone["shearY"] = shy;
                if (tmode < kTransformModes.size()) bone["transform"] = kTransformModes[tmode];
                if (skin) bone["skin"] = true;

                bones.push_back(std::move(bone));
            }
            return bones;
        }

        json ParseIkConstraints() {
            json iks = json::array();
            const auto count = m_reader.TryRead<uint16_t>();
            if (!count) return iks;

            for (uint16_t i = 0; i < *count; ++i) {
                if (!m_reader.HasBytes(28)) break;

                std::string name = ReadStringRef();
                if (name.empty()) name = "ik" + std::to_string(i);

                const auto order = *m_reader.TryRead<uint32_t>();
                const bool skin_required = (*m_reader.TryRead<uint8_t>()) != 0;
                const auto bend_direction = *m_reader.TryRead<int32_t>();
                const bool compress = (*m_reader.TryRead<uint8_t>()) != 0;
                const auto mix = *m_reader.TryRead<float>();
                const auto softness = *m_reader.TryRead<float>();
                const bool stretch = (*m_reader.TryRead<uint8_t>()) != 0;
                const bool uniform = (*m_reader.TryRead<uint8_t>()) != 0;
                const auto target_idx = *m_reader.TryRead<int16_t>();
                const auto bone_count = *m_reader.TryRead<uint16_t>();

                json bone_list = json::array();
                for (const int16_t bidx : m_reader.ReadArray<int16_t>(bone_count)) {
                    if (bidx >= 0) {
                        if (const auto it = m_bone_names.find(bidx); it != m_bone_names.end()) {
                            bone_list.push_back(it->second);
                        }
                    }
                }

                json ik;
                ik["name"] = name;
                ik["order"] = static_cast<int>(order);
                ik["skin"] = skin_required;
                ik["bones"] = std::move(bone_list);
                ik["target"] = (target_idx >= 0) ? LookupOrDefault(m_bone_names, target_idx, "") : "";
                ik["mix"] = mix;
                ik["softness"] = softness;
                ik["bendPositive"] = (bend_direction >= 0);
                if (compress) ik["compress"] = true;
                if (stretch) ik["stretch"] = true;
                if (uniform) ik["uniform"] = true;

                m_ik_names[i] = name;
                iks.push_back(std::move(ik));
            }
            return iks;
        }

        json ParseSlots() {
            json slots = json::array();
            const auto count = m_reader.TryRead<uint16_t>();
            if (!count) return slots;

            for (uint16_t i = 0; i < *count; ++i) {
                if (!m_reader.HasBytes(47)) break;

                const auto slot_index = *m_reader.TryRead<int16_t>();
                std::string name = ReadStringRef();
                const auto bone_idx = *m_reader.TryRead<int16_t>();

                const auto cr = *m_reader.TryRead<float>();
                const auto cg = *m_reader.TryRead<float>();
                const auto cb = *m_reader.TryRead<float>();
                const auto ca = *m_reader.TryRead<float>();

                const auto dr = *m_reader.TryRead<float>();
                const auto dg = *m_reader.TryRead<float>();
                const auto db = *m_reader.TryRead<float>();
                m_reader.Skip(4); // dark alpha unused

                const bool has_dark = (*m_reader.TryRead<uint8_t>()) != 0;
                const std::string attachment = ReadStringRef();
                const auto blend_raw = *m_reader.TryRead<uint16_t>();

                if (name.empty()) name = "slot" + std::to_string(slot_index);
                m_slot_names[slot_index] = name;

                json slot;
                slot["name"] = name;
                slot["bone"] = LookupOrDefault(m_bone_names, bone_idx, "");

                if (const std::string col_hex = RgbaToHex(cr, cg, cb, ca); col_hex != "FFFFFFFF") {
                    slot["color"] = col_hex;
                }
                if (has_dark) {
                    slot["dark"] = RgbToHex(dr, dg, db);
                }
                if (!attachment.empty()) {
                    slot["attachment"] = attachment;
                }
                if (blend_raw < kBlendModes.size() && kBlendModes[blend_raw] != "normal") {
                    slot["blend"] = kBlendModes[blend_raw];
                }

                slots.push_back(std::move(slot));
            }
            return slots;
        }

        json ParseTransformConstraints() {
            json transforms = json::array();
            const auto count = m_reader.TryRead<uint16_t>();
            if (!count) return transforms;

            for (uint16_t i = 0; i < *count; ++i) {
                if (!m_reader.HasBytes(55)) break;

                std::string name = ReadStringRef();
                if (name.empty()) name = "transform" + std::to_string(i);

                const auto order = *m_reader.TryRead<uint32_t>();
                const bool skin_req = (*m_reader.TryRead<uint8_t>()) != 0;
                const auto rotate_mix = *m_reader.TryRead<float>();
                const auto translate_mix = *m_reader.TryRead<float>();
                const auto scale_mix = *m_reader.TryRead<float>();
                const auto shear_mix = *m_reader.TryRead<float>();
                const auto offset_rot = *m_reader.TryRead<float>();
                const auto offset_x = *m_reader.TryRead<float>();
                const auto offset_y = *m_reader.TryRead<float>();
                const auto offset_sx = *m_reader.TryRead<float>();
                const auto offset_sy = *m_reader.TryRead<float>();
                const auto offset_shy = *m_reader.TryRead<float>();
                const bool is_relative = (*m_reader.TryRead<uint8_t>()) != 0;
                const bool is_local = (*m_reader.TryRead<uint8_t>()) != 0;
                const auto target_idx = *m_reader.TryRead<int16_t>();
                const auto bone_count = *m_reader.TryRead<uint16_t>();

                json bone_list = json::array();
                for (const int16_t bi : m_reader.ReadArray<int16_t>(bone_count)) {
                    bone_list.push_back(bi >= 0 ? LookupOrDefault(m_bone_names, bi, "root") : "root");
                }

                json tr;
                tr["name"] = name;
                tr["order"] = static_cast<int>(order);
                tr["skin"] = skin_req;
                tr["target"] = (target_idx >= 0) ? LookupOrDefault(m_bone_names, target_idx, "root") : "root";
                tr["bones"] = std::move(bone_list);
                tr["rotateMix"] = rotate_mix;
                tr["translateMix"] = translate_mix;
                tr["scaleMix"] = scale_mix;
                tr["shearMix"] = shear_mix;
                tr["rotation"] = offset_rot;
                tr["x"] = offset_x;
                tr["y"] = offset_y;
                tr["scaleX"] = offset_sx;
                tr["scaleY"] = offset_sy;
                tr["shearY"] = offset_shy;
                tr["relative"] = is_relative;
                tr["local"] = is_local;

                m_transform_names[i] = name;
                transforms.push_back(std::move(tr));
            }
            return transforms;
        }

        json ParsePathConstraints() {
            json paths = json::array();
            const auto count = m_reader.TryRead<uint16_t>();
            if (!count) return paths;

            for (uint16_t i = 0; i < *count; ++i) {
                if (!m_reader.HasBytes(39)) break;

                std::string name = ReadStringRef();
                if (name.empty()) name = "path" + std::to_string(i);

                const auto order = *m_reader.TryRead<uint32_t>();
                const bool skin_req = (*m_reader.TryRead<uint8_t>()) != 0;
                const auto pos_mode = *m_reader.TryRead<uint16_t>();
                const auto spacing_mode = *m_reader.TryRead<uint16_t>();
                const auto rotate_mode = *m_reader.TryRead<uint16_t>();
                const auto offset_rot = *m_reader.TryRead<float>();
                const auto position = *m_reader.TryRead<float>();
                const auto spacing = *m_reader.TryRead<float>();
                const auto rotate_mix = *m_reader.TryRead<float>();
                const auto translate_mix = *m_reader.TryRead<float>();
                const auto target_slot = *m_reader.TryRead<int16_t>();
                const auto bone_count = *m_reader.TryRead<uint16_t>();

                json bone_list = json::array();
                for (const int16_t bi : m_reader.ReadArray<int16_t>(bone_count)) {
                    bone_list.push_back(bi >= 0 ? LookupOrDefault(m_bone_names, bi, "root") : "root");
                }

                json pc;
                pc["name"] = name;
                pc["order"] = static_cast<int>(order);
                pc["skin"] = skin_req;
                pc["positionMode"] = pos_mode < kPositionModes.size() ? kPositionModes[pos_mode] : "percent";
                pc["spacingMode"] = spacing_mode < kSpacingModes.size() ? kSpacingModes[spacing_mode] : "length";
                pc["rotateMode"] = rotate_mode < kRotateModes.size() ? kRotateModes[rotate_mode] : "tangent";
                pc["rotation"] = offset_rot;
                pc["position"] = position;
                pc["spacing"] = spacing;
                pc["rotateMix"] = rotate_mix;
                pc["translateMix"] = translate_mix;
                pc["target"] = (target_slot >= 0) ? LookupOrDefault(m_slot_names, target_slot, "slot0") : "slot0";
                pc["bones"] = std::move(bone_list);

                m_path_names[i] = name;
                paths.push_back(std::move(pc));
            }
            return paths;
        }

        json ParseAttachment(const std::string& skin_name, uint16_t slot_idx, const std::string& att_name, AttachmentType type) {
            json att;
            switch (type) {
                case AttachmentType::Region: {
                    if (!m_reader.HasBytes(28 + 24)) break;
                    const auto x = *m_reader.TryRead<float>();
                    const auto y = *m_reader.TryRead<float>();
                    const auto rot = *m_reader.TryRead<float>();
                    const auto sx = *m_reader.TryRead<float>();
                    const auto sy = *m_reader.TryRead<float>();
                    const auto w = *m_reader.TryRead<float>();
                    const auto h = *m_reader.TryRead<float>();
                    m_reader.Skip(24); // 6 floats before vc/uc

                    const auto vc = m_reader.TryRead<uint16_t>();
                    if (!vc || !m_reader.Skip(4u * (*vc))) break;
                    const auto uc = m_reader.TryRead<uint16_t>();
                    if (!uc || !m_reader.Skip(4u * (*uc))) break;

                    if (!m_reader.HasBytes(20)) break;
                    const std::string path = ReadStringRef();
                    const auto cr = *m_reader.TryRead<float>();
                    const auto cg = *m_reader.TryRead<float>();
                    const auto cb = *m_reader.TryRead<float>();
                    const auto ca = *m_reader.TryRead<float>();

                    att["type"] = "region";
                    att["x"] = x;
                    att["y"] = y;
                    att["rotation"] = rot;
                    att["scaleX"] = sx;
                    att["scaleY"] = sy;
                    att["width"] = w;
                    att["height"] = h;
                    if (!path.empty()) att["path"] = path;
                    if (const std::string color = RgbaToHex(cr, cg, cb, ca); color != "FFFFFFFF") {
                        att["color"] = color;
                    }
                    break;
                }
                case AttachmentType::BoundingBox: {
                    const auto va = ReadVertexAttachment();
                    const bool weighted = !va.bones.empty();
                    m_attachment_meta[{skin_name, slot_idx, att_name}] = {weighted, weighted ? std::vector<float>{} : va.vertices};

                    att["type"] = "boundingbox";
                    att["vertexCount"] = static_cast<int>(va.world_vertices_len >> 1);
                    att["vertices"] = BuildVerticesJson(va.bones, va.vertices);
                    if (!va.path.empty()) att["path"] = va.path;
                    break;
                }
                case AttachmentType::Mesh:
                case AttachmentType::LinkedMesh: {
                    const auto va = ReadVertexAttachment();
                    const bool weighted = !va.bones.empty();
                    m_attachment_meta[{skin_name, slot_idx, att_name}] = {weighted, weighted ? std::vector<float>{} : va.vertices};

                    m_reader.Skip(24);
                    const auto uvc = m_reader.TryRead<uint16_t>().value_or(0);
                    (void)m_reader.ReadArray<float>(uvc);

                    const auto ruvc = m_reader.TryRead<uint16_t>().value_or(0);
                    const auto region_uvs = m_reader.ReadArray<float>(ruvc);

                    const auto tc = m_reader.TryRead<uint16_t>().value_or(0);
                    const auto triangles = m_reader.ReadArray<uint16_t>(tc);

                    const auto ec = m_reader.TryRead<uint16_t>().value_or(0);
                    const auto edges = m_reader.ReadArray<uint16_t>(ec);

                    std::string mpath = va.path;
                    if (const std::string mesh_path = ReadStringRef(); !mesh_path.empty()) {
                        mpath = mesh_path;
                    }

                    m_reader.Skip(16); // regionU, regionV, regionU2, regionV2
                    const auto width = m_reader.TryRead<float>().value_or(0.0f);
                    const auto height = m_reader.TryRead<float>().value_or(0.0f);
                    m_reader.Skip(16); // rgba
                    const auto hull = m_reader.TryRead<uint32_t>().value_or(0);
                    m_reader.Skip(5); // regionRotate(1) + deg(4)
                    const std::string parent_name = ReadStringRef();

                    json verts_json = BuildVerticesJson(va.bones, va.vertices);

                    if (type == AttachmentType::LinkedMesh) {
                        std::string legacy_skin_name;
                        if (m_header.hdr_version > kLegacySkinVersionThreshold) {
                            m_reader.Skip(2);
                        } else {
                            m_reader.Skip(2); // temp_skin_idx
                            legacy_skin_name = ReadStringRef();
                        }
                        const auto skin_idx = m_reader.TryRead<int16_t>().value_or(0);
                        const bool deform_flag = m_reader.TryRead<uint8_t>().value_or(0) != 0;

                        att["type"] = "linkedmesh";
                        att["parent"] = !parent_name.empty() ? parent_name : att_name;
                        att["deform"] = deform_flag;
                        att["uvs"] = region_uvs;
                        att["triangles"] = triangles;
                        att["vertices"] = std::move(verts_json);
                        att["hull"] = hull;
                        att["edges"] = edges;
                        att["width"] = width;
                        att["height"] = height;
                        if (m_header.hdr_version > kLegacySkinVersionThreshold) {
                            att["skinIndex"] = skin_idx;
                        } else {
                            att["skin"] = legacy_skin_name.empty() ? "default" : legacy_skin_name;
                        }
                        if (!mpath.empty()) att["path"] = mpath;
                    } else {
                        m_reader.Skip(5);
                        att["type"] = "mesh";
                        att["uvs"] = region_uvs;
                        att["triangles"] = triangles;
                        att["vertices"] = std::move(verts_json);
                        att["hull"] = hull;
                        att["edges"] = edges;
                        att["width"] = width;
                        att["height"] = height;
                        if (!mpath.empty()) att["path"] = mpath;
                    }
                    break;
                }
                case AttachmentType::Path: {
                    const auto va = ReadVertexAttachment();
                    const bool weighted = !va.bones.empty();
                    m_attachment_meta[{skin_name, slot_idx, att_name}] = {weighted, weighted ? std::vector<float>{} : va.vertices};

                    const auto cnt = m_reader.TryRead<uint16_t>().value_or(0);
                    const auto lengths = m_reader.ReadArray<float>(cnt);
                    const bool closed = m_reader.TryRead<uint8_t>().value_or(0) != 0;
                    const bool constant_speed = m_reader.TryRead<uint8_t>().value_or(0) != 0;

                    att["type"] = "path";
                    att["closed"] = closed;
                    att["constantSpeed"] = constant_speed;
                    att["lengths"] = lengths;
                    att["vertexCount"] = static_cast<int>(va.world_vertices_len >> 1);
                    att["vertices"] = BuildVerticesJson(va.bones, va.vertices);
                    if (!va.path.empty()) att["path"] = va.path;
                    break;
                }
                case AttachmentType::Point: {
                    const auto x = m_reader.TryRead<float>().value_or(0.0f);
                    const auto y = m_reader.TryRead<float>().value_or(0.0f);
                    const auto rotation = m_reader.TryRead<float>().value_or(0.0f);
                    m_reader.Skip(4);

                    att["type"] = "point";
                    att["x"] = x;
                    att["y"] = y;
                    att["rotation"] = rotation;
                    break;
                }
                case AttachmentType::Clipping: {
                    const auto va = ReadVertexAttachment();
                    const bool weighted = !va.bones.empty();
                    m_attachment_meta[{skin_name, slot_idx, att_name}] = {weighted, weighted ? std::vector<float>{} : va.vertices};

                    const auto end_slot_idx = m_reader.TryRead<int16_t>().value_or(0);
                    att["type"] = "clipping";
                    att["end"] = LookupOrDefault(m_slot_names, end_slot_idx, "slot" + std::to_string(end_slot_idx));
                    att["vertexCount"] = static_cast<int>(va.world_vertices_len >> 1);
                    att["vertices"] = BuildVerticesJson(va.bones, va.vertices);
                    if (!va.path.empty()) att["path"] = va.path;
                    break;
                }
            }
            return att;
        }

        json ParseSkins() {
            json skins = json::array();
            const auto skin_count = m_reader.TryRead<uint16_t>();
            if (!skin_count) return skins;

            for (uint16_t sidx = 0; sidx < *skin_count; ++sidx) {
                if (!m_reader.HasBytes(4)) break;
                std::string name = ReadStringRef();
                if (name.empty()) name = "default";
                m_skin_names[sidx] = name;

                const auto bc = m_reader.TryRead<uint16_t>();
                if (!bc || !m_reader.Skip(2u * (*bc))) break;

                const auto cc = m_reader.TryRead<uint16_t>();
                if (!cc || !m_reader.Skip(4u * (*cc))) break;

                const auto ac = m_reader.TryRead<uint16_t>();
                if (!ac) break;

                json attachments_json = json::object();
                for (uint16_t a = 0; a < *ac; ++a) {
                    if (!m_reader.HasBytes(12)) break;
                    const auto slot_idx = *m_reader.TryRead<uint16_t>();
                    const std::string slot_name = LookupOrDefault(
                        m_slot_names, static_cast<int16_t>(slot_idx), "slot" + std::to_string(slot_idx));

                    std::string att_name = ReadStringRef();
                    if (att_name.empty()) att_name = "att" + std::to_string(a);

                    const auto atype = static_cast<AttachmentType>(*m_reader.TryRead<int16_t>());
                    m_reader.Skip(4); // ctor_name unused

                    json att = ParseAttachment(name, slot_idx, att_name, atype);
                    if (!att.empty()) {
                        if (!attachments_json.contains(slot_name)) {
                            attachments_json[slot_name] = json::object();
                        }
                        attachments_json[slot_name][att_name] = std::move(att);
                    }
                }

                json skin_obj;
                skin_obj["name"] = name;
                skin_obj["attachments"] = std::move(attachments_json);
                skins.push_back(std::move(skin_obj));
            }

            // Resolve linkedmesh skinIndex -> skin name
            for (auto& skin : skins) {
                if (!skin.contains("attachments")) continue;
                for (auto& [slot, slot_atts] : skin["attachments"].items()) {
                    for (auto& [att_name, att] : slot_atts.items()) {
                        if (att.contains("type") && att["type"] == "linkedmesh" && att.contains("skinIndex")) {
                            const int idx = att["skinIndex"].get<int>();
                            att["skin"] = LookupOrDefault(m_skin_names, idx, "default");
                            att.erase("skinIndex");
                        }
                    }
                }
            }

            return skins;
        }

        json ParseEvents() {
            json events = json::object();
            const auto event_count = m_reader.TryRead<uint16_t>();
            if (!event_count) return events;

            for (uint16_t e = 0; e < *event_count; ++e) {
                if (!m_reader.HasBytes(28)) break;
                const std::string name = ReadStringRef();
                const auto int_data = *m_reader.TryRead<uint32_t>();
                const auto float_data = *m_reader.TryRead<float>();
                const std::string string_data = ReadStringRef();
                const std::string audio_data = ReadStringRef();
                const auto volume = *m_reader.TryRead<float>();
                const auto balance = *m_reader.TryRead<float>();

                if (!name.empty()) {
                    json evt;
                    evt["int"] = int_data;
                    evt["float"] = float_data;
                    evt["string"] = string_data;
                    evt["audio"] = audio_data;
                    evt["volume"] = volume;
                    evt["balance"] = balance;
                    events[name] = std::move(evt);
                    m_event_names[e] = name;
                }
            }
            return events;
        }

        void ParseBoneTimeline(TimelineType ttype, json& anim) {
            const auto bone_idx = m_reader.TryRead<uint16_t>().value_or(0);
            const auto fc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto values = m_reader.ReadArray<float>(fc);
            const auto cc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto curves = m_reader.ReadArray<float>(cc);

            const std::string bkey = LookupOrDefault(m_bone_names, static_cast<int16_t>(bone_idx), std::to_string(bone_idx));
            std::string_view tname = "shear";
            if (ttype == TimelineType::Rotate) tname = "rotate";
            else if (ttype == TimelineType::Translate) tname = "translate";
            else if (ttype == TimelineType::Scale) tname = "scale";

            if (!anim["bones"].contains(bkey)) anim["bones"][bkey] = json::object();
            if (!anim["bones"][bkey].contains(tname)) anim["bones"][bkey][tname] = json::array();

            const size_t stride = (ttype == TimelineType::Rotate) ? 2u : 3u;
            const size_t frame_count = values.size() / stride;
            for (size_t i = 0; i < frame_count; ++i) {
                json fr;
                if (ttype == TimelineType::Rotate) {
                    fr["time"] = values[i * 2];
                    fr["angle"] = values[i * 2 + 1];
                } else {
                    fr["time"] = values[i * 3];
                    fr["x"] = values[i * 3 + 1];
                    fr["y"] = values[i * 3 + 2];
                }
                AppendCurve(i, curves, fr);
                anim["bones"][bkey][tname].push_back(std::move(fr));
            }
        }

        void ParseAttachmentTimeline(json& anim) {
            const auto slot_idx = m_reader.TryRead<uint16_t>().value_or(0);
            const auto fc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto times = m_reader.ReadArray<float>(fc);
            const auto name_cnt = m_reader.TryRead<uint16_t>().value_or(0);

            std::vector<std::string> names;
            names.reserve(name_cnt);
            for (uint16_t i = 0; i < name_cnt; ++i) {
                names.push_back(ReadStringRef());
            }

            const std::string sname = LookupOrDefault(m_slot_names, static_cast<int16_t>(slot_idx), std::to_string(slot_idx));
            if (!anim["slots"].contains(sname)) anim["slots"][sname] = json::object();
            if (!anim["slots"][sname].contains("attachment")) anim["slots"][sname]["attachment"] = json::array();

            const size_t count = (std::min)(times.size(), names.size());
            for (size_t i = 0; i < count; ++i) {
                json fr;
                fr["time"] = times[i];
                if (!names[i].empty()) fr["name"] = names[i];
                else fr["name"] = nullptr;
                anim["slots"][sname]["attachment"].push_back(std::move(fr));
            }
        }

        void ParseDeformTimeline(json& anim) {
            const auto slot_idx = m_reader.TryRead<uint16_t>().value_or(0);
            const auto fc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto values = m_reader.ReadArray<float>(fc);
            const auto cc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto curves = m_reader.ReadArray<float>(cc);

            const auto fv_frames = m_reader.TryRead<uint16_t>().value_or(0);
            std::vector<std::vector<float>> frame_vertices;
            frame_vertices.reserve(fv_frames);
            for (uint16_t i = 0; i < fv_frames; ++i) {
                const auto cnt = m_reader.TryRead<uint16_t>().value_or(0);
                frame_vertices.push_back(m_reader.ReadArray<float>(cnt));
            }

            const std::string att_name = ReadStringRef();
            std::string skin_name = "default";
            if (m_header.hdr_version > kLegacySkinVersionThreshold && m_reader.HasBytes(2)) {
                const auto sidx = *m_reader.TryRead<uint16_t>();
                skin_name = LookupOrDefault(m_skin_names, static_cast<int>(sidx), "default");
            }

            bool is_unweighted = true;
            const std::vector<float>* setup_ptr = nullptr;
            if (const auto it = m_attachment_meta.find({skin_name, static_cast<int>(slot_idx), att_name});
                it != m_attachment_meta.end())
            {
                is_unweighted = !it->second.weighted;
                setup_ptr = &it->second.setup;
            }

            const std::string sname = LookupOrDefault(m_slot_names, static_cast<int16_t>(slot_idx), std::to_string(slot_idx));
            auto& target_arr = anim["deform"][skin_name][sname][att_name];
            if (!target_arr.is_array()) target_arr = json::array();

            const size_t n = (std::min)(values.size(), frame_vertices.size());
            for (size_t i = 0; i < n; ++i) {
                json fr;
                fr["time"] = values[i];
                if (const auto& verts = frame_vertices[i]; !verts.empty()) {
                    std::vector<float> diffs;
                    if (is_unweighted && setup_ptr && setup_ptr->size() == verts.size()) {
                        diffs.resize(verts.size());
                        for (size_t k = 0; k < verts.size(); ++k) {
                            diffs[k] = verts[k] - (*setup_ptr)[k];
                        }
                    } else {
                        diffs = verts;
                    }

                    size_t start = 0;
                    while (start < diffs.size() && std::abs(diffs[start]) < 1e-6f) ++start;
                    if (start < diffs.size()) {
                        size_t end = diffs.size() - 1;
                        while (end > start && std::abs(diffs[end]) < 1e-6f) --end;

                        json v = json::array();
                        for (size_t k = start; k <= end; ++k) v.push_back(diffs[k]);
                        fr["vertices"] = std::move(v);
                        if (start > 0) fr["offset"] = static_cast<int>(start);
                    }
                }
                AppendCurve(i, curves, fr);
                target_arr.push_back(std::move(fr));
            }
        }

        void ParseDrawOrderTimeline(json& anim) {
            const int slot_count = static_cast<int>(m_slot_names.size());
            const auto fc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto times = m_reader.ReadArray<float>(fc);
            const auto groups = m_reader.TryRead<uint16_t>().value_or(0);

            json draw_order = json::array();
            for (uint16_t i = 0; i < groups; ++i) {
                const auto c = m_reader.TryRead<uint16_t>().value_or(0);
                json fr;
                fr["time"] = (i < times.size()) ? times[i] : 0.0f;

                json offsets = json::array();
                if (c == slot_count) {
                    const auto raw_order = m_reader.ReadArray<uint32_t>(c);
                    for (int orig = 0; orig < slot_count; ++orig) {
                        const auto it = std::ranges::find(raw_order, static_cast<uint32_t>(orig));
                        if (it != raw_order.end()) {
                            const int new_pos = static_cast<int>(std::distance(raw_order.begin(), it));
                            if (new_pos != orig) {
                                json off;
                                off["slot"] = LookupOrDefault(m_slot_names, static_cast<int16_t>(orig), std::to_string(orig));
                                off["offset"] = new_pos - orig;
                                offsets.push_back(std::move(off));
                            }
                        }
                    }
                } else {
                    for (uint16_t j = 0; j < c; ++j) {
                        const auto sidx = static_cast<int>(m_reader.TryRead<uint32_t>().value_or(0));
                        const auto offset = m_reader.TryRead<int32_t>().value_or(0);
                        if (offset != 0) {
                            json off;
                            off["slot"] = LookupOrDefault(m_slot_names, static_cast<int16_t>(sidx), std::to_string(sidx));
                            off["offset"] = offset;
                            offsets.push_back(std::move(off));
                        }
                    }
                }
                if (!offsets.empty()) {
                    fr["offsets"] = std::move(offsets);
                    draw_order.push_back(std::move(fr));
                }
            }
            if (!draw_order.empty()) anim["drawOrder"] = std::move(draw_order);
        }

        void ParseCurveKeyedTimeline(TimelineType ttype, json& anim) {
            const auto idx = m_reader.TryRead<uint16_t>().value_or(0);
            const auto fc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto values = m_reader.ReadArray<float>(fc);
            const auto cc = m_reader.TryRead<uint16_t>().value_or(0);
            const auto curves = m_reader.ReadArray<float>(cc);

            switch (ttype) {
                case TimelineType::Color: {
                    const std::string sname = LookupOrDefault(m_slot_names, static_cast<int16_t>(idx), std::to_string(idx));
                    auto& arr = anim["slots"][sname]["color"];
                    if (!arr.is_array()) arr = json::array();
                    const size_t frames = values.size() / 5;
                    for (size_t i = 0; i < frames; ++i) {
                        const size_t b = i * 5;
                        json fr;
                        fr["time"] = values[b];
                        fr["color"] = RgbaToHex(values[b + 1], values[b + 2], values[b + 3], values[b + 4]);
                        AppendCurve(i, curves, fr);
                        arr.push_back(std::move(fr));
                    }
                    break;
                }
                case TimelineType::IkConstraint: {
                    const std::string cname = LookupOrDefault(m_ik_names, static_cast<int>(idx), "ik" + std::to_string(idx));
                    auto& arr = anim["ik"][cname];
                    if (!arr.is_array()) arr = json::array();
                    const size_t frames = values.size() / 6;
                    for (size_t i = 0; i < frames; ++i) {
                        const size_t b = i * 6;
                        json fr;
                        fr["time"] = values[b];
                        fr["mix"] = values[b + 1];
                        fr["softness"] = values[b + 2];
                        fr["bendPositive"] = (values[b + 3] >= 0.0f);
                        if (values[b + 4] != 0.0f) fr["compress"] = true;
                        if (values[b + 5] != 0.0f) fr["stretch"] = true;
                        AppendCurve(i, curves, fr);
                        arr.push_back(std::move(fr));
                    }
                    break;
                }
                case TimelineType::TransformConstraint: {
                    const std::string cname = LookupOrDefault(m_transform_names, static_cast<int>(idx), "transform" + std::to_string(idx));
                    auto& arr = anim["transform"][cname];
                    if (!arr.is_array()) arr = json::array();
                    const size_t frames = values.size() / 5;
                    for (size_t i = 0; i < frames; ++i) {
                        const size_t b = i * 5;
                        json fr;
                        fr["time"] = values[b];
                        fr["rotateMix"] = values[b + 1];
                        fr["translateMix"] = values[b + 2];
                        fr["scaleMix"] = values[b + 3];
                        fr["shearMix"] = values[b + 4];
                        AppendCurve(i, curves, fr);
                        arr.push_back(std::move(fr));
                    }
                    break;
                }
                case TimelineType::PathPosition:
                case TimelineType::PathSpacing:
                case TimelineType::PathMix: {
                    const std::string cname = LookupOrDefault(m_path_names, static_cast<int>(idx), "path" + std::to_string(idx));
                    if (!anim["path"].contains(cname)) anim["path"][cname] = json::object();
                    if (ttype == TimelineType::PathMix) {
                        json arr = json::array();
                        const size_t frames = values.size() / 3;
                        for (size_t i = 0; i < frames; ++i) {
                            const size_t b = i * 3;
                            json fr;
                            fr["time"] = values[b];
                            fr["rotateMix"] = values[b + 1];
                            fr["translateMix"] = values[b + 2];
                            AppendCurve(i, curves, fr);
                            arr.push_back(std::move(fr));
                        }
                        anim["path"][cname]["mix"] = std::move(arr);
                    } else {
                        const std::string key = (ttype == TimelineType::PathPosition) ? "position" : "spacing";
                        json arr = json::array();
                        const size_t frames = values.size() / 2;
                        for (size_t i = 0; i < frames; ++i) {
                            const size_t b = i * 2;
                            json fr;
                            fr["time"] = values[b];
                            fr[key] = values[b + 1];
                            AppendCurve(i, curves, fr);
                            arr.push_back(std::move(fr));
                        }
                        anim["path"][cname][key] = std::move(arr);
                    }
                    break;
                }
                case TimelineType::TwoColor: {
                    const std::string sname = LookupOrDefault(m_slot_names, static_cast<int16_t>(idx), std::to_string(idx));
                    auto& arr = anim["slots"][sname]["twoColor"];
                    if (!arr.is_array()) arr = json::array();
                    const size_t frames = values.size() / 8;
                    for (size_t i = 0; i < frames; ++i) {
                        const size_t b = i * 8;
                        json fr;
                        fr["time"] = values[b];
                        fr["light"] = RgbaToHex(values[b + 1], values[b + 2], values[b + 3], values[b + 4]);
                        fr["dark"] = RgbToHex(values[b + 5], values[b + 6], values[b + 7]);
                        AppendCurve(i, curves, fr);
                        arr.push_back(std::move(fr));
                    }
                    break;
                }
                default:
                    break;
            }
        }

        json ParseAnimations() {
            json animations = json::object();
            const auto anim_count = m_reader.TryRead<uint16_t>();
            if (!anim_count) return animations;

            for (uint16_t ai = 0; ai < *anim_count; ++ai) {
                if (!m_reader.HasBytes(10)) break;

                std::string name = ReadStringRef();
                const auto duration = *m_reader.TryRead<float>();
                if (name.empty()) name = "anim" + std::to_string(ai);

                json anim;
                anim["bones"] = json::object();
                anim["slots"] = json::object();
                anim["ik"] = json::object();
                anim["transform"] = json::object();
                anim["path"] = json::object();
                anim["deform"] = json::object();
                anim["events"] = json::array();

                const auto timeline_count = *m_reader.TryRead<uint16_t>();
                for (uint16_t k = 0; k < timeline_count; ++k) {
                    const auto raw_type = m_reader.TryRead<uint16_t>();
                    if (!raw_type) break;
                    const auto ttype = static_cast<TimelineType>(*raw_type);

                    if (*raw_type <= 3) {
                        ParseBoneTimeline(ttype, anim);
                    } else if (ttype == TimelineType::Attachment) {
                        ParseAttachmentTimeline(anim);
                    } else if (ttype == TimelineType::Deform) {
                        ParseDeformTimeline(anim);
                    } else if (ttype == TimelineType::Event) {
                        const auto fc = m_reader.TryRead<uint16_t>().value_or(0);
                        (void)m_reader.ReadArray<float>(fc);
                        const auto evc = m_reader.TryRead<uint16_t>().value_or(0);
                        m_reader.Skip(4u * evc);
                    } else if (ttype == TimelineType::DrawOrder) {
                        ParseDrawOrderTimeline(anim);
                    } else if (ttype == TimelineType::Color || ttype == TimelineType::IkConstraint ||
                               ttype == TimelineType::TransformConstraint || *raw_type >= 11)
                    {
                        ParseCurveKeyedTimeline(ttype, anim);
                    } else {
                        break;
                    }
                }

                if (anim["events"].empty()) anim.erase("events");
                anim["duration"] = duration;
                animations[name] = std::move(anim);
            }
            return animations;
        }

        Core::BinaryReader m_reader;
        std::span<const uint8_t> m_string_table;
        Header m_header;

        std::unordered_map<int16_t, std::string> m_bone_names;
        std::unordered_map<int16_t, std::string> m_slot_names;
        std::unordered_map<int, std::string> m_ik_names;
        std::unordered_map<int, std::string> m_transform_names;
        std::unordered_map<int, std::string> m_path_names;
        std::unordered_map<int, std::string> m_skin_names;
        std::unordered_map<int, std::string> m_event_names;
        std::map<AttachmentMetaKey, AttachmentMeta> m_attachment_meta;
    };

} // namespace

nlohmann::ordered_json ScspParser::ParseToJson(std::span<const uint8_t> scsp_data) {
    const std::vector<uint8_t> decompressed = Core::DecompressLz4WithHeader(scsp_data, true);
    ScspDecoder decoder(decompressed);
    return decoder.DecodeFull();
}

std::string ScspParser::ConvertToJson(std::span<const uint8_t> scsp_data) {
    return ParseToJson(scsp_data).dump(4);
}

ScspParser::HeaderInfo ScspParser::ExtractHeader(std::span<const uint8_t> scsp_data) {
    const std::vector<uint8_t> decompressed = Core::DecompressLz4WithHeader(scsp_data, true);
    if (decompressed.empty()) return {};
    ScspDecoder decoder(decompressed);
    return decoder.DecodeHeaderOnly();
}
