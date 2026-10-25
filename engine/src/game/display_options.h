// The Display options (this port's): the window or screen size (16:9 up to 3840x2160), the
// display mode, V-Sync, the frame-rate cap (30-120 or unlocked; the game's logic steps at a
// fixed 120 Hz either way), and the upscaler with its mode and sharpening. Both menus show
// them: the C++ pause menu's Display page and the Flash options' DISPLAY page.
#pragma once

#include <string>
#include <vector>

#include "game/settings.h"
#include "render/upscaler.h"

namespace tdkr {

struct DisplayOption {
    std::string name;    // RESOLUTION, ...
    std::string value;   // 1920 X 1080, ...
};

// The rows as they stand.
std::vector<DisplayOption> display_options(const Settings& s, const UpscalerSupport& support);
// A step of row `row` (direction -1 / +1; 0 a press: forward, wrapping). True if the window or
// swapchain must change (size, mode, V-Sync); the frame cap and the upscaler apply as they are.
bool step_display_option(Settings& s, int row, int direction, const UpscalerSupport& support);

// A row in the Flash menus' casing (their rows read "Sound", "Collectibles"): each word
// capitalised, the acronyms kept ("Upscaler: AMD FSR 1", "Resolution: 1920 x 1080").
std::string title_case(const std::string& upper);

// The render scale an upscaler mode stands for (output size / render size).
float upscale_ratio(Settings::UpscaleMode m);
// The upscaler that runs: the chosen one if it can, else none.
Settings::Upscaler active_upscaler(const Settings& s, const UpscalerSupport& support);

}  // namespace tdkr
