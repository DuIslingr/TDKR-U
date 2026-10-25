// The game: a level and everything that plays it (scripts, combat, the first-person view,
// HUD and menus), run in a window or headless for screenshots and tests. Built for one
// level from the options; later levels load into the same objects.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/camera.h"
#include "app/options.h"
#include "audio/audio.h"
#include "core/vfs.h"
#include "game/combat.h"
#include "game/death_screen.h"
#include "game/display_options.h"
#include "game/end_level_screen.h"
#include "game/flash_hud.h"
#include "game/flash_screen.h"
#include "game/front_end.h"
#include "game/missions.h"
#include "game/hud.h"
#include "game/lens_flare.h"
#include "game/level_logic.h"
#include "game/loading_screen.h"
#include "game/main_menu.h"
#include "game/pause_menu.h"
#include "game/batpod.h"
#include "game/brawl.h"
#include "game/hero.h"
#include "game/player.h"
#include "game/save_game.h"
#include "game/scene.h"
#include "game/settings.h"
#include "game/storyboard.h"
#include "game/shop.h"
#include "game/touch_controls.h"
#include "game/viewmodel.h"
#include "render/renderer.h"
#include "render/text.h"

struct SDL_Window;
union SDL_Event;

namespace tdkr {

class Game {
public:
    // Loads the options' level (or shows the main menu first). `window` is null headless.
    Game(const Options& options, const Vfs& vfs, const Gol& gol, SDL_Window* window, const std::string& shader_dir);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // The headless runs (screenshot, walk test, audit) or the window's loop; the exit code.
    int run();

private:
    static constexpr float kPhysicsStep = 1.0f / 120.0f;

    // Screens over or instead of the level.
    const CampaignLevel* campaign_level(const std::string& level) const;
    FrameParams draw_loading(int w, int h);
    void show_loading(const std::string& level, bool section = false);
    FrameParams draw_main_menu(int w, int h);
    // The level's frame as seen by `cam`: fog, lights, particles, post effects.
    FrameParams frame(const Camera& cam, int w, int h);
    // The HUD, prompts, subtitles and menus over a frame.
    void draw_text(FrameParams& f, int w, int h);
    static void fullscreen(Overlay& o, int w, int h, const glm::vec4& c);

    // Campaign progress and the save file.
    std::string next_level(const std::string& level) const;
    int chapter_of(const std::string& level) const;
    bool saving(const std::string& level) const;
    // The save file as the story stands (game/save_game.h): at a checkpoint, on entering a
    // level (RequireLoadLevel, with its spawn point), or a mission started from the menu.
    enum class SaveKind { Checkpoint, LevelChange, MissionStart };
    void write_save(SaveKind kind, int spawn = -1);
    EndLevelScreen::Stats end_stats();
    EndLevelScreen::Rules end_rules();
    void load_level(const std::string& level);
    void resume(const SaveGame& g);

    // A fixed step of level logic (and what it sets off: sections, respawns, level ends).
    void step_logic(float dt);
    void movement_states(bool aim);
    // A combat step: the player's bullets leave from the view model's muzzle, which flashes
    // for each shot fired.
    void fire_step(float dt, const CombatInput& in, const glm::vec3& eye, const glm::vec3& aim, bool aiming);
    void arms_distort();   // the level logic's Test_distort_* requests, to the view model
    void apply_sound();
    void update_frost(float dt);

    // Headless (game_headless.cpp).
    int run_audit();
    void run_walk_test();
    int run_screenshot();
    FrameParams shot_frame(Camera base);
    // Where the view goes for a yaw/pitch: behind Batman (third person), else at the player's eye.
    glm::vec3 view_position(float yaw, float pitch) const {
        return hero.active() ? hero.camera_position(player, yaw, hero.view_pitch(pitch), scene.collision()) : player.eye();
    }

    // The window (game_window.cpp).
    void run_window();
    void handle_event(const SDL_Event& e);
    glm::vec2 to_pixels(float x, float y, bool normalized) const;
    void apply_display();
    void set_menu(bool on);
    void menu_action(PauseMenu::Action a);
    void end_action(EndLevelScreen::Action a);
    void death_action(DeathScreen::Action a);
    void main_action(MainMenu::Action a);
    void title_music(const char* state);

