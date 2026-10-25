// TDKR's menus, played from the game's Flash movies the way CMenuManager and the natives the
// scripts call drive them:
//   title     MenuPressKey.swf; a key or click goes on to the main menu
//   main      MainMenu.swf, screenManager.startScreen("main"), over menu_bg_gotham.swf (the menu
//             level's background); Chapters is filled through CHAPTER_LIST_INIT
//   pause     MainMenu.swf, startScreen("ingame") (CHUDDisplay::EnterIGM), over the game
//   death     the same, startScreen("death")
// The scripts report presses with registerInGameEvent(NAME, ...): NEW_GAME, CONTINUE_GAME,
// MISSION_ACTIVATED, EXIT_GAME, BACK_TO_MAIN_MENU, BUTTON_RESTART_CHECKPOINT/LEVEL...; those
// become actions for the game.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/flash_screen.h"
#include "game/missions.h"

namespace tdkr {

class Vfs;
class StringTable;

class FrontEnd {
public:
    FrontEnd(const Vfs& vfs, Renderer& renderer, const StringTable* strings, const std::vector<Mission>* missions);
    ~FrontEnd();

    enum class Screen { None, Title, Main, Pause, Death };
    // False if the game's menu movies are missing (then nothing shows).
    bool show(Screen screen);
    void close();
    Screen screen() const { return screen_; }
    bool open() const { return screen_ != Screen::None; }

    struct Action {
        enum Kind { None, StartMission, Continue, Resume, RestartCheckpoint, RestartLevel, MainMenu, Quit, ResetProgress } kind = None;
        int mission = -1;
        bool new_game = false;   // StartMission: New Game (else a mission of Chapter Select)
    };
    // The action the last input led to (once).
    Action take_action();

    enum class Key { Up, Down, Left, Right, Activate, Back };
    void key(Key k);
    void pointer_move(float x, float y);
    void pointer_down(float x, float y);
    void pointer_up(float x, float y);

    void update(float dt, int width, int height);
    void draw(ParticleList& out);

    std::function<void(const std::string& sound)> play_sound;
    // The port's DISPLAY page in the Flash options (game/display_options.h): a DISPLAY row at
    // the end of Options opens it, a row per setting reading "NAME: VALUE"; a press steps the
    // setting and the rows are written again. The game supplies the rows and takes the presses.
    std::function<std::vector<std::string>()> display_rows;
    std::function<void(int row)> display_press;
    // Whether there is a save to continue (canStartNewGame is its opposite: New Game is offered
    // without one; with one Continue, and Options' Reset Progress), and whether a mission is
    // unlocked (Chapter Select's padlocks; a locked one does not start).
    std::function<bool()> has_save;
    std::function<bool(int mission)> mission_unlocked;
    // A native the menus call that the front end does not handle itself (logged otherwise).
    swf::Value native(const std::string& name, swf::CallArgs& args);
    swf::Player* player() { return menu_ ? menu_->player() : nullptr; }

private:
    void start_screen(const char* type);
    // The options screens' screenInit, wrapped: the DISPLAY row, or the DISPLAY page itself.
    void install_display_page();
    void relabel_display_page();
    bool display_next_ = false;   // the next options screen built is the DISPLAY page
    // The DISPLAY row last pressed (-1 none): a press replays the row's release animation, which
    // drops the focus, so the keys pick it up again from there.
    int display_row_ = -1;
    const void* focus_row(int row);   // the list's row `row` focused (its button clip), or null
    void fill_chapters(const std::string& path);
    void focus_move(glm::vec2 dir);

    const StringTable* strings_;
    const std::vector<Mission>* missions_;
    std::unique_ptr<FlashScreen> background_, menu_;
    const Vfs& vfs_;
    Renderer& renderer_;
    Screen screen_ = Screen::None;
    Action action_;
    int width_ = 1280, height_ = 720;
    swf::Instance* focus_ = nullptr;
    std::weak_ptr<swf::Object> focus_ref_;
    std::string pending_menu_;   // NativeSwitchMenu to another movie, done on the next update
    std::string pending_chapters_;   // CHAPTER_LIST_INIT's list, filled on the next update
};

}  // namespace tdkr
