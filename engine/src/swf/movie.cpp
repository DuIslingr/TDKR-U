#include "swf/movie.h"

#include <cmath>

#include <zlib.h>

namespace tdkr::swf {

Matrix Matrix::operator*(const Matrix& n) const {
    return {a * n.a + c * n.b, b * n.a + d * n.b, a * n.c + c * n.d, b * n.c + d * n.d,
            a * n.tx + c * n.ty + tx, b * n.tx + d * n.ty + ty};
}

Matrix Matrix::inverse() const {
    const float det = a * d - b * c;
    if (std::abs(det) < 1e-12f) return {1, 0, 0, 1, -tx, -ty};
    const float id = 1.0f / det;
    Matrix m{d * id, -b * id, -c * id, a * id, 0, 0};
    m.tx = -(m.a * tx + m.c * ty);
    m.ty = -(m.b * tx + m.d * ty);
    return m;
}

float Matrix::x_scale() const { return std::sqrt(a * a + b * b); }

float Matrix::y_scale() const {
    // Flash keeps the sign of a reflection on the y axis.
    const float s = std::sqrt(c * c + d * d);
    return (a * d - b * c) < 0 ? -s : s;
}

float Matrix::rotation() const { return std::atan2(b, a); }

void Matrix::set_scale_rotation(float xs, float ys, float rot) {
    // Keeps the skew between the axes Flash would (none): x axis at `rot`, y axis 90 degrees on.
    const float cs = std::cos(rot), sn = std::sin(rot);
    a = xs * cs;
    b = xs * sn;
    c = -ys * sn;
    d = ys * cs;
}

int SpriteDef::label_frame(const std::string& label) const {
    const std::string want = to_lower(label);
    for (size_t i = 0; i < frames.size(); ++i)
        if (!frames[i].label.empty() && to_lower(frames[i].label) == want) return int(i);
    return -1;
}

const CharacterDef* MovieDef::character(uint16_t id) const {
    auto it = characters.find(id);
    return it == characters.end() ? nullptr : it->second.get();
}

uint16_t MovieDef::exported(const std::string& n) const {
    if (auto it = exports.find(n); it != exports.end()) return it->second;
    const std::string want = to_lower(n);
    for (const auto& [k, v] : exports)
        if (to_lower(k) == want) return v;
    return 0;
}

std::string MovieDef::export_name(uint16_t id) const {
    for (const auto& [k, v] : exports)
        if (v == id) return k;
    return {};
}

namespace {

Bytes inflate_all(const uint8_t* p, size_t n, size_t hint = 0) {
    Bytes out(hint ? hint : n * 4 + 1024);
    z_stream zs{};
    zs.next_in = const_cast<uint8_t*>(p);
    zs.avail_in = uInt(n);
    inflateInit(&zs);
    int rc = Z_OK;
    while (rc == Z_OK) {
        if (zs.total_out == out.size()) out.resize(out.size() * 2);
        zs.next_out = out.data() + zs.total_out;
        zs.avail_out = uInt(out.size() - zs.total_out);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_BUF_ERROR && zs.avail_out == 0) rc = Z_OK;
    }
    out.resize(zs.total_out);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) throw FormatError("swf: bad zlib data");
    return out;
}

class Reader {
public:
    Reader(const Bytes& d, size_t p, size_t end) : d_(d), p_(p), end_(end) {}
    size_t pos() const { return p_; }
    void seek(size_t p) { p_ = p, bit_ = 0; }
    size_t end() const { return end_; }

