// Player settings (the in-game menu's Options), kept between runs in a small key=value
// file in the user's preferences folder (SDL_GetPrefPath).
#pragma once

#include <string>

namespace tdkr {

struct Settings {
    // Sound (the original's Sound page).
    bool sound = true;
    float sfx_volume = 1, music_volume = 1;   // 0..1
    // Controls.
    float sensitivity = 1;      // look speed multiplier, 0.25..2.5
    bool invert_y = false, left_handed = false;
    bool aim_toggle = false;    // mouse and keyboard: the right button toggles aim mode (else hold it)
    // Advanced.
    bool aim_assist = true, auto_switch = true, auto_leveling = false, metric = true;
    // Display: the window or fullscreen size (16:9), the mode, V-Sync and a frame-rate cap.
    int width = 1600, height = 900;
    enum class DisplayMode { Windowed, Fullscreen, Borderless } display = DisplayMode::Windowed;
    bool vsync = true;
    int fps_limit = 0;          // frames per second; 0: unlocked
    // Upscaling: the scene drawn at a lower resolution and upscaled (FSR 1: spatial; FSR 3,
    // DLSS, MetalFX: temporal), or at full resolution and anti-aliased (Native AA, DLAA). The
    // HUD is always drawn at full resolution.
    enum class Upscaler { Off, Fsr1, Fsr3, Dlss, MetalFx } upscaler = Upscaler::Off;
    enum class UpscaleMode { NativeAa, Quality, Balanced, Performance, UltraPerformance } upscale_mode = UpscaleMode::Quality;
    float sharpness = 0.5f;     // the upscaler's sharpening (RCAS), 0..1
    // Campaign progress: how many chapters the player has reached (the C++ main menu's chapter
    // select), and the missions the story has unlocked (UnlockMission(chapter, mission): the
    // Flash Chapter Select's), as "chapter.mission" separated by commas. The first is always open.
    int chapters = 1;
    std::string missions = "1.1";
    bool mission_unlocked(int chapter, int mission) const;
    bool unlock_mission(int chapter, int mission);   // true if it was locked
    // Options' Reset Progress (Application::ResetAllProgress): back to a new player's.
    void reset_progress();
    // The difficulty (0 Easy, 1 Normal, 2 Hard), and whether Hard is offered (the campaign
    // finished once).
    int difficulty = 1;
    bool hard_unlocked = false;
    // Credits (the shop's currency), earned at the end of each level.
    int credits = 0;
    // The Military Shop's lasting purchases (weapons, the Repel and Paralyze upgrades), a bit
    // per upgrade (gol 4510's index).
    int upgrades = 0;

    // The settings file; empty if there is no preferences folder.
    static std::string default_path();
    // Missing or unreadable files leave the defaults.
    static Settings load(const std::string& path);
    bool save(const std::string& path) const;
};

}  // namespace tdkr
