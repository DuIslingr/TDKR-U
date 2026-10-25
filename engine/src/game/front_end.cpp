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
    focus_ = nullptr, display_row_ = -1;
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
    // (the main menu's backdrop is the menu level, drawn by the game behind it)
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
    focus_ = nullptr, display_row_ = -1;
}

void FrontEnd::start_screen(const char* type) {
    if (!menu_ || !menu_->player()) return;
    menu_->update(0, width_, height_);
    install_display_page();
    menu_->player()->invoke("screenManager", "startScreen", {swf::Value(type)});
}

// MainMenu.swf's Options (scr.screen.options, screenOptions; optionsIGM in the pause menu)
// builds its list in screenInit: bL.addButton(label, screen, action), then bL.animate(0). The
// wrapper adds a DISPLAY row before the list animates in (its action reaches the game through
// registerInGameEvent(action, "RELEASE"), as the original's actions do). The DISPLAY page is
// the other options screen (screenOptions from the pause menu, screenOptionsIGM from the main
// menu) built with the display rows instead; its Back returns to Options.
void FrontEnd::install_display_page() {
    swf::Player* p = menu_ ? menu_->player() : nullptr;
    if (!p || !display_rows) return;
    swf::VM& vm = p->vm;
    const swf::Value proto = vm.get_member(vm.get_member(vm.get_member(vm.get_member(swf::Value(vm.global), "scr"), "screen"), "options"), "prototype");
    const swf::Value original = vm.get_member(proto, "screenInit");
    if (!proto.is_object() || !original.function() || vm.get_member(proto, "tdkrDisplay").type == swf::Value::Boolean) return;
    vm.set_member(proto, "tdkrDisplay", swf::Value(true));
    vm.set_member(proto, "screenInit", swf::Value(vm.native([this, original](swf::VM& vm, swf::CallArgs& a) -> swf::Value {
        const swf::Value self = a.self, list = vm.get_member(self, "bL");
        display_row_ = -1, focus_ = nullptr;   // (the screen before may still be up, animating out)
        if (display_next_) {   // the DISPLAY page: its title, a row per setting
            display_next_ = false;
            vm.set_member(self, "idx", swf::Value(0));
            vm.set_member(vm.get_member(vm.get_member(self, "headerText"), "txt"), "text", swf::Value("DISPLAY"));
            vm.set_member(list, "horizontal", swf::Value(false));
            const std::vector<std::string> rows = display_rows();
            vm.set_member(list, "size", swf::Value(rows.size() > 5 ? 110 : 140));   // (Options' rows are at 160)
            for (size_t i = 0; i < rows.size(); ++i)
                vm.call_method(list, "addButton", {swf::Value(rows[i]), swf::Value(), swf::Value("TDKR_DISPLAY_" + std::to_string(i))});
            vm.call_method(list, "animate", {swf::Value(0)});
            return {};
        }
        // Options: the original's rows, then DISPLAY (added as the list starts to animate).
        const swf::Value animate = vm.get_member(list, "animate");
        if (list.is_object() && animate.function()) {
            list.object()->set_own("animate", swf::Value(vm.native([list, animate](swf::VM& vm, swf::CallArgs& b) -> swf::Value {
                list.object()->remove_own("animate");
                vm.call_method(list, "addButton", {swf::Value("Display"), swf::Value(), swf::Value("TDKR_DISPLAY")});
                return vm.call(animate, list, b.args);
            })));
        }
        return vm.call(original, self, a.args);
    })));
}

