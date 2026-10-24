// tdkr-check swf: play one of the game's Flash movies headless and write its frame as a PNG
// (a software rasteriser over the player's draw list; text runs drawn as tinted boxes).
//
//   tdkr-check swf DATA_DIR MOVIE.swf OUT.png [--size WxH] [--frames N] [STEP...]
//   STEP: goto=PATH:LABEL  stop=PATH:LABEL  call=PATH:METHOD[:ARG...]  hide=PATH  show=PATH
//         set=PATH:MEMBER:VALUE  text=PATH:TEXT  load=PATH:MOVIE  frames=N  tree=PATH
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "assets/strings.h"
#include "core/vfs.h"
#include "swf/library.h"
#include "swf/player.h"
#include "swf/steps.h"

using namespace tdkr;

namespace {

void write_png(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + long(size_t(y) * size_t(w) * 4), rgba.begin() + long(size_t(y + 1) * size_t(w) * 4));
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zlen);
    compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6);
    z.resize(zlen);
    std::ofstream f(path, std::ios::binary);
    auto be32 = [&](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        f.write(reinterpret_cast<const char*>(b), 4);
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(uint32_t(data.size()));
        f.write(type, 4);
        f.write(reinterpret_cast<const char*>(data.data()), long(data.size()));
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        crc = crc32(crc, data.data(), uInt(data.size()));
        be32(uint32_t(crc));
    };
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<uint8_t> ihdr(13);
    for (int i = 0; i < 4; ++i) ihdr[size_t(i)] = uint8_t(uint32_t(w) >> (24 - 8 * i)), ihdr[size_t(4 + i)] = uint8_t(uint32_t(h) >> (24 - 8 * i));
    ihdr[8] = 8, ihdr[9] = 6;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
}

struct Canvas {
    int w, h;
    std::vector<glm::vec4> px;
    Canvas(int w_, int h_, glm::vec4 bg) : w(w_), h(h_), px(size_t(w_) * size_t(h_), bg) {}

    template <typename Shade>
    void triangle(const swf::DrawVertex* t, Shade shade) {
        const float x0 = std::min({t[0].pos.x, t[1].pos.x, t[2].pos.x}), x1 = std::max({t[0].pos.x, t[1].pos.x, t[2].pos.x});
        const float y0 = std::min({t[0].pos.y, t[1].pos.y, t[2].pos.y}), y1 = std::max({t[0].pos.y, t[1].pos.y, t[2].pos.y});
        const glm::vec2 a = t[0].pos, b = t[1].pos, c = t[2].pos;
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-6f) return;
        for (int y = std::max(0, int(std::floor(y0))); y <= std::min(h - 1, int(std::ceil(y1))); ++y)
            for (int x = std::max(0, int(std::floor(x0))); x <= std::min(w - 1, int(std::ceil(x1))); ++x) {
                const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
                const float w0 = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / area;
                const float w1 = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / area;
                const float w2 = 1 - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                const glm::vec2 uv = t[0].uv * w0 + t[1].uv * w1 + t[2].uv * w2;
                const glm::vec4 col = t[0].color * w0 + t[1].color * w1 + t[2].color * w2;
                const glm::vec4 s = shade(uv) * col;
                glm::vec4& d = px[size_t(y) * size_t(w) + size_t(x)];
                d = glm::vec4(glm::vec3(s) * s.a + glm::vec3(d) * (1 - s.a), s.a + d.a * (1 - s.a));
            }
    }
};

glm::vec4 gradient_at(const swf::FillStyle& fs, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (fs.stops.empty()) return fs.color;
    if (t <= fs.stops.front().first) return fs.stops.front().second;
    for (size_t i = 1; i < fs.stops.size(); ++i)
        if (t <= fs.stops[i].first) {
            const auto& [r0, c0] = fs.stops[i - 1];
            const auto& [r1, c1] = fs.stops[i];
            return glm::mix(c0, c1, r1 > r0 ? (t - r0) / (r1 - r0) : 0.0f);
        }
    return fs.stops.back().second;
}

}  // namespace

