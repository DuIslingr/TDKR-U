#include "game/front_end.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "assets/strings.h"
#include "core/vfs.h"
#include "swf/steps.h"

namespace tdkr {

FrontEnd::FrontEnd(const Vfs& vfs, Renderer& renderer, const StringTable* strings, const std::vector<Mission>* missions)
    : strings_(strings), missions_(missions), vfs_(vfs), renderer_(renderer) {}

FrontEnd::~FrontEnd() = default;

bool FrontEnd::show(Screen screen) {
    focus_ = nullptr;
    focus_ref_.reset();
    pending_menu_.clear();
    pending_chapters_.clear();
    action_ = {};
    screen_ = Screen::None;
    background_.reset();
    menu_ = std::make_unique<FlashScreen>(vfs_, renderer_, strings_);
    menu_->on_native = [this](const std::string& name, swf::CallArgs& a) { return native(name, a); };
    const char* movie = screen == Screen::Title ? "MenuPressKey.swf" : "MainMenu.swf";
    if (screen == Screen::None || !menu_->load(movie, width_, height_)) {
        menu_.reset();
        return false;
    }
    if (screen == Screen::Main) {
        background_ = std::make_unique<FlashScreen>(vfs_, renderer_, strings_);
        if (!background_->load("menu_bg_gotham.swf", width_, height_)) background_.reset();
    }
    screen_ = screen;
    if (screen == Screen::Main) start_screen("main");
    else if (screen == Screen::Pause) start_screen("ingame");
    else if (screen == Screen::Death) start_screen("death");
    return true;
}

void FrontEnd::close() {
    screen_ = Screen::None;
    menu_.reset();
    background_.reset();
    focus_ = nullptr;
}

void FrontEnd::start_screen(const char* type) {
    if (!menu_ || !menu_->player()) return;
    menu_->update(0, width_, height_);
    menu_->player()->invoke("screenManager", "startScreen", {swf::Value(type)});
}

FrontEnd::Action FrontEnd::take_action() {
    const Action a = action_;
    action_ = {};
    return a;
}

swf::Value FrontEnd::native(const std::string& name, swf::CallArgs& a) {
    // (called while a movie builds too, before menu_ holds its player)
    auto str = [&](size_t k) {
        const swf::Value& v = a[k];
        if (v.type == swf::Value::String) return v.s;
        if (v.type == swf::Value::Number) return swf::VM::number_to_string(v.n);
        if (v.type == swf::Value::Boolean) return std::string(v.b ? "true" : "false");
        return std::string();
    };
    auto num = [&](size_t k) {
        const swf::Value& v = a[k];
        return v.type == swf::Value::Number ? v.n : v.type == swf::Value::String ? std::atof(v.s.c_str()) : -1.0;
    };
    if (name == "playSound") {
        if (play_sound) play_sound(str(0));
        return swf::Value(true);
    }
    if (name == "NativeSwitchMenu" || name == "NativePushMenu") {
        // Menus by movie name (CMenuManager::SwitchMenu); clips of this movie are its own business.
        const std::string target = str(0);
        if (target.size() > 4 && to_lower(target.substr(target.size() - 4)) == ".swf") pending_menu_ = target;
        return swf::Value(true);
    }
    if (name == "NativeStartLevel") {
        action_ = {Action::StartMission, int(num(0))};
        return swf::Value(true);
    }
    if (name != "registerInGameEvent" && name != "registerInGameEventExtended") return {};
    const std::string event = str(0), arg = str(1);
    if (event == "NEW_GAME") {
        action_ = {Action::StartMission, missions_ ? first_mission(*missions_) : -1};
    } else if (event == "CONTINUE_GAME" || event == "CONTINUE_FROM_WIN") {
        action_ = {Action::Continue};
    } else if (event == "MISSION_ACTIVATED") {
        action_ = {Action::StartMission, int(num(1))};
    } else if (event == "CHAPTER_LIST_INIT") {
        pending_chapters_ = arg;   // after the screen that asks has finished building
    } else if (event == "EXIT_GAME") {
        action_ = {Action::Quit};
    } else if (event == "BACK_TO_MAIN_MENU" || event == "BUTTON_RETURN_MENU") {
        action_ = {Action::MainMenu};
    } else if (event == "BUTTON_RESTART_CHECKPOINT") {
        action_ = {Action::RestartCheckpoint};
    } else if (event == "BUTTON_RESTART_LEVEL") {
        action_ = {Action::RestartLevel};
    } else if (event == "IGMMODE") {   // the pause menu's Continue
        if (screen_ == Screen::Pause) action_ = {Action::Resume};
    } else if (event == "BACK_FROM_ROOT" || event == "BUTTON_IGM") {
        if (screen_ == Screen::Pause) action_ = {Action::Resume};
    } else {
        std::fprintf(stderr, "menu event %s(%s)\n", event.c_str(), arg.c_str());
    }
    return swf::Value(true);
}

// CHUDDisplay::InitChapterSelection: init, the chapters (addToList), init again (a box each),
// then their missions (addMissionToChapter), all unlocked.
void FrontEnd::fill_chapters(const std::string& path) {
    swf::Player* p = menu_ ? menu_->player() : nullptr;
    if (!p || !missions_) return;
    std::string dotted;
    for (char c : path) dotted += c == '/' ? '.' : c;
    while (!dotted.empty() && dotted[0] == '.') dotted.erase(0, 1);
    auto text = [&](const std::string& id) {
        const std::string t = strings_ ? strings_->find(id) : std::string();
        return swf::Value(t.empty() ? id : t);
    };
    std::map<int, std::string> chapters;
    for (const Mission& m : *missions_)
        if (m.chapter > 0 && !chapters.count(m.chapter)) chapters[m.chapter] = m.chapter_title;
    p->invoke(dotted, "init");
    for (const auto& [c, title] : chapters) p->invoke(dotted, "addToList", {swf::Value(c), text(title)});
    p->invoke(dotted, "init");   // builds a box per chapter (populateSet); the missions go into them
    for (size_t i = 0; i < missions_->size(); ++i) {
        const Mission& m = (*missions_)[i];
        if (m.chapter <= 0) continue;
        p->invoke(dotted, "addMissionToChapter",
                  // (the chapter's box is 0-based)
                  {swf::Value(m.chapter - 1), swf::Value(m.number), swf::Value(true), swf::Value(int(i)), text(m.index_title),
                   text(m.selection), swf::Value(""), text(m.chapter_title), text(m.selection), text(m.description),
                   swf::Value(m.picture)});
    }
}

void FrontEnd::focus_move(glm::vec2 dir) {
    swf::Player* p = menu_ ? menu_->player() : nullptr;
    if (!p) return;
    const auto buttons = p->buttons();
    if (buttons.empty()) return;
    const swf::Player::Button* current = nullptr;
    if (focus_ && !focus_ref_.expired())
        for (const auto& b : buttons)
            if (b.clip == focus_) current = &b;
    const swf::Player::Button* best = nullptr;
    if (!current) {
        // the screen's list first (listButtons' bT_0, the first mission), else the top-left
        // button below the title bar (its corners hold back / info buttons)
        for (const auto& b : buttons) {
            const std::string path = b.clip->target_path();
            if (!best && (path.find(".bT_0.") != std::string::npos || path.find(".mission1.") != std::string::npos)) best = &b;
        }
        for (int pass = 0; pass < 2 && !best; ++pass)
            for (const auto& b : buttons) {
                if (pass == 0 && b.center.y < float(height_) * 0.15f) continue;
                if (!best || b.center.y + b.center.x * 0.2f < best->center.y + best->center.x * 0.2f) best = &b;
            }
    } else {
        float best_score = 1e30f;
        for (const auto& b : buttons) {
            if (&b == current) continue;
            const glm::vec2 d = b.center - current->center;
            const float along = glm::dot(d, dir);
            if (along <= 4.0f) continue;
            const float across = std::abs(d.x * dir.y - d.y * dir.x);
            const float score = along + across * 0.5f;   // menus are tilted lists: the next one is off to the side
            if (score < best_score) best_score = score, best = &b;
        }
        if (!best) return;
    }
    focus_ = best->clip;
    focus_ref_ = best->clip->shared_from_this();
    p->hover(focus_);
}

void FrontEnd::key(Key k) {
    if (screen_ == Screen::Title) {
        if (k == Key::Activate || k == Key::Back) show(Screen::Main);
        return;
    }
    swf::Player* p = menu_ ? menu_->player() : nullptr;
    if (!p) return;
    switch (k) {
        case Key::Up: focus_move({0, -1}); break;
        case Key::Down: focus_move({0, 1}); break;
        case Key::Left: focus_move({-1, 0}); break;
        case Key::Right: focus_move({1, 0}); break;
        case Key::Activate:
            if (!focus_ || focus_ref_.expired()) focus_move({0, 1});
            else p->press(focus_);
            break;
        case Key::Back: {
            // screenManager.goBack(): BACK_FROM_ROOT at the root (resumes the pause menu)
            const swf::Value r = p->get_member("screenManager", "goBack");
            if (r.function()) p->invoke("screenManager", "goBack");
            else if (screen_ == Screen::Pause) action_ = {Action::Resume};
            focus_ = nullptr;
            break;
        }
    }
}

void FrontEnd::pointer_move(float x, float y) {
    if (menu_ && menu_->player()) menu_->player()->pointer_move(x, y);
}

void FrontEnd::pointer_down(float x, float y) {
    if (screen_ == Screen::Title) return;
    if (menu_ && menu_->player()) menu_->player()->pointer_down(x, y);
}

void FrontEnd::pointer_up(float x, float y) {
    if (screen_ == Screen::Title) {
        show(Screen::Main);
        return;
    }
    if (menu_ && menu_->player()) menu_->player()->pointer_up(x, y);
}

void FrontEnd::update(float dt, int width, int height) {
    width_ = width, height_ = height;
    if (!pending_menu_.empty() && menu_) {
        const std::string movie = pending_menu_;
        pending_menu_.clear();
        std::fprintf(stderr, "menu: switch to %s\n", movie.c_str());
        auto next = std::make_unique<FlashScreen>(vfs_, renderer_, strings_);
        next->on_native = [this](const std::string& name, swf::CallArgs& a) { return native(name, a); };
        if (next->load(movie, width, height)) menu_ = std::move(next), focus_ = nullptr;
    }
    if (!pending_chapters_.empty()) {
        const std::string path = pending_chapters_;
        pending_chapters_.clear();
        fill_chapters(path);
    }
    if (background_) background_->update(dt, width, height);
    if (menu_) menu_->update(dt, width, height);
}

void FrontEnd::draw(ParticleList& out) {
    if (background_) background_->draw(out);
    if (menu_) menu_->draw(out);
}

}  // namespace tdkr