// After a press on the DISPLAY page: every row's text again (listButton.writeText).
void FrontEnd::relabel_display_page() {
    swf::Player* p = menu_ ? menu_->player() : nullptr;
    if (!p || !display_rows) return;
    swf::VM& vm = p->vm;
    const swf::Value screen = vm.get_member(vm.get_member(swf::Value(vm.global), "screenManager"), "currentScreen");
    const swf::Value buttons = vm.get_member(vm.get_member(screen, "bL"), "buttons");
    swf::ArrayObject* list = buttons.object() ? buttons.object()->as_array() : nullptr;
    if (!list) return;
    const std::vector<std::string> rows = display_rows();
    for (size_t i = 0; i < rows.size() && i < list->items.size(); ++i) {
        vm.set_member(list->items[i], "t", swf::Value(rows[i]));
        vm.call_method(list->items[i], "writeText", {swf::Value(rows[i]), swf::Value(1)});
    }
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
    // (no save yet: the main menu offers New Game; else Continue, and Options Reset Progress)
    if (name == "canStartNewGame") return swf::Value(has_save ? !has_save() : true);
    if (name != "registerInGameEvent" && name != "registerInGameEventExtended") return {};
    const std::string event = str(0), arg = str(1);
    if (event == "NEW_GAME") {
        action_ = {Action::StartMission, missions_ ? first_mission(*missions_) : -1, true};
    } else if (event == "CONTINUE_GAME" || event == "CONTINUE_FROM_WIN") {
        action_ = {Action::Continue};
    } else if (event == "MISSION_ACTIVATED") {   // (GS_BaseMenu::StartChapter: not a locked one)
        const int m = int(num(1));
        if (!mission_unlocked || mission_unlocked(m)) action_ = {Action::StartMission, m};
        else std::fprintf(stderr, "menu: mission %d is locked\n", m);
    } else if (event == "RESET_PROGRESS") {   // Application::ResetAllProgress, then Options again
        action_ = {Action::ResetProgress};
        if (swf::Player* p = menu_ ? menu_->player() : nullptr) p->invoke("screenManager", "jumpTo", {swf::Value("screenOptions")});
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
    } else if (event == "TDKR_DISPLAY") {   // Options' DISPLAY row (the action, then "RELEASE"): the page
        display_next_ = true;
        swf::Player* p = menu_ ? menu_->player() : nullptr;
        if (p) p->invoke("screenManager", "jumpTo", {swf::Value(screen_ == Screen::Pause ? "screenOptions" : "screenOptionsIGM")});
    } else if (event.rfind("TDKR_DISPLAY_", 0) == 0) {   // a row of the DISPLAY page
        display_row_ = std::atoi(event.c_str() + 13);
        if (display_press) display_press(display_row_);
        relabel_display_page();
    } else {
        std::fprintf(stderr, "menu event %s(%s)\n", event.c_str(), arg.c_str());
    }
    return swf::Value(true);
}

// CHUDDisplay::InitChapterSelection: init, the chapters (addToList), init again (a box each),
// then their missions (addMissionToChapter), open as the story has unlocked them.
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
                  {swf::Value(m.chapter - 1), swf::Value(m.number), swf::Value(!mission_unlocked || mission_unlocked(int(i))),
                   swf::Value(int(i)), text(m.index_title),
                   text(m.selection), swf::Value(""), text(m.chapter_title), text(m.selection), text(m.description),
                   swf::Value(m.picture)});
    }
}

const void* FrontEnd::focus_row(int row) {
    swf::Player* p = menu_ ? menu_->player() : nullptr;
    if (!p || row < 0) return nullptr;
    const std::string name = ".bT_" + std::to_string(row) + ".";
    for (const auto& b : p->buttons())
        if (b.clip->target_path().find(name) != std::string::npos) {
            focus_ = b.clip;
            focus_ref_ = b.clip->shared_from_this();
            p->hover(focus_);
            return focus_;
        }
    return nullptr;
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
    if (!current && focus_row(display_row_))
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
        // Up and down a list (listButtons' bT_N): the next row by its number, wherever its
        // label has put its hit zone.
        const std::string path = current->clip->target_path();
        const size_t at = path.rfind(".bT_");
        if (dir.x == 0 && at != std::string::npos) {
            const size_t end = path.find('.', at + 4);
            const int n = std::atoi(path.c_str() + at + 4) + (dir.y > 0 ? 1 : -1);
            const std::string next = path.substr(0, at) + ".bT_" + std::to_string(n) + (end == std::string::npos ? "" : path.substr(end));
            for (const auto& b : buttons)
                if (n >= 0 && b.clip->target_path() == next && (b.center.y - current->center.y) * dir.y > 4.0f) best = &b;   // (a vertical list)
        }
    }
    if (current && !best) {
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
            if ((!focus_ || focus_ref_.expired()) && !focus_row(display_row_)) focus_move({0, 1});
            else p->press(focus_);
            break;
        case Key::Back: {
            // screenManager.goBack(): BACK_FROM_ROOT at the root (resumes the pause menu)
            const swf::Value r = p->get_member("screenManager", "goBack");
            if (r.function()) p->invoke("screenManager", "goBack");
            else if (screen_ == Screen::Pause) action_ = {Action::Resume};
            focus_ = nullptr, display_row_ = -1;
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