    void align() {
        if (bit_) bit_ = 0, ++p_;
    }
    uint32_t ub(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (p_ >= d_.size()) throw FormatError("swf: bits past end");
            v = (v << 1) | ((d_[p_] >> (7 - bit_)) & 1);
            if (++bit_ == 8) bit_ = 0, ++p_;
        }
        return v;
    }
    int32_t sb(int n) {
        const uint32_t v = ub(n);
        return n && (v & (1u << (n - 1))) ? int32_t(v) - int32_t(1u << n) : int32_t(v);
    }
    float fb(int n) { return float(sb(n)) / 65536.0f; }
    uint8_t u8() {
        align();
        check_range(d_.size(), p_, 1);
        return d_[p_++];
    }
    uint16_t u16() {
        align();
        const uint16_t v = read_le<uint16_t>(d_, p_);
        p_ += 2;
        return v;
    }
    int16_t s16() { return int16_t(u16()); }
    uint32_t u32() {
        align();
        const uint32_t v = read_le<uint32_t>(d_, p_);
        p_ += 4;
        return v;
    }
    std::string string() {
        align();
        size_t e = p_;
        while (e < d_.size() && d_[e]) ++e;
        std::string s(reinterpret_cast<const char*>(d_.data() + p_), e - p_);
        p_ = e + 1;
        return s;
    }
    Rect rect() {
        align();
        const int n = int(ub(5));
        Rect r;
        r.xmin = float(sb(n));
        r.xmax = float(sb(n));
        r.ymin = float(sb(n));
        r.ymax = float(sb(n));
        align();
        return r;
    }
    Matrix matrix() {
        align();
        Matrix m;
        if (ub(1)) {
            const int n = int(ub(5));
            m.a = fb(n);
            m.d = fb(n);
        }
        if (ub(1)) {
            const int n = int(ub(5));
            m.b = fb(n);
            m.c = fb(n);
        }
        const int n = int(ub(5));
        m.tx = float(sb(n));
        m.ty = float(sb(n));
        align();
        return m;
    }
    CxForm cxform(bool alpha) {
        align();
        const bool has_add = ub(1), has_mul = ub(1);
        const int n = int(ub(4));
        const int k = alpha ? 4 : 3;
        CxForm cx;
        if (has_mul)
            for (int i = 0; i < k; ++i) cx.mul[i] = float(sb(n)) / 256.0f;
        if (has_add)
            for (int i = 0; i < k; ++i) cx.add[i] = float(sb(n)) / 255.0f;
        align();
        return cx;
    }
    glm::vec4 rgb(bool alpha) {
        align();
        check_range(d_.size(), p_, alpha ? 4 : 3);
        glm::vec4 c(d_[p_] / 255.0f, d_[p_ + 1] / 255.0f, d_[p_ + 2] / 255.0f, alpha ? d_[p_ + 3] / 255.0f : 1.0f);
        p_ += alpha ? 4 : 3;
        return c;
    }

private:
    const Bytes& d_;
    size_t p_;
    size_t end_;
    int bit_ = 0;
};

Edge reversed(const Edge& e) { return {e.p1, e.control, e.p0, e.curve}; }

class Parser {
public:
    Parser(MovieDef& m, const Bytes& d) : m_(m), d_(d) {}

