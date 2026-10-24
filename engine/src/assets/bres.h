// Random access to Glitch "BRES" resource images (.bdae): a memory image whose
// pointers are stored as file offsets, listed in a relocation table.
//
// TDKR keeps each mesh out of line: the geometry points at {0, n, block offset, block
// size, 0}, and the block is u32 size, u32 size - 0x2c, n - 2 self-relative offsets of
// its pointer fields, then the SMesh. Pointers inside a block are relative to their own
// address (the list leaves some out, so every pointer in a block is read that way) and
// its buffers have no u32 prefix.
#pragma once

#include <cstring>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/bytes.h"

namespace tdkr {

// A (count, pointer) collection inside a struct, with its element stride.
struct Coll {
    uint32_t count, ptr, stride;
};

class Bres {
public:
    explicit Bres(const Bytes& d) : data(d) {
        if (data.size() < 0x3C || std::memcmp(data.data(), "BRES", 4) != 0) throw FormatError("not a BRES file");
        const uint32_t n = u32(0x10), table = u32(0x18);
        for (uint32_t i = 0; i < n; ++i) relocs.insert(u32(table + 4 * i));
        root = u32(0x20);
    }
    uint32_t u32(size_t off) const { return read_le<uint32_t>(data, off); }
    int32_t i32(size_t off) const { return read_le<int32_t>(data, off); }
    float f32(size_t off) const { return read_le<float>(data, off); }
    uint32_t ptr(size_t off) const {
        const uint32_t v = u32(off);
        if (!v) return 0;
        if (relocs.count(uint32_t(off))) return v;
        return in_block(off) ? uint32_t(off + v) : 0;
    }
    // A vertex, index or influence buffer: past the u32 prefix, which blocks do not have.
    uint32_t buffer(size_t off) const {
        const uint32_t p = ptr(off);
        return p && !in_block(off) ? p + 4 : p;
    }
    // If the geometry's mesh pointer at `off` leads to a TDKR mesh block, register the
    // block and return its SMesh; else the plain pointer.
    uint32_t mesh(size_t off) {
        const uint32_t p = ptr(off);
        if (!p || u32(p) != 0 || relocs.count(p + 8) || !u32(p + 8)) return p;
        const uint32_t n = u32(p + 4), block = u32(p + 8), size = u32(p + 12);
        check_range(data.size(), block, size);
        blocks[block] = block + size;
        return block + 8 + 4 * (n - 2);
    }
    std::string cstr(size_t off) const {
        const uint32_t p = ptr(off);
        if (!p) return {};
        check_range(data.size(), p, 0);
        const auto* begin = reinterpret_cast<const char*>(data.data() + p);
        return std::string(begin, strnlen(begin, data.size() - p));
    }
    std::vector<uint32_t> array(uint32_t base, Coll c) const {
        const int32_t n = i32(base + c.count);
        const uint32_t p = ptr(base + c.ptr);
        std::vector<uint32_t> out;
        if (n <= 0 || !p) return out;
        for (int32_t i = 0; i < n; ++i) out.push_back(p + uint32_t(i) * c.stride);
        return out;
    }

    bool in_block(size_t off) const {
        auto it = blocks.upper_bound(uint32_t(off));
        return it != blocks.begin() && off < (--it)->second;
    }

    const Bytes& data;
    std::unordered_set<uint32_t> relocs;
    std::map<uint32_t, uint32_t> blocks;   // TDKR mesh blocks: start -> end
    uint32_t root = 0;
};

}  // namespace tdkr
