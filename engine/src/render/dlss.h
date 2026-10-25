// NVIDIA DLSS Super Resolution, and DLAA (its anti-aliasing at the same size), through NGX on
// Vulkan: the frame, depth and motion vectors of render/temporal.h handed over as the patched
// SDL's own Vulkan objects (third_party/sdl). Built only with NVIDIA's DLSS SDK (TDKR_DLSS,
// from -DTDKR_DLSS_SDK=..., not part of this repository); it runs on an RTX card with NVIDIA's
// driver and the SDK's DLSS runtime (libnvidia-ngx-dlss.so.*, nvngx_dlss.dll) beside the game.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "render/temporal.h"

namespace tdkr {

// What the GPU device has to be made with for DLSS (NGX's Vulkan extensions), found before it
// is: a short-lived Vulkan instance looks for an NVIDIA GPU and asks NGX about it.
struct DlssRequirements {
    bool possible = false;
    std::string why = "NVIDIA RTX ONLY";   // (when not, for the Display options)
    uint32_t vulkan_version = 0;
    std::vector<std::string> instance_extensions, device_extensions;
};
DlssRequirements dlss_requirements();

class Dlss {
public:
    Dlss(SDL_GPUDevice* device, const DlssRequirements& requirements);
    ~Dlss();
    Dlss(const Dlss&) = delete;
    Dlss& operator=(const Dlss&) = delete;
    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }

    // The upscaled frame (output size, RGBA16F), or null if DLSS failed on it.
    SDL_GPUTexture* upscale(SDL_GPUCommandBuffer* cmd, const TemporalUpscale& f, const TemporalInputs& in);

private:
    struct State;
    std::unique_ptr<State> state_;
    SDL_GPUDevice* device_;
    bool ok_ = false;
    std::string why_;
};

}  // namespace tdkr
