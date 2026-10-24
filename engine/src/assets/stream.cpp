#include "assets/stream.h"

#include <algorithm>
#include <array>
#include <map>

#include "core/zipfile.h"

namespace tdkr {

namespace {

struct Attribute {
    uint32_t offset = 0;
    uint16_t semantic = 0, type = 0, components = 0, stride = 0;
};

struct Batch {
    uint16_t material = 0;
    std::vector<Attribute> attributes;
    std::map<int, std::array<float, 4>> texcoord;   // semantic -> scale u, scale v, offset u, offset v
};

struct Segment {
    uint32_t batch = 0, data_offset = 0, vertex_bytes = 0, index_bytes = 0;
};

constexpr int kSemPosition = 0, kSemNormal = 16, kSemColor = 17;

float component(const uint8_t* p, uint16_t type) {
    switch (type) {
        case 0: return float(int8_t(*p));
        case 1: return float(*p);
        case 2: return float(load_le<int16_t>(p));
        case 3: return float(load_le<uint16_t>(p));
        case 6: return load_le<float>(p);
        default: throw FormatError("stream: unsupported component type " + std::to_string(type));
    }
}

size_t component_size(uint16_t type) { return type == 6 ? 4 : type == 2 || type == 3 ? 2 : 1; }

Bytes member(const Bytes& zip, const std::string& suffix) {
    for (const std::string& name : zip_names(zip))
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            return unzip_member(zip, name);
    return {};
}

}  // namespace

Model load_level_stream(const Bytes& zip, int lod) {
    const Bytes info = member(zip, "stream_info.bin"), batch_info = member(zip, "batch_info.bin");
    const Bytes table = member(zip, "lod_table.bin"), data = member(zip, "lod_data.bin");
    if (info.size() < 32 || batch_info.empty() || table.empty() || data.empty())
        throw FormatError("stream: missing stream_info, batch_info, lod_table or lod_data");
    const uint32_t record = read_le<uint32_t>(info, 28);
    if (read_le<uint32_t>(batch_info, 0) != record || record == 0 || (batch_info.size() - 4) % record)
        throw FormatError("stream: batch_info does not match the record size");

    // Batches: u16 material, 30 bytes of attribute mapping, u8, then the glitch stream header.
    std::vector<Batch> batches((batch_info.size() - 4) / record);
    for (size_t i = 0; i < batches.size(); ++i) {
        size_t p = 4 + i * record;
        Batch& b = batches[i];
        b.material = read_le<uint16_t>(batch_info, p);
        p += 2 + 30 + 1;
        const uint32_t n = read_le<uint32_t>(batch_info, p);
        p += 4;
        for (uint32_t k = 0; k < n; ++k, p += 12) {
            Attribute a;
            a.offset = read_le<uint32_t>(batch_info, p);
            a.semantic = read_le<uint16_t>(batch_info, p + 4);
            a.type = read_le<uint16_t>(batch_info, p + 6);
            a.components = read_le<uint16_t>(batch_info, p + 8);
            a.stride = read_le<uint16_t>(batch_info, p + 10);
            b.attributes.push_back(a);
        }
        p += 24;   // position offset and scale
        for (const Attribute& a : b.attributes) {
            if (a.semantic < 1 || a.semantic > 8) continue;
            b.texcoord[a.semantic] = {read_le<float>(batch_info, p), read_le<float>(batch_info, p + 4),
                                      read_le<float>(batch_info, p + 12), read_le<float>(batch_info, p + 16)};
            p += 24;
        }
        if (b.attributes.empty()) throw FormatError("stream: batch without attributes");
    }

    // Group descriptors (offset, size) in lod_data; take each group's segments at `lod`.
    std::vector<std::vector<Segment>> by_batch(batches.size());
    const uint32_t groups = read_le<uint32_t>(table, 0);
    for (uint32_t g = 0; g < groups; ++g) {
        const uint32_t off = read_le<uint32_t>(table, 4 + 8 * size_t(g)), size = read_le<uint32_t>(table, 8 + 8 * size_t(g));
        check_range(data.size(), off, size);
        const uint32_t nlod = read_le<uint32_t>(data, off + 28);
        size_t p = off + 32;
        const uint32_t want = nlod ? std::min<uint32_t>(uint32_t(std::max(lod, 0)), nlod - 1) : 0;
        for (uint32_t l = 0; l < nlod; ++l) {
            const uint32_t nseg = read_le<uint32_t>(data, p);
            p += 4;
            for (uint32_t s = 0; s < nseg; ++s, p += 50) {
                Segment seg;
                seg.batch = read_le<uint32_t>(data, p + 4);
                seg.data_offset = read_le<uint32_t>(data, p + 32);
                seg.vertex_bytes = read_le<uint32_t>(data, p + 40);
                seg.index_bytes = read_le<uint32_t>(data, p + 44);
                if (l == want && seg.vertex_bytes && seg.batch < batches.size()) by_batch[seg.batch].push_back(seg);
            }
        }
        if (p != size_t(off) + size) throw FormatError("stream: group descriptor size mismatch");
    }

    Model model;
    if (const Bytes mats = member(zip, "_materials.bdae"); !mats.empty()) model.materials = load_bdae(mats).materials;
    ModelNode root;
    root.id = "stream";
    for (size_t b = 0; b < batches.size(); ++b) {
        if (by_batch[b].empty()) continue;
        const Batch& batch = batches[b];
        const uint32_t stride = batch.attributes[0].stride;
        Primitive prim;
        prim.material = batch.material < model.materials.size() ? int(batch.material) : -1;
        for (const Segment& seg : by_batch[b]) {
            const uint32_t count = seg.vertex_bytes / stride;
            const size_t start = size_t(seg.data_offset) + 4;   // a u32 precedes the vertices
            check_range(data.size(), start, size_t(seg.vertex_bytes) + seg.index_bytes);
            const uint32_t base = uint32_t(prim.vertices.size());
            for (uint32_t v = 0; v < count; ++v) {
                Vertex out;
                const uint8_t* vp = data.data() + start + size_t(v) * stride;
                for (const Attribute& a : batch.attributes) {
                    const size_t cs = component_size(a.type);
                    auto c = [&](int k) { return component(vp + a.offset + k * cs, a.type); };
                    if (a.semantic == kSemPosition) {
                        out.position = {c(0), c(1), c(2)};
                    } else if (a.semantic == kSemNormal) {
                        glm::vec3 n{c(0), c(1), c(2)};
                        if (a.type == 0) n /= 127.0f;
                        const float len = glm::length(n);
                        out.normal = len > 0 ? n / len : glm::vec3(0, 0, 1);
                    } else if (a.semantic == kSemColor) {
                        out.color = load_le<uint32_t>(vp + a.offset);
                    } else if (auto t = batch.texcoord.find(a.semantic); t != batch.texcoord.end()) {
                        const glm::vec2 uv{c(0) * t->second[0] + t->second[2], c(1) * t->second[1] + t->second[3]};
                        if (a.semantic == 1) out.uv0 = uv;
                        else if (a.semantic == 2) out.uv1 = uv;
                        else if (a.semantic == 3) out.uv2 = uv;
                    }
                }
                prim.vertices.push_back(out);
            }
            const size_t ip = start + seg.vertex_bytes;
            for (uint32_t k = 0; k < seg.index_bytes / 2; ++k) prim.indices.push_back(base + load_le<uint16_t>(&data[ip + 2 * k]));
        }
        Mesh mesh;
        mesh.id = "batch" + std::to_string(b);
        mesh.primitives.push_back(std::move(prim));
        root.meshes.push_back(int(model.meshes.size()));
        model.meshes.push_back(std::move(mesh));
    }
    model.nodes.push_back(std::move(root));
    model.roots.push_back(0);
    return model;
}

}  // namespace tdkr
