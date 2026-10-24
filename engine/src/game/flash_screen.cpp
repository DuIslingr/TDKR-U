#include "game/flash_screen.h"

#include <cstdio>

#include "assets/strings.h"
#include "core/vfs.h"
#include "render/swf_draw.h"

namespace tdkr {

FlashScreen::FlashScreen(const Vfs& vfs, Renderer& renderer, const StringTable* strings)
    : strings_(strings), library_(std::make_unique<swf::Library>(vfs)) {
    drawer_ = std::make_unique<SwfDrawer>(renderer, *library_);
}

FlashScreen::~FlashScreen() = default;

bool FlashScreen::load(const std::string& movie, int width, int height) {
    auto def = library_->movie(movie);
    if (!def) {
        std::fprintf(stderr, "swf: no %s\n", movie.c_str());
        player_.reset();
        return false;
    }
    const std::string tag = def->name;
    auto setup = [this, tag](swf::Player& p) {
        p.log = [tag](const std::string& s) { std::fprintf(stderr, "%s: %s\n", tag.c_str(), s.c_str()); };
        p.vm.trace = [](const std::string&) {};   // the menus trace a lot
        p.vm.get_string = [this](const std::string& id) {
            const std::string t = strings_ ? strings_->find(id) : std::string();
            return t.empty() ? id : t;
        };
        p.measure_text = [this](const swf::TextRun& run) { return drawer_->measure(run); };
        p.on_native = [this](const std::string& name, swf::CallArgs& a) { return on_native ? on_native(name, a) : swf::Value(); };
    };
    player_ = std::make_unique<swf::Player>(def, [this](const std::string& name) { return library_->movie(name); },
                                            glm::vec2(float(width), float(height)), setup);
    width_ = width, height_ = height;
    return true;
}

void FlashScreen::update(float dt, int width, int height) {
    resized_ = false;
    if (!player_) return;
    if (width != width_ || height != height_) {
        width_ = width, height_ = height;
        player_->set_viewport(float(width), float(height));
        resized_ = true;
    }
    player_->advance(dt);
}

void FlashScreen::draw(ParticleList& out) {
    if (!player_) return;
    player_->render(list_);
    drawer_->draw(list_, out);
}

}  // namespace tdkr
