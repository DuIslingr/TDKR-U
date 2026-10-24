// TDKR's storyboards (StartIntroStory(name, callback, ...): CHUDDisplay::StartStory, CStoryManager):
// the story of that name in StoryBoard.xml, a <frame texture=...> each with its duration (ms),
// its move (animType: none, pan_left, pan_right, pan_up, pan_down, zoom_in, zoom_out) and an
// optional caption (stringID), played full screen over StoryboardPlayer.swf; then the script's
// callback. The pictures fill the top 1024x768 of their 1024x1024 textures.
#pragma once

#include <string>
#include <vector>

#include "render/renderer.h"

namespace tdkr {

class Scene;
class Vfs;

class Storyboard {
public:
    struct Frame {
        std::string texture, caption;   // caption: a string id, or empty
        float seconds = 5;
        std::string move = "none";
    };
    // The story `name` from StoryBoard.xml; false if there is none (nothing to play).
    bool start(const Vfs& vfs, const std::string& name);
    bool active() const { return active_; }
    // Play on; true once, when the story has ended (or was skipped).
    bool update(float dt);
    void skip() { if (active_) time_ = 1e9f; }
    const std::string& caption() const;   // the frame's caption id
    // The frame over the whole screen (pixels), its move under way, black between frames.
    void draw(ParticleList& out, Scene& scene, const GpuTexture* white, int width, int height) const;

private:
    std::vector<Frame> frames_;
    size_t frame_ = 0;
    float time_ = 0;    // seconds into the frame
    bool active_ = false;
};

}  // namespace tdkr
