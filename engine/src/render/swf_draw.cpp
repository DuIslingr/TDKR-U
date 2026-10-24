#include "render/swf_draw.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/vfs.h"

namespace tdkr {

namespace {

// SWF font names -> the game's font files. Everything else falls back to the HUD's font.
std::string font_file(const std::string& name) {
    const std::string n = to_lower(name);
    if (n.find("rodin") != std::string::npos) return "RodinNTLGProDB.ttf";
    if (n.find("nanum") != std::string::npos) return "NanumGothicBold.ttf";
    if (n.find("wenquan") != std::string::npos) return "WenQuanYi.ttf";
    if (n.find("pfennig") != std::string::npos) return "Pfennig.ttf";
    return "AccidentalPresidency.ttf";
}

glm::vec4 gradient_color(const swf::FillStyle& fs, float t) {
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

SwfDrawer::SwfDrawer(Renderer& renderer, swf::Library& library) : renderer_(renderer), library_(library) {
    if (!TTF_WasInit() && !TTF_Init()) std::fprintf(stderr, "swf: TTF_Init: %s\n", SDL_GetError());
}

SwfDrawer::~SwfDrawer() {
    for (auto& [k, f] : fonts_)
        if (f) TTF_CloseFont(f);
}

TTF_Font* SwfDrawer::font(const std::string& name, float size) {
    const std::string file = font_file(name);
    const int px = std::max(1, int(std::lround(size)));
    const auto key = std::make_pair(file, px);
    if (auto it = fonts_.find(key); it != fonts_.end()) return it->second;
    TTF_Font* f = nullptr;
    if (library_.vfs().exists(file)) {
        Bytes& data = font_data_[file];
        if (data.empty()) data = library_.vfs().read(file);
        f = TTF_OpenFontIO(SDL_IOFromConstMem(data.data(), data.size()), true, float(px));
    }
    return fonts_[key] = f;
}

glm::vec2 SwfDrawer::measure(const swf::TextRun& run) {
    TTF_Font* f = font(run.font, run.size);
    if (!f || run.text.empty()) return glm::vec2(0);
    int w = 0, h = 0;
    if (run.wrap && run.width > 0) {
        if (SDL_Surface* s = TTF_RenderText_Blended_Wrapped(f, run.text.c_str(), 0, SDL_Color{255, 255, 255, 255}, int(run.width))) {
            w = s->w, h = s->h;
            SDL_DestroySurface(s);
        }
    } else {
        int lines = 0;
        size_t p = 0;
        for (;;) {
            const size_t q = run.text.find('\n', p);
            const std::string line = run.text.substr(p, q == std::string::npos ? std::string::npos : q - p);
            int lw = 0, lh = 0;
            TTF_GetStringSize(f, line.c_str(), 0, &lw, &lh);
            w = std::max(w, lw);
            ++lines;
            if (q == std::string::npos) break;
            p = q + 1;
        }
        h = lines * TTF_GetFontLineSkip(f);
    }
    return glm::vec2(float(w), float(h));
}

const GpuTexture* SwfDrawer::text_texture(const swf::TextRun& run) {
    const int bw = std::max(1, int(std::ceil(run.width))), bh = std::max(1, int(std::ceil(run.height)));
    char head[96];
    std::snprintf(head, sizeof head, "%s|%.1f|%d|%dx%d|%d|", run.font.c_str(), run.size, run.align, bw, bh, int(run.wrap));
    const std::string key = head + run.text;
    if (auto it = texts_.find(key); it != texts_.end()) return it->second;
    TTF_Font* f = font(run.font, run.size);
    const GpuTexture* tex = nullptr;
    if (f && bw <= 4096 && bh <= 4096) {
        Image img;
        img.width = bw, img.height = bh;
        img.rgba.assign(size_t(bw) * size_t(bh) * 4, 0);
        // White text (the vertex colour tints it), each line placed by the run's alignment.
        auto blit = [&](SDL_Surface* s, int x0, int y0) {
            SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
            if (!rgba) return;
            for (int y = 0; y < rgba->h; ++y) {
                const int dy = y0 + y;
                if (dy < 0 || dy >= bh) continue;
                for (int x = 0; x < rgba->w; ++x) {
                    const int dx = x0 + x;
                    if (dx < 0 || dx >= bw) continue;
                    const uint8_t* sp = static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch) + size_t(x) * 4;
                    uint8_t* dp = &img.rgba[(size_t(dy) * size_t(bw) + size_t(dx)) * 4];
                    dp[0] = dp[1] = dp[2] = 255;
                    dp[3] = std::max(dp[3], sp[3]);
                }
            }
            SDL_DestroySurface(rgba);
        };
        TTF_SetFontWrapAlignment(f, run.align == 1 ? TTF_HORIZONTAL_ALIGN_RIGHT : run.align == 2 ? TTF_HORIZONTAL_ALIGN_CENTER : TTF_HORIZONTAL_ALIGN_LEFT);
        if (run.wrap) {
            if (SDL_Surface* s = TTF_RenderText_Blended_Wrapped(f, run.text.c_str(), 0, SDL_Color{255, 255, 255, 255}, bw)) {
                const int x0 = run.align == 1 ? bw - s->w : run.align == 2 ? (bw - s->w) / 2 : 0;
                blit(s, x0, 0);
                SDL_DestroySurface(s);
            }
        } else {
            int y = 0;
            size_t p = 0;
            const int skip = TTF_GetFontLineSkip(f);
            for (;;) {
                const size_t q = run.text.find('\n', p);
                const std::string line = run.text.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (!line.empty())
                    if (SDL_Surface* s = TTF_RenderText_Blended(f, line.c_str(), 0, SDL_Color{255, 255, 255, 255})) {
                        const int x0 = run.align == 1 ? bw - s->w : run.align == 2 ? (bw - s->w) / 2 : 0;
                        blit(s, x0, y);
                        SDL_DestroySurface(s);
                    }
                y += skip;
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        tex = renderer_.upload_texture(img);
    }
    return texts_[key] = tex;
}

const GpuTexture* SwfDrawer::gradient(const swf::FillStyle* fs) {
    if (auto it = gradients_.find(fs); it != gradients_.end()) return it->second;
    Image img;
    const bool linear = fs->kind == swf::FillStyle::Linear;
    img.width = linear ? 256 : 64;
    img.height = linear ? 1 : 64;
    img.rgba.resize(size_t(img.width) * size_t(img.height) * 4);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const glm::vec2 uv((float(x) + 0.5f) / float(img.width), (float(y) + 0.5f) / float(img.height));
            const float t = linear ? uv.x : glm::length(uv - 0.5f) * 2.0f;
            const glm::vec4 c = glm::clamp(gradient_color(*fs, t), 0.0f, 1.0f) * 255.0f + 0.5f;
            uint8_t* p = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
            p[0] = uint8_t(c.r), p[1] = uint8_t(c.g), p[2] = uint8_t(c.b), p[3] = uint8_t(c.a);
        }
    return gradients_[fs] = renderer_.upload_texture(img);
}

void SwfDrawer::draw(const swf::DrawList& list, ParticleList& out) {
    for (const swf::DrawBatch& b : list.batches) {
        if (b.vertices.empty()) continue;
        const GpuTexture* tex = renderer_.white();
        glm::vec2 uv_scale(1.0f);
        switch (b.texture.kind) {
            case swf::TextureRef::White: break;
            case swf::TextureRef::Gradient: tex = gradient(b.texture.gradient); break;
            case swf::TextureRef::Text: tex = text_texture(list.texts[size_t(b.texture.text)]); break;
            case swf::TextureRef::Bitmap: {
                const std::string key = b.texture.movie->name + "#" + std::to_string(b.texture.bitmap) + "@" +
                                        std::to_string(reinterpret_cast<uintptr_t>(b.texture.movie));
                auto it = bitmaps_.find(key);
                if (it == bitmaps_.end()) {
                    const Image* img = library_.bitmap(*b.texture.movie, b.texture.bitmap);
                    it = bitmaps_.emplace(key, img && img->width ? renderer_.upload_texture(*img) : nullptr).first;
                }
                tex = it->second;
                if (tex) uv_scale = glm::vec2(1.0f / float(tex->width), 1.0f / float(tex->height));
                break;
            }
        }
        if (!tex) continue;
        const uint32_t first = uint32_t(out.vertices.size());
        for (const swf::DrawVertex& v : b.vertices)
            out.vertices.push_back({{v.pos.x, v.pos.y, 0.0f}, v.uv * uv_scale, pack_rgba(v.color)});
        const uint32_t count = uint32_t(out.vertices.size()) - first;
        if (!out.batches.empty() && out.batches.back().texture == tex && !out.batches.back().additive &&
            out.batches.back().first + out.batches.back().count == first)
            out.batches.back().count += count;
        else
            out.batches.push_back({tex, false, first, count});
    }
}

}  // namespace tdkr
