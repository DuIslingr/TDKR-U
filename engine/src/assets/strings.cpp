#include "assets/strings.h"

#include "core/vfs.h"

namespace tdkr {

namespace {

uint16_t u16(const Bytes& d, size_t p) { return p + 2 <= d.size() ? uint16_t(d[p] | d[p + 1] << 8) : 0; }
uint32_t u32(const Bytes& d, size_t p) { return p + 4 <= d.size() ? uint32_t(u16(d, p) | uint32_t(u16(d, p + 2)) << 16) : 0; }

// UTF-16LE (TDKR's tables) to UTF-8, up to the first NUL.
std::string utf8(const Bytes& d, size_t p, size_t end) {
    std::string out;
    for (size_t i = p; i + 1 < end && i + 1 < d.size(); i += 2) {
        uint32_t c = u16(d, i);
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < end) {
            const uint32_t lo = u16(d, i + 2);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 63));
        else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 63)), out += char(0x80 | (c & 63));
        else out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 63)), out += char(0x80 | (c >> 6 & 63)), out += char(0x80 | (c & 63));
    }
    return out;
}

}  // namespace

bool StringTable::load(const Vfs& vfs, const std::string& table, const std::string& lang) {
    const std::string map = table + "_map.bin", off = table + "_" + lang + ".off", bin = table + "_" + lang + ".bin";
    if (!vfs.exists(map) || !vfs.exists(off) || !vfs.exists(bin)) return false;
    // _map.bin: u16 count, then (u16 length, id with its NUL); .off: u16 count, then the
    // end offset of each string in .bin (NUL-terminated UTF-8).
    const Bytes m = vfs.read(map), o = vfs.read(off), b = vfs.read(bin);
    const size_t n = std::min<size_t>(u16(m, 0), u16(o, 0));
    // TDKR: u32 lengths and offsets, UTF-16LE ids and text.
    if (m.size() > 8 && m[4] == 0 && m[5] == 0 && m[7] == 0) {
        size_t p = 2, start = 0;
        for (size_t i = 0; i < n; ++i) {
            const size_t len = u32(m, p);
            if (p + 4 + len > m.size()) break;
            std::string id = utf8(m, p + 4, p + 4 + len);
            p += 4 + len;
            const size_t end = std::min<size_t>(u32(o, 2 + 4 * i), b.size());
            text_[id] = utf8(b, start, end);
            start = end;
        }
        return true;
    }
    size_t p = 2, start = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t len = u16(m, p);
        if (p + 2 + len > m.size()) break;
        std::string id(reinterpret_cast<const char*>(m.data()) + p + 2, len);
        while (!id.empty() && id.back() == '\0') id.pop_back();
        p += 2 + len;
        const size_t end = std::min<size_t>(u16(o, 2 + 2 * i), b.size());
        std::string text(reinterpret_cast<const char*>(b.data()) + start, end > start ? end - start : 0);
        while (!text.empty() && text.back() == '\0') text.pop_back();
        start = end;
        text_[id] = std::move(text);
    }
    return true;
}

std::string StringTable::find(const std::string& id) const {
    if (auto it = text_.find(id); it != text_.end()) return it->second;
    if (auto it = text_.find("STR_" + id); it != text_.end()) return it->second;
    return {};
}

}  // namespace tdkr