    void timeline(SpriteDef& sprite, size_t p, size_t end) {
        sprite.frames.emplace_back();
        while (p + 2 <= end) {
            const uint16_t h = read_le<uint16_t>(d_, p);
            p += 2;
            const uint32_t code = h >> 6;
            size_t len = h & 0x3F;
            if (len == 0x3F) {
                len = read_le<uint32_t>(d_, p);
                p += 4;
            }
            check_range(d_.size(), p, len);
            Reader r(d_, p, p + len);
            Frame& f = sprite.frames.back();
            switch (code) {
                case 0: p = end; continue;
                case 1: sprite.frames.emplace_back(); break;
                case 2: shape(r, 1); break;
                case 22: shape(r, 2); break;
                case 32: shape(r, 3); break;
                case 83: shape(r, 4); break;
                case 20: bitmap(r, false); break;
                case 36: bitmap(r, true); break;
                case 37: edit_text(r); break;
                case 48:
                case 75: font(r, code == 75); break;
                case 39: {
                    auto s = std::make_shared<SpriteDef>();
                    s->id = r.u16();
                    s->frame_count = r.u16();
                    timeline(*s, r.pos(), p + len);
                    m_.characters[s->id] = s;
                    break;
                }
                case 26:
                case 70: f.controls.push_back({Control::Place, place(r, code == 70)}); break;
                case 28: {
                    Control c{Control::Remove, {}};
                    c.place.depth = r.u16();
                    f.controls.push_back(std::move(c));
                    break;
                }
                case 5: {
                    r.u16();
                    Control c{Control::Remove, {}};
                    c.place.depth = r.u16();
                    f.controls.push_back(std::move(c));
                    break;
                }
                case 43: f.label = r.string(); break;
                case 12: f.actions.push_back(std::make_shared<const Bytes>(d_.begin() + long(p), d_.begin() + long(p + len))); break;
                case 59: {
                    const uint16_t id = r.u16();
                    f.init_actions.emplace_back(id, std::make_shared<const Bytes>(d_.begin() + long(p + 2), d_.begin() + long(p + len)));
                    break;
                }
                case 56: {
                    const uint16_t n = r.u16();
                    for (uint16_t i = 0; i < n; ++i) {
                        const uint16_t id = r.u16();
                        m_.exports[r.string()] = id;
                    }
                    break;
                }
                case 9: m_.background = r.rgb(false); break;
                default: break;
            }
            p += len;
        }
        if (!sprite.frames.empty() && sprite.frames.back().controls.empty() && sprite.frames.back().actions.empty() &&
            sprite.frames.back().label.empty() && sprite.frames.back().init_actions.empty() &&
            sprite.frames.size() > std::max<size_t>(sprite.frame_count, 1))
            sprite.frames.pop_back();
        while (sprite.frames.size() < sprite.frame_count) sprite.frames.emplace_back();
        if (sprite.frames.size() > sprite.frame_count && sprite.frame_count) sprite.frames.resize(sprite.frame_count);
    }

private:
    FillStyle fill_style(Reader& r, int ver) {
        FillStyle fs;
        const uint8_t t = r.u8();
        if (t == 0) {
            fs.kind = FillStyle::Solid;
            fs.color = r.rgb(ver >= 3);
        } else if (t == 0x10 || t == 0x12 || t == 0x13) {
            fs.kind = t == 0x10 ? FillStyle::Linear : t == 0x12 ? FillStyle::Radial : FillStyle::Focal;
            fs.matrix = r.matrix();
            r.align();
            fs.spread = uint8_t(r.ub(2));
            r.ub(2);
            const int n = int(r.ub(4));
            for (int i = 0; i < n; ++i) {
                const float ratio = r.u8() / 255.0f;
                fs.stops.emplace_back(ratio, r.rgb(ver >= 3));
            }
            if (t == 0x13) fs.focal = r.s16() / 256.0f;
            if (!fs.stops.empty()) fs.color = fs.stops.front().second;
        } else if (t >= 0x40 && t <= 0x43) {
            fs.kind = FillStyle::Bitmap;
            fs.bitmap = r.u16();
            fs.matrix = r.matrix();
            fs.repeat = t == 0x40 || t == 0x41;
            fs.smooth = t == 0x40 || t == 0x42;
            fs.color = glm::vec4(1.0f);
        } else {
            throw FormatError("swf: fill style " + std::to_string(t));
        }
        return fs;
    }

    std::vector<FillStyle> fill_styles(Reader& r, int ver) {
        size_t n = r.u8();
        if (n == 0xFF && ver >= 2) n = r.u16();
        std::vector<FillStyle> out;
        for (size_t i = 0; i < n; ++i) out.push_back(fill_style(r, ver));
        return out;
    }

    std::vector<LineStyle> line_styles(Reader& r, int ver) {
        size_t n = r.u8();
        if (n == 0xFF) n = r.u16();
        std::vector<LineStyle> out;
        for (size_t i = 0; i < n; ++i) {
            LineStyle ls;
            ls.width = r.u16();
            if (ver == 4) {
                r.ub(2);
                const uint32_t join = r.ub(2);
                const bool has_fill = r.ub(1);
                r.ub(3);
                r.ub(5);
                r.ub(1);
                r.ub(2);
                if (join == 2) r.u16();
                ls.color = has_fill ? fill_style(r, ver).color : r.rgb(true);
            } else {
                ls.color = r.rgb(ver >= 3);
            }
            out.push_back(ls);
        }
        return out;
    }

