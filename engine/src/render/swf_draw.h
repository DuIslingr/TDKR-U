// The Flash player's frame (swf::DrawList) as the renderer's screen sprites: atlas bitmaps and
// gradients become textures, device-font text runs are rasterised with SDL_ttf from the game's
// fonts (data/*.ttf).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "render/renderer.h"
#include "swf/library.h"
#include "swf/player.h"

struct TTF_Font;

namespace tdkr {

class SwfDrawer {
public:
    SwfDrawer(Renderer& renderer, swf::Library& library);
    ~SwfDrawer();
    SwfDrawer(const SwfDrawer&) = delete;
    SwfDrawer& operator=(const SwfDrawer&) = delete;

    void draw(const swf::DrawList& list, ParticleList& out);
    // A text run's size in pixels (for the player's autoSize / textWidth).
    glm::vec2 measure(const swf::TextRun& run);

private:
    TTF_Font* font(const std::string& name, float size);
    const GpuTexture* text_texture(const swf::TextRun& run);
    const GpuTexture* gradient(const swf::FillStyle* fs);

    Renderer& renderer_;
    swf::Library& library_;
    std::unordered_map<std::string, const GpuTexture*> bitmaps_;
    std::unordered_map<const swf::FillStyle*, const GpuTexture*> gradients_;
    std::unordered_map<std::string, const GpuTexture*> texts_;
    std::map<std::string, Bytes> font_data_;
    std::map<std::pair<std::string, int>, TTF_Font*> fonts_;
};

}  // namespace tdkr