    Options opt;
    const Vfs& vfs;
    const Gol& gol;
    SDL_Window* window;
    const bool headless;
    Renderer renderer;
    Scene scene;
    // The loading screen (GS_Loading): the level's picture and story while it loads.
    const std::vector<CampaignLevel> campaign;
    StringTable main_strings;
    LoadingScreen loading;
    Hud loading_hud;
    TextRenderer loading_title, loading_text;
    Overlay loading_overlay;
    ParticleList loading_sprites;
    int loading_fonts_for;
    MainMenu main_menu;
    float clock;
    const PostParams* post_params = nullptr;   // the level logic's, once it exists
    const float* zfar = nullptr;               // the level logic's far plane (SetZFar), once it exists
    ParticleList particle_list;
    Player player;
    Hero hero;                                 // TDKR: Batman in third person and the follow camera
    LevelLogic logic;
    Brawl brawl{logic, scene, player, hero};   // TDKR: Batman's fights
    std::unique_ptr<Audio> audio;
    std::vector<float> rendered;               // --sound-render
    Combat combat;
    Overlay overlay;
    ViewModel viewmodel;
    // Subtitles: the latest script message, bottom centre.
    TextRenderer subtitles;
    ParticleList hud_sprites;
    Hud hud;
    FlashHud flash_hud;            // TDKR's HUD (BatmanHUD.swf) once Batman is playable
    std::string flash_hud_level;   // the level it was set up for
    float flash_hud_clock = -1;
    int flash_hud_checkpoint = -1;   // the checkpoint whose notice it showed
    int flash_hud_objective = 0;               // the last hud_objective serial shown
    // TDKR's menus from its Flash movies (title, main menu, chapters, pause); the N.O.V.A. 3
    // menus remain for data without them.
    std::vector<Mission> missions;
    FrontEnd front_end;
    bool flash_menus = false;
    int mission = -1;                         // the mission being played
    std::chrono::steady_clock::time_point menu_clock{};
    void show_main_menu();
    void front_action(FrontEnd::Action a);
    void start_mission(int index);
    float menu_dt();
    bool menu_scene_camera(Camera& cam);
    std::string menu_camera_level;
    int menu_camera_object = -1;
    glm::vec3 menu_eye{0};
    bool menu_scene_shown = false;   // the last draw_main_menu showed the menu level behind
    std::unique_ptr<FlashScreen> swf_view;   // --swf: a Flash movie over the game (input goes to it)
    float swf_clock = -1;
    void start_swf_view(int w, int h);
    TouchControls touch;                       // the Touch scheme's on-screen controls
    const std::string settings_path;
    Settings settings;
    PauseMenu menu;
    UpscalerSupport upscalers;   // what the renderer can run (the Display options' upscaler rows)
    Shop shop;                                 // the Military Shop on the pause page
    const std::string save_path;
    LensFlares flares;
    DeathScreen death;
    EndLevelScreen end_screen;
    TextRenderer menu_title, objective_text, dialogue;
    uint32_t frost_serial = 0;
    int fonts_for;                             // the screen height the fonts are sized for
    float flare_clock = -1;
    bool end_screen_done = false;              // dismissed with Continue: the level's end goes on
    float ended_for = 0;                       // seconds since the level ended
    Camera cam;
    uint32_t view_spawns = 0, view_jumps = 0, distort_serial = 0;

    // Playing in the window.
    bool flying = false;
    float accumulator = 0;
    CombatInput combat_in;
    bool fire_held = false, aim_held = false, aim_toggled = false;
    bool touch_mode = false;
    // A gamepad (SDL3), laid out as the Android build's PowerA pad: left stick moves, right
    // stick looks, A jumps / glides, X attacks, Y counters, B batarang, RT grapnel, LT gadget,
    // Start pauses; in menus the D-pad or left stick moves, A selects, B goes back.
    SDL_Gamepad* pad = nullptr;
    bool pad_trigger_held = false;          // RT past its threshold (fires once per pull)
    bool pad_a_used = false;                // A was the use button: no jump until it is let go
    glm::vec2 pad_menu_stick{0};            // last left-stick direction sent to a menu
    void pad_event(const SDL_Event& e);
    float pad_axis(SDL_GamepadAxis axis) const;   // -1..1, with a dead zone
    // The grapnel may fire: no script has it off (SetGrapnelControlEnable) and no cinematic runs.
    bool can_grapple() const { return !opt.scripts || (logic.controls.grapnel && !logic.player_locked()); }
    // Fire the grapnel: at the scripts' locked target if there is one in reach, else at the edge
    // the view aims at.
    bool fire_grapnel(const glm::vec3& eye, const glm::vec3& forward);
    bool answer_qte();   // a QTE button pressed: true if one was up
    // The Batpod (EnterBatPod): one physics step of the ride; false when Batman is on foot.
    static constexpr float kHangBelow = 3.2f;   // Batman's feet under a helicopter he hangs from
    bool hanging_ = false;
    bool auto_jump = false;   // --auto-play: jump next step
    bool auto_fan_flight = false;   // --auto-play: aloft from a fan it jumped up (it steers the glide)
    std::unordered_set<int> auto_fans_used;   // the fans risen on since
    bool ride_step(const PlayerInput& in, float dt);
    Batpod batpod;
    Storyboard storyboard;   // StartIntroStory
    std::string story_callback;
    // --auto-play on the Batpod: after the first objective marker, along its trail (an object
    // on the move: tailed from 40-50 m back).
    PlayerInput auto_drive();
    std::deque<glm::vec3> ride_trail;
    int ride_target = -1;
    float ride_stuck = 0, ride_backup = 0;   // seconds
    int ride_tries = 0;                     // back-ups since it last got anywhere
    glm::vec3 ride_progress{0};
    void auto_play(float dt);   // --auto-play
    glm::vec3 on_floor(const glm::vec3& p) const;
    float auto_play_wait = 3.0f;
    float auto_play_held = 0;   // seconds it has waited for a fight, a cinematic, ...
    std::vector<glm::vec3> auto_play_path;   // the feet's next positions
    int auto_play_use = -1;                  // the thing to use at the end of it
    std::unordered_set<int> auto_play_entered;   // triggers walked into to get a place marker to go
    std::unordered_map<int, int> auto_play_stale;   // place markers stood on with nothing happening
    std::vector<int> auto_play_markers;   // the markers when it last looked
    float fight_test_hack = 0;   // --fight-test: seconds into a hacking game
    int fight_test_lock = -1;    // --fight-test: the grapnel lock it last flew to
    bool hud_pointer = false;    // the HUD (the hacking game) has the mouse pointer
    bool running = true;
    std::chrono::steady_clock::time_point last;
};

}  // namespace tdkr
