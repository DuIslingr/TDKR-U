#include "game/storyboard.h"

#include <algorithm>
#include <cstdlib>

#include "core/vfs.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr float kFade = 0.5f;            // seconds through black at each end of a frame
constexpr float kPictureV = 768.0f / 1024.0f;   // the picture's share of the texture's height
constexpr float kMoveZoom = 0.15f;       // how far a zoom goes; a pan travels within this margin

std::string attribute(const std::string& tag, const std::string& name) {
    const size_t at = tag.find(name + "=\"");
    if (at == std::string::npos) return "";
    const size_t start = at + name.size() + 2, end = tag.find('"', start);
    return end == std::string::npos ? "" : tag.substr(start, end - start);
}

}  // namespace

bool Storyboard::start(const Vfs& vfs, const std::string& name) {
    active_ = false;
    frames_.clear();
    if (!vfs.exists("StoryBoard.xml")) return false;
    const Bytes raw = vfs.read("StoryBoard.xml");
    std::string xml(raw.begin(), raw.end());
    for (size_t c; (c = xml.find("<!--")) != std::string::npos;) {   // comments out
        const size_t e = xml.find("-->", c);
        xml.erase(c, e == std::string::npos ? std::string::npos : e + 3 - c);
    }
    const size_t story = xml.find("<story name=\"" + name + "\"");
    if (story == std::string::npos) return false;
    const size_t end = std::min(xml.find("</story>", story), xml.size());
    for (size_t at = xml.find("<frame", story); at < end; at = xml.find("<frame", at + 1)) {
        const size_t close = std::min(xml.find("</frame>", at), end);
        const std::string body = xml.substr(at, close - at);
        Frame f;
        f.texture = attribute(body.substr(0, body.find('>')), "texture");
        for (size_t p = body.find("<p "); p != std::string::npos; p = body.find("<p ", p + 1)) {
            const std::string tag = body.substr(p, body.find('>', p) - p);
            const std::string key = attribute(tag, "key"), val = attribute(tag, "val");
            if (key == "duration") f.seconds = std::max(float(std::atof(val.c_str())) / 1000.0f, 0.5f);
            else if (key == "animType") f.move = val;
            else if (key == "stringID") f.caption = val;
        }
        if (!f.texture.empty()) frames_.push_back(f);
    }
    frame_ = 0, time_ = 0;
    active_ = !frames_.empty();
    return active_;
}

bool Storyboard::update(float dt) {
    if (!active_) return false;
    time_ += dt;
    while (frame_ < frames_.size() && time_ >= frames_[frame_].seconds) time_ -= frames_[frame_].seconds, ++frame_;
    if (frame_ < frames_.size() && time_ < 1e8f) return false;
    active_ = false;
    return true;
}

const std::string& Storyboard::caption() const {
    static const std::string none;
    return active_ && frame_ < frames_.size() ? frames_[frame_].caption : none;
}

void Storyboard::draw(ParticleList& out, Scene& scene, const GpuTexture* white, int width, int height) const {
    if (!active_ || frame_ >= frames_.size()) return;
    const float w = float(width), h = float(height);
    auto quad = [&](const GpuTexture* tex, glm::vec2 a, glm::vec2 b, glm::vec2 uv0, glm::vec2 uv1, uint32_t rgba) {
        const uint32_t first = uint32_t(out.vertices.size());
        const ParticleVertex v[4] = {{{a.x, a.y, 0}, uv0, rgba}, {{b.x, a.y, 0}, {uv1.x, uv0.y}, rgba},
                                     {{b.x, b.y, 0}, uv1, rgba}, {{a.x, b.y, 0}, {uv0.x, uv1.y}, rgba}};
        for (int i : {0, 1, 2, 0, 2, 3}) out.vertices.push_back(v[i]);
        out.batches.push_back({tex, false, first, 6});
    };
    quad(white, {0, 0}, {w, h}, {0, 0}, {1, 1}, pack_rgba({0, 0, 0, 1}));
    const Frame& f = frames_[frame_];
    const GpuTexture* tex = scene.texture(f.texture);
    if (!tex) return;
    // The 4:3 picture covering the screen, then its move: k runs 0 -> 1 over the frame.
    const float k = std::clamp(time_ / f.seconds, 0.0f, 1.0f);
    float zoom = 1 + kMoveZoom;
    glm::vec2 shift(0);   // in units of the margin, -1 .. 1
    if (f.move == "zoom_in") zoom = 1 + kMoveZoom * k;
    else if (f.move == "zoom_out") zoom = 1 + kMoveZoom * (1 - k);
    else if (f.move == "pan_left") shift.x = 1 - 2 * k;
    else if (f.move == "pan_right") shift.x = 2 * k - 1;
    else if (f.move == "pan_up") shift.y = 1 - 2 * k;
    else if (f.move == "pan_down") shift.y = 2 * k - 1;
    else zoom = 1;
    const float cover = std::max(w / 4.0f, h / 3.0f) * zoom;   // pixels per picture unit (4 x 3)
    const glm::vec2 size(4 * cover, 3 * cover);
    const glm::vec2 margin = (size - glm::vec2(w, h)) * 0.5f;
    const glm::vec2 a = glm::vec2(w, h) * 0.5f - size * 0.5f - margin * shift;
    // Faded in and out through black.
    const float alpha = std::clamp(std::min(time_, f.seconds - time_) / kFade, 0.0f, 1.0f);
    quad(tex, a, a + size, {0, 0}, {1, kPictureV}, pack_rgba({1, 1, 1, alpha}));
}

}  // namespace tdkr
