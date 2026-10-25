#include "game/display_options.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace tdkr {

namespace {

struct Size {
    int w, h;
};
constexpr Size kResolutions[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
constexpr int kFrameRates[] = {30, 45, 60, 75, 90, 120, 0};   // 0: unlocked
const char* const kModes[] = {"WINDOWED", "FULLSCREEN", "BORDERLESS"};

int wrap(int i, int n, int direction) { return (i + (direction == 0 ? 1 : direction) % n + n) % n; }

const char* upscaler_name(Settings::Upscaler u) {
    switch (u) {
        case Settings::Upscaler::Off: return "OFF";
        case Settings::Upscaler::Fsr1: return "AMD FSR 1";
        case Settings::Upscaler::Fsr3: return "AMD FSR 3";
        case Settings::Upscaler::Dlss: return "NVIDIA DLSS";
        case Settings::Upscaler::MetalFx: return "APPLE METALFX";
    }
    return "";
}

// The mode's name as each upscaler calls it: full resolution is FSR's Native AA, DLSS's DLAA,
// MetalFX's native temporal mode; FSR 1 has no anti-aliasing (it only sharpens).
std::string mode_name(Settings::Upscaler u, Settings::UpscaleMode m) {
    using M = Settings::UpscaleMode;
    switch (m) {
        case M::NativeAa:
            return u == Settings::Upscaler::Dlss ? "DLAA" : u == Settings::Upscaler::Fsr1 ? "NATIVE (SHARPEN ONLY)" : "NATIVE AA";
        case M::Quality: return "QUALITY";
        case M::Balanced: return "BALANCED";
        case M::Performance: return "PERFORMANCE";
        case M::UltraPerformance: return "ULTRA PERFORMANCE";
    }
    return "";
}

bool available(Settings::Upscaler u, const UpscalerSupport& s) {
    switch (u) {
        case Settings::Upscaler::Off: return true;
        case Settings::Upscaler::Fsr1: return s.fsr1;
        case Settings::Upscaler::Fsr3: return s.fsr3;
        case Settings::Upscaler::Dlss: return s.dlss;
        case Settings::Upscaler::MetalFx: return s.metalfx;
    }
    return false;
}

}  // namespace

std::string title_case(const std::string& upper) {
    static const char* const kKept[] = {"AMD", "FSR", "NVIDIA", "DLSS", "DLAA", "AA", "FPS", "RTX"};
    static const std::pair<const char*, const char*> kNamed[] = {{"X", "x"}, {"V-SYNC", "V-Sync"}, {"METALFX", "MetalFX"}, {"MACOS", "macOS"}};
    std::string out;
    size_t i = 0;
    while (i < upper.size()) {
        const size_t end = std::min(upper.find(' ', i), upper.size());
        std::string word = upper.substr(i, end - i);
        const size_t lead = word.find_first_not_of("(:");                  // "(NOT" -> "(" + "NOT"
        const size_t tail = word.find_last_not_of("):%");                  // "MODE:" -> "MODE" + ":"
        std::string core = lead == std::string::npos ? "" : word.substr(lead, tail + 1 - lead);
        bool done = false;
        for (const char* k : kKept) done |= core == k;
        for (const auto& [from, to] : kNamed)
            if (core == from) core = to, done = true;
        if (!done)
            for (size_t c = 1; c < core.size(); ++c) core[c] = char(std::tolower(static_cast<unsigned char>(core[c])));
        if (lead != std::string::npos) word = word.substr(0, lead) + core + word.substr(tail + 1);
        out += word;
        if (end < upper.size()) out += ' ';
        i = end + 1;
    }
    return out;
}

float upscale_ratio(Settings::UpscaleMode m) {
    using M = Settings::UpscaleMode;
    switch (m) {   // (the ratios FSR, DLSS and MetalFX share)
        case M::NativeAa: return 1.0f;
        case M::Quality: return 1.5f;
        case M::Balanced: return 1.7f;
        case M::Performance: return 2.0f;
        case M::UltraPerformance: return 3.0f;
    }
    return 1.0f;
}

Settings::Upscaler active_upscaler(const Settings& s, const UpscalerSupport& support) {
    return available(s.upscaler, support) ? s.upscaler : Settings::Upscaler::Off;
}

std::vector<DisplayOption> display_options(const Settings& s, const UpscalerSupport& support) {
    std::vector<DisplayOption> out;
    char size[32];
    std::snprintf(size, sizeof size, "%d X %d", s.width, s.height);
    out.push_back({"RESOLUTION", size});
    out.push_back({"DISPLAY MODE", kModes[int(s.display)]});
    out.push_back({"V-SYNC", s.vsync ? "ON" : "OFF"});
    out.push_back({"FRAME RATE LIMIT", s.fps_limit == 0 ? "UNLOCKED" : std::to_string(s.fps_limit) + " FPS"});
    std::string up = upscaler_name(s.upscaler);
    if (!available(s.upscaler, support))
        up += " (" + (s.upscaler == Settings::Upscaler::MetalFx ? support.metalfx_why
                      : s.upscaler == Settings::Upscaler::Dlss  ? support.dlss_why
                                                                : support.fsr3_why) + ")";
    out.push_back({"UPSCALER", up});
    // (always listed, so the rows stay where they are: the Flash page rewrites them in place)
    const bool on = s.upscaler != Settings::Upscaler::Off;
    out.push_back({"UPSCALER MODE", on ? mode_name(s.upscaler, s.upscale_mode) : "-"});
    char sharp[16];
    std::snprintf(sharp, sizeof sharp, "%d%%", int(std::lround(s.sharpness * 100)));
    out.push_back({"SHARPNESS", on ? sharp : "-"});
    return out;
}

bool step_display_option(Settings& s, int row, int direction, const UpscalerSupport&) {
    switch (row) {
        case 0: {
            int i = 1;
            for (int k = 0; k < int(std::size(kResolutions)); ++k)
                if (kResolutions[k].w == s.width && kResolutions[k].h == s.height) i = k;
            i = wrap(i, int(std::size(kResolutions)), direction);
            s.width = kResolutions[i].w, s.height = kResolutions[i].h;
            return true;
        }
        case 1: s.display = Settings::DisplayMode(wrap(int(s.display), 3, direction)); return true;
        case 2: s.vsync = !s.vsync; return true;
        case 3: {
            int i = 0;
            for (int k = 0; k < int(std::size(kFrameRates)); ++k)
                if (kFrameRates[k] == s.fps_limit) i = k;
            s.fps_limit = kFrameRates[wrap(i, int(std::size(kFrameRates)), direction)];
            return false;
        }
        case 4: s.upscaler = Settings::Upscaler(wrap(int(s.upscaler), 5, direction)); return false;
        case 5: s.upscale_mode = Settings::UpscaleMode(wrap(int(s.upscale_mode), 5, direction)); return false;
        case 6: {   // in tenths, wrapping
            int t = int(std::lround(s.sharpness * 10));
            t = wrap(t, 11, direction);
            s.sharpness = float(t) / 10.0f;
            return false;
        }
    }
    return false;
}

}  // namespace tdkr
