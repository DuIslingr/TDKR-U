// TDKR's in-game HUD: BatmanHUD.swf played by the Flash player and set up the way
// CHUDDisplay::Init / SetBatmanInterface / SetMinimapInterface leave it on foot (action
// buttons, minimap with the level's <level>_minimap.swf loaded into it, pause button), with
// the grapnel target icon over the edge the grapnel would take.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "game/flash_screen.h"

namespace tdkr {

class Vfs;
class StringTable;

class FlashHud {
public:
    FlashHud(const Vfs& vfs, Renderer& renderer, const StringTable* strings);
    ~FlashHud();

    // Load the HUD for a level with its minimap (level properties: the movie, pixels per metre,
    // zoom). False if BatmanHUD.swf is missing.
    bool start(const std::string& minimap, float minimap_scale, float minimap_zoom);
    bool active() const { return screen_.loaded(); }

    struct State {
        bool visible = true;
        bool cinematic = false;
        bool grapple = false;            // a grapnel edge in reach: the target icon at `grapple_at`
        glm::vec2 grapple_at{0};         // screen pixels
        float health = 1;                // 0..1
        glm::vec2 position{0};           // Batman, world metres (the minimap follows him)
        float camera_yaw = 0, facing = 0;   // degrees about Z (the map turns with the camera)
        enum class Move { Ground, Air, Glide, Plunge } move = Move::Ground;   // the jump slot's style
        const char* attack = "STRIKE";   // the attack slot's style (STRIKE, TAKEDOWN, COUNTERATTACK)
        bool counter = false;            // the counter button is up (an attack can be countered)
        bool jump_on = true, attack_on = true, grapnel_on = true;   // what the scripts leave enabled
        bool riding = false;             // on the Batpod: its buttons instead of Batman's
        std::string dialog;              // the line being said (ShowMessage): subtitles in a cinematic
        // Objective markers (ObjectiveAdd): where the object shows on screen (pixels), or the
        // screen edge towards it with the arrow's angle (degrees) when it is off screen.
        struct Marker {
            int id = 0;
            std::string kind;            // talk, kill, follow, objective, sideObjective, ...
            glm::vec2 at{0};
            bool on_screen = true;
            float angle = 0, distance = 0;
        };
        std::vector<Marker> markers;
        std::string frame = "HUD";       // SetHudState: the movie's frame (HUD, Binoculars, ...)
        // StartPipeGame: the hacking game to start in the Hacking frame (a new serial: a new game).
        struct Hack {
            int serial = 0;
            std::string grid;
            int size = 0, start_row = 0, end_row = 0, ms = 0, price = 0;
        } hack;
        // QTE_EnableControl: the QTE button over an object (screen pixels).
        bool qte = false;
        glm::vec2 qte_at{0};
        float enemy_health = -1;         // AddHealthBar (baneHealthBar): 0..1, -1 none
        float progress = -1;             // a held use (disarmInstance's bar): 0..1, -1 none
    };
    void update(float dt, const State& state, int width, int height);
    // The objective popup (objectiveMessage_popin): show with text, or hide.
    void objective(bool shown, const std::string& text);
    void run(float seconds);   // play the movie on (headless screenshots)
    // CHUDDisplay::SetWinScreen: the mission's end (the HUD's WinScreen frame: its title, the XP
    // and WayneTech won, the time taken) until its button (CONTINUE_FROM_WIN) or close_win().
    void win_screen(const std::string& title, int xp, int coins, float seconds);
    // The checkpoint notice (CheckpointMessageInstance: its text, then its Anim).
    void checkpoint_message(const std::string& text);
    bool win_open() const { return win_open_; }
    // The hacking game (minigame.swf in mHackArea): it takes the pointer while it is up; its
    // outcome once (registerInGameEvent GAME_PIPE WIN / LOSE; its skip button counts as a win):
    // 0 none yet, 1 won, 2 lost.
    bool hacking() const { return frame_ == "Hacking" && hack_serial_ > 0; }
    int take_hack_result() { return std::exchange(hack_result_, 0); }
    void close_win();
    void press_continue();   // the win screen's button, from the keyboard or a pad
    void draw(ParticleList& out);
    swf::Player* player() { return screen_.player(); }

private:
    void set_up();
    void hide_unused();
    // CHUDDisplay::StartCinematic / EndCinematic: the letterbox bars and subtitles instead of
    // the controls (mCinematicHUD.startCinematic hides the rest of the HUD).
    void cinematic(bool on);
    // CHUDDisplay::AddDialogLine / AddCinematicLine: the Dialog box (setTextDialog,
    // openDialog / closeDialog), or the cinematic subtitle.
    void dialog(const std::string& text);
    bool cinematic_ = false;
    bool win_open_ = false;
    // CHUDDisplay::SetHudState: another of the movie's frames instead of the HUD (the binoculars'
    // visor); back on "HUD" the HUD is set up again.
    void set_frame(const std::string& frame);
    std::string frame_ = "HUD";
    // CHUDDisplay::AddObjectiveToScene / UpdateObjectivesPosition: one copy of the
    // ObjectiveBtn icon (hudAS.mNPCIcon) per marker, on the frame of its kind.
    void update_markers(const std::vector<State::Marker>& markers);
    std::map<int, std::string> marker_frames_;   // marker id -> the frame its icon shows
    std::string dialog_;
    // CHUDDisplay::SetButtonStyle: the slot's button switches style and (dis)ables, once per change.
    void button(const std::string& slot, const std::string& style, bool enabled);
    std::map<std::string, std::pair<std::string, int>> buttons_;   // slot -> (style, enabled)
    FlashScreen screen_;
    State state_;
    int width_ = 0, height_ = 0;
    std::string minimap_;
    float minimap_scale_ = 1, minimap_zoom_ = 1;
    glm::vec2 map_center_{0}, map_zero_{0};   // twips
    bool grapple_shown_ = false;
    int hack_serial_ = 0, hack_result_ = 0;
    void start_hack(const State::Hack& h);
    bool qte_shown_ = false;
    bool riding_ = false;
    std::map<std::string, float> left_base_;   // the left group's authored x (twips)
};

}  // namespace tdkr