int cmd_swf(int argc, char** argv) {
    // argv: DATA_DIR MOVIE OUT [options/steps]
    Vfs vfs;
    vfs.mount_directory(argv[0]);
    swf::Library lib(vfs);
    auto movie = lib.movie(argv[1]);
    if (!movie) {
        std::fprintf(stderr, "no movie %s\n", argv[1]);
        return 1;
    }
    int w = int(movie->width()), h = int(movie->height());
    std::vector<std::string> steps;
    int frames = 1;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &w, &h);
        } else if (a == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else {
            steps.push_back(a);
        }
    }
    StringTable strings;
    strings.load(vfs, "MAIN", "en");
    int messages = 0;
    swf::Player player(movie, [&](const std::string& name) { return lib.movie(name); }, glm::vec2(float(w), float(h)),
                       [&](swf::Player& p) {
                           p.log = [&](const std::string& s) {
                               if (++messages <= 200) std::printf("%s\n", s.c_str());
                           };
                           p.vm.trace = p.log;
                           p.on_native = [&](const std::string& name, swf::CallArgs& a) {
                               if (name == "getString" || name == "NativeGetString") return swf::Value();
                               std::string args;
                               for (const swf::Value& v : a.args) args += (args.empty() ? "" : ", ") + p.vm.to_string(v);
                               std::printf("native %s(%s)\n", name.c_str(), args.c_str());
                               return swf::Value();
                           };
                           p.vm.get_string = [&](const std::string& id) {
                               const std::string t = strings.find(id);
                               return t.empty() ? id : t;
                           };
                       });
    for (int i = 0; i < frames; ++i) player.step_frame();
    for (const std::string& s : steps) {
        std::string error;
        if (!swf::run_step(player, s, &error)) std::fprintf(stderr, "%s\n", error.c_str());
    }
    swf::DrawList dl;
    player.render(dl);
    Canvas canvas(w, h, glm::vec4(0.12f, 0.14f, 0.18f, 1.0f));
    size_t tris = 0;
    for (const auto& b : dl.batches) {
        tris += b.vertices.size() / 3;
        const Image* img = b.texture.kind == swf::TextureRef::Bitmap ? lib.bitmap(*b.texture.movie, b.texture.bitmap) : nullptr;
        for (size_t i = 0; i + 2 < b.vertices.size(); i += 3) {
            canvas.triangle(&b.vertices[i], [&](glm::vec2 uv) -> glm::vec4 {
                switch (b.texture.kind) {
                    case swf::TextureRef::White: return glm::vec4(1.0f);
                    case swf::TextureRef::Gradient: {
                        const swf::FillStyle& fs = *b.texture.gradient;
                        const float t = fs.kind == swf::FillStyle::Linear ? uv.x : glm::length(uv - 0.5f) * 2.0f;
                        return gradient_at(fs, t);
                    }
                    case swf::TextureRef::Text: {
                        const auto& run = dl.texts[size_t(b.texture.text)];
                        const bool edge = uv.x < 0.02f || uv.x > 0.98f || uv.y < 0.04f || uv.y > 0.96f;
                        (void)run;
                        return glm::vec4(1.0f, 1.0f, 1.0f, edge ? 0.9f : 0.25f);
                    }
                    case swf::TextureRef::Bitmap: {
                        if (!img || !img->width) return glm::vec4(1, 0, 1, 1);
                        int x = int(std::floor(uv.x)), y = int(std::floor(uv.y));
                        x = std::clamp(x, 0, img->width - 1), y = std::clamp(y, 0, img->height - 1);
                        const uint8_t* q = &img->rgba[(size_t(y) * size_t(img->width) + size_t(x)) * 4];
                        return glm::vec4(q[0], q[1], q[2], q[3]) / 255.0f;
                    }
                }
                return glm::vec4(1.0f);
            });
        }
    }
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < canvas.px.size(); ++i) {
        const glm::vec4 c = glm::clamp(canvas.px[i], 0.0f, 1.0f) * 255.0f + 0.5f;
        out[i * 4] = uint8_t(c.r), out[i * 4 + 1] = uint8_t(c.g), out[i * 4 + 2] = uint8_t(c.b), out[i * 4 + 3] = 255;
    }
    write_png(argv[2], w, h, out);
    std::printf("%s: %dx%d, %zu batches, %zu triangles, %zu text runs\n", argv[2], w, h, dl.batches.size(), tris, dl.texts.size());
    for (const auto& t : dl.texts)
        if (!t.text.empty()) std::printf("  text %.0fx%.0f %s %.0fpx: %s\n", t.width, t.height, t.font.c_str(), t.size, t.text.substr(0, 60).c_str());
    return 0;
}
