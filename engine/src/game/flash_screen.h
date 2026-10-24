// One of the game's Flash movies on screen (menus, HUD): the movie from the archives played
// by the Flash player, its text from the string tables, drawn as the frame's screen sprites.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "render/renderer.h"
#include "swf/library.h"
#include "swf/player.h"

namespace tdkr {

class Vfs;
class SwfDrawer;
class StringTable;

class FlashScreen {
public:
    FlashScreen(const Vfs& vfs, Renderer& renderer, const StringTable* strings);
    ~FlashScreen();

    // Load a movie (e.g. "MainMenu.swf") for a screen of this size; false if it is missing.
    bool load(const std::string& movie, int width, int height);
    bool loaded() const { return player_ != nullptr; }
    swf::Player* player() { return player_.get(); }
    swf::Library& library() { return *library_; }

    // The game's natives the scripts call (NativeSwitchMenu, registerInGameEvent, ...).
    std::function<swf::Value(const std::string& name, swf::CallArgs& args)> on_native;

    // Size the movie to the screen (pixels) and run it for `dt` seconds.
    void update(float dt, int width, int height);
    void draw(ParticleList& out);
    bool resized() const { return resized_; }   // the last update changed the viewport

private:
    const StringTable* strings_;
    std::unique_ptr<swf::Library> library_;
    std::unique_ptr<SwfDrawer> drawer_;
    std::unique_ptr<swf::Player> player_;
    swf::DrawList list_;
    int width_ = 0, height_ = 0;
    bool resized_ = false;
};

}  // namespace tdkr
