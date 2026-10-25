// MetalFX where there is none (metalfx.mm is the macOS one).
#include "render/metalfx.h"

#if !(defined(__APPLE__) && defined(TDKR_METALFX))

namespace tdkr {

struct MetalFx::State {};

MetalFx::MetalFx(SDL_GPUDevice* device) : device_(device) {
#ifdef __APPLE__
    why_ = "NOT IN THIS BUILD";
#endif
}

MetalFx::~MetalFx() = default;

SDL_GPUTexture* MetalFx::upscale(SDL_GPUCommandBuffer*, const TemporalUpscale&, const TemporalInputs&) { return nullptr; }

}  // namespace tdkr

#endif