    std::vector<ShapeGroup> records(Reader& r, int ver, std::vector<FillStyle> fills, std::vector<LineStyle> lines,
                                    int nf, int nl) {
        std::vector<ShapeGroup> groups(1);
        auto start = [&](ShapeGroup& g) {
            g.fills = std::move(fills);
            g.lines = std::move(lines);
            g.fill_edges.resize(g.fills.size());
            g.line_edges.resize(g.lines.size());
        };
        start(groups.back());
        int32_t x = 0, y = 0;
        uint32_t f0 = 0, f1 = 0, ln = 0;
        for (;;) {
            if (r.ub(1) == 0) {
                const uint32_t flags = r.ub(5);
                if (flags == 0) break;
                if (flags & 1) {
                    const int n = int(r.ub(5));
                    x = r.sb(n);
                    y = r.sb(n);
                }
                if (flags & 2) f0 = r.ub(nf);
                if (flags & 4) f1 = r.ub(nf);
                if (flags & 8) ln = r.ub(nl);
                if (flags & 16) {
                    fills = fill_styles(r, ver);
                    lines = line_styles(r, ver);
                    r.align();
                    nf = int(r.ub(4));
                    nl = int(r.ub(4));
                    groups.emplace_back();
                    start(groups.back());
                    f0 = f1 = ln = 0;
                }
                continue;
            }
            Edge e;
            e.p0 = glm::vec2(float(x), float(y));
            const bool straight = r.ub(1);
            const int n = int(r.ub(4)) + 2;
            if (straight) {
                int32_t dx = 0, dy = 0;
                if (r.ub(1)) {
                    dx = r.sb(n);
                    dy = r.sb(n);
                } else if (r.ub(1)) {
                    dy = r.sb(n);
                } else {
                    dx = r.sb(n);
                }
                x += dx;
                y += dy;
                e.p1 = glm::vec2(float(x), float(y));
                e.control = (e.p0 + e.p1) * 0.5f;
            } else {
                const int32_t cx = x + r.sb(n), cy = y + r.sb(n);
                x = cx + r.sb(n);
                y = cy + r.sb(n);
                e.control = glm::vec2(float(cx), float(cy));
                e.p1 = glm::vec2(float(x), float(y));
                e.curve = true;
            }
            ShapeGroup& g = groups.back();
            if (f1 && f1 <= g.fill_edges.size()) g.fill_edges[f1 - 1].push_back(e);
            if (f0 && f0 <= g.fill_edges.size()) g.fill_edges[f0 - 1].push_back(reversed(e));
            if (ln && ln <= g.line_edges.size()) g.line_edges[ln - 1].push_back(e);
        }
        r.align();
        return groups;
    }

    void shape(Reader& r, int ver) {
        auto s = std::make_shared<ShapeDef>();
        s->id = r.u16();
        s->bounds = r.rect();
        if (ver == 4) {
            r.rect();
            r.u8();
        }
        auto fills = fill_styles(r, ver);
        auto lines = line_styles(r, ver);
        r.align();
        const int nf = int(r.ub(4)), nl = int(r.ub(4));
        s->groups = records(r, ver, std::move(fills), std::move(lines), nf, nl);
        m_.characters[s->id] = s;
    }

