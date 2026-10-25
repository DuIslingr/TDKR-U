// Apple MetalFX's temporal scaler (macOS 13 and later), and at the same size its
// anti-aliasing: the frame, depth and motion vectors of render/temporal.h handed over as the
// patched SDL's own Metal objects (third_party/sdl). macOS builds only (metalfx.mm); elsewhere
// (metalfx.cpp) it says why it cannot run.
#pragma once

#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "render/temporal.h"

namespace tdkr {

class MetalFx {
public:
    explicit MetalFx(SDL_GPUDevice* device);
    ~MetalFx();
    MetalFx(const MetalFx&) = delete;
    MetalFx& operator=(const MetalFx&) = delete;
    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }

    // The upscaled frame (output size, RGBA16F), or null if MetalFX failed on it.
    SDL_GPUTexture* upscale(SDL_GPUCommandBuffer* cmd, const TemporalUpscale& f, const TemporalInputs& in);

private:
    struct State;
    std::unique_ptr<State> state_;
    SDL_GPUDevice* device_;
    bool ok_ = false;
    std::string why_ = "MACOS ONLY";
};

}  // namespace tdkr
