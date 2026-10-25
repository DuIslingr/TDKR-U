// Upscaling (the Display options' upscaler): what this build and GPU can run, and what a frame
// asks for. The renderer draws the scene and its post effects at the render size, the
// upscaler brings them to the output size, then the HUD is drawn at the output size.
#pragma once

#include <string>

namespace tdkr {

enum class UpscalerKind { None, Fsr1, Fsr3, Dlss, MetalFx };

struct UpscalerSupport {
    bool fsr1 = true, fsr3 = false, dlss = false, metalfx = false;
    std::string fsr3_why = "NOT AVAILABLE", dlss_why = "NOT AVAILABLE", metalfx_why = "MACOS ONLY";   // shown when not
};

struct Upscale {
    UpscalerKind kind = UpscalerKind::None;
    float ratio = 1;          // output size / render size (1: native resolution, anti-aliasing / sharpening only)
    int mode = 1;             // 0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    float sharpness = 0.5f;   // 0..1
    bool reset = false;       // a cut: the temporal upscalers drop their history
};

}  // namespace tdkr