    void bitmap(Reader& r, bool alpha) {
        auto b = std::make_shared<BitmapDef>();
        b->id = r.u16();
        const uint8_t fmt = r.u8();
        b->width = r.u16();
        b->height = r.u16();
        const int ncolors = fmt == 3 ? r.u8() + 1 : 0;
        m_.characters[b->id] = b;
        if (r.pos() >= r.end()) return;     // an atlas placeholder
        const Bytes raw = inflate_all(d_.data() + r.pos(), r.end() - r.pos());
        const size_t w = size_t(b->width), h = size_t(b->height);
        b->rgba.assign(w * h * 4, 0);
        auto put = [&](size_t i, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
            if (alpha && ca && ca < 255) {
                cr = uint8_t(std::min(255, cr * 255 / ca));
                cg = uint8_t(std::min(255, cg * 255 / ca));
                cb = uint8_t(std::min(255, cb * 255 / ca));
            }
            b->rgba[i * 4] = cr, b->rgba[i * 4 + 1] = cg, b->rgba[i * 4 + 2] = cb, b->rgba[i * 4 + 3] = ca;
        };
        if (fmt == 3) {
            const size_t cs = alpha ? 4 : 3, stride = (w + 3) & ~size_t(3), base = size_t(ncolors) * cs;
            check_range(raw.size(), 0, base + stride * h);
            for (size_t y = 0; y < h; ++y)
                for (size_t x = 0; x < w; ++x) {
                    const size_t c = raw[base + y * stride + x] * cs;
                    if (c + cs > base) continue;
                    put(y * w + x, raw[c], raw[c + 1], raw[c + 2], alpha ? raw[c + 3] : 255);
                }
        } else if (fmt == 5) {
            check_range(raw.size(), 0, w * h * 4);
            for (size_t i = 0; i < w * h; ++i)
                put(i, raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3], alpha ? raw[i * 4] : 255);
        } else if (fmt == 4) {
            const size_t stride = (w * 2 + 3) & ~size_t(3);
            check_range(raw.size(), 0, stride * h);
            for (size_t y = 0; y < h; ++y)
                for (size_t x = 0; x < w; ++x) {
                    const uint16_t v = uint16_t(raw[y * stride + x * 2] << 8 | raw[y * stride + x * 2 + 1]);
                    put(y * w + x, uint8_t((v >> 10 & 31) * 255 / 31), uint8_t((v >> 5 & 31) * 255 / 31),
                        uint8_t((v & 31) * 255 / 31), 255);
                }
        }
    }

    void edit_text(Reader& r) {
        auto t = std::make_shared<EditTextDef>();
        t->id = r.u16();
        t->bounds = r.rect();
        const uint8_t b1 = r.u8(), b2 = r.u8();
        const bool has_text = b1 & 0x80, has_color = b1 & 0x04, has_max = b1 & 0x02, has_font = b1 & 0x01;
        t->word_wrap = b1 & 0x40;
        t->multiline = b1 & 0x20;
        t->password = b1 & 0x10;
        t->read_only = b1 & 0x08;
        const bool has_font_class = b2 & 0x80, has_layout = b2 & 0x20;
        t->auto_size = b2 & 0x40;
        t->no_select = b2 & 0x10;
        t->border = b2 & 0x08;
        t->html = b2 & 0x02;
        t->use_outlines = b2 & 0x01;
        if (has_font) t->font = r.u16();
        if (has_font_class) r.string();
        if (has_font || has_font_class) t->height = r.u16();
        if (has_color) t->color = r.rgb(true);
        if (has_max) t->max_length = r.u16();
        if (has_layout) {
            t->align = r.u8();
            t->left_margin = r.u16();
            t->right_margin = r.u16();
            t->indent = r.u16();
            t->leading = r.s16();
        }
        t->variable = r.string();
        if (has_text) t->text = r.string();
        m_.characters[t->id] = t;
    }

    void font(Reader& r, bool font3) {
        auto f = std::make_shared<FontDef>();
        f->id = r.u16();
        const uint8_t flags = r.u8();
        const bool has_layout = flags & 0x80, wide_offsets = flags & 0x08, wide_codes = flags & 0x04;
        f->italic = flags & 0x02;
        f->bold = flags & 0x01;
        r.u8();
        const uint8_t name_len = r.u8();
        check_range(d_.size(), r.pos(), name_len);
        f->name.assign(reinterpret_cast<const char*>(d_.data() + r.pos()), name_len);
        while (!f->name.empty() && f->name.back() == '\0') f->name.pop_back();
        r.seek(r.pos() + name_len);
        const uint16_t n = r.u16();
        const size_t table = r.pos();
        std::vector<uint32_t> offsets(n);
        for (auto& o : offsets) o = wide_offsets ? r.u32() : r.u16();
        const uint32_t code_off = n || r.pos() < r.end() ? (wide_offsets ? r.u32() : r.u16()) : 0;
        f->em = font3 ? 1024.0f * 20.0f : 1024.0f;
        for (uint16_t i = 0; i < n; ++i) {
            Reader g(d_, table + offsets[i], r.end());
            const int nf = int(g.ub(4)), nl = int(g.ub(4));
            FillStyle solid;
            auto groups = records(g, 1, {solid}, {}, nf, nl);
            f->glyphs.push_back(std::move(groups.front()));
        }
        if (n) {
            r.seek(table + code_off);
            for (uint16_t i = 0; i < n; ++i) f->index[wide_codes ? r.u16() : r.u8()] = i;
        }
        if (has_layout && r.pos() + 6 <= r.end()) {
            f->ascent = r.u16();
            f->descent = r.u16();
            f->leading = r.s16();
            for (uint16_t i = 0; i < n; ++i) f->advances.push_back(r.s16());
        }
        m_.characters[f->id] = f;
    }

    PlaceTag place(Reader& r, bool v3) {
        PlaceTag pl;
        const uint8_t flags = r.u8();
        const uint8_t flags2 = v3 ? r.u8() : 0;
        pl.depth = r.u16();
        pl.move = flags & 0x01;
        if (v3 && ((flags2 & 0x08) || ((flags2 & 0x10) && (flags & 0x02)))) r.string();
        if ((pl.has_char = flags & 0x02)) pl.character = r.u16();
        if ((pl.has_matrix = flags & 0x04)) pl.matrix = r.matrix();
        if ((pl.has_cxform = flags & 0x08)) pl.cxform = r.cxform(true);
        if ((pl.has_ratio = flags & 0x10)) pl.ratio = r.u16();
        if ((pl.has_name = flags & 0x20)) pl.name = r.string();
        if (flags & 0x40) pl.clip_depth = r.u16();
        if (v3) {
            if (flags2 & 0x01) skip_filters(r);
            if (flags2 & 0x02) pl.blend = r.u8();
            if (flags2 & 0x04) r.u8();
            if (flags2 & 0x20) {
                pl.visible = int8_t(r.u8());
                r.u32();
            }
        }
        if (flags & 0x80) {
            r.u16();
            if (m_.version >= 6) r.u32(); else r.u16();
            while (r.pos() < r.end()) {
                const uint32_t ev = m_.version >= 6 ? r.u32() : r.u16();
                if (!ev) break;
                uint32_t size = r.u32();
                ClipAction ca;
                ca.events = ev;
                if (ev & (1u << 17)) {
                    ca.key = r.u8();
                    --size;
                }
                check_range(d_.size(), r.pos(), size);
                ca.code = std::make_shared<const Bytes>(d_.begin() + long(r.pos()), d_.begin() + long(r.pos() + size));
                r.seek(r.pos() + size);
                pl.clip_actions.push_back(std::move(ca));
            }
        }
        return pl;
    }

    static void skip_filters(Reader& r) {
        const uint8_t n = r.u8();
        for (uint8_t i = 0; i < n; ++i) {
            const uint8_t t = r.u8();
            size_t skip = 0;
            switch (t) {
                case 0: skip = 23; break;
                case 1: skip = 9; break;
                case 2: skip = 15; break;
                case 3: skip = 27; break;
                case 4:
                case 7: skip = size_t(r.u8()) * 5 + 19; break;
                case 5: {
                    const size_t x = r.u8(), y = r.u8();
                    skip = 8 + x * y * 4 + 5;
                    break;
                }
                case 6: skip = 80; break;
                default: break;
            }
            r.seek(r.pos() + skip);
        }
    }

    MovieDef& m_;
    const Bytes& d_;
};

}  // namespace

std::shared_ptr<MovieDef> load_movie(const Bytes& raw, const std::string& name) {
    if (raw.size() < 8) throw FormatError("swf: too short");
    Bytes data;
    if (raw[0] == 'C' && raw[1] == 'W' && raw[2] == 'S') {
        const uint32_t size = read_le<uint32_t>(raw, 4);
        data.assign(raw.begin(), raw.begin() + 8);
        const Bytes body = inflate_all(raw.data() + 8, raw.size() - 8, size);
        data.insert(data.end(), body.begin(), body.end());
    } else if (raw[0] == 'F' && raw[1] == 'W' && raw[2] == 'S') {
        data = raw;
    } else {
        throw FormatError("swf: not a SWF file");
    }
    auto m = std::make_shared<MovieDef>();
    m->name = name;
    m->version = data[3];
    Reader r(data, 8, data.size());
    m->frame_size = r.rect();
    m->frame_rate = r.u16() / 256.0f;
    if (m->frame_rate <= 0) m->frame_rate = 12;
    m->root.frame_count = r.u16();
    Parser(*m, data).timeline(m->root, r.pos(), data.size());
    return m;
}

}  // namespace tdkr::swf
