// AMD FSR 3's upscaler on SDL's GPU compute: third_party/fidelityfx's passes as they ship, the
// host side after the SDK's ffx_fsr3upscaler.cpp (MIT, as third_party/fidelityfx/LICENSE.txt).
// A jittered frame with its depth and motion becomes an upscaled one, accumulated over frames;
// at the same size it is anti-aliasing only (Native AA).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "render/temporal.h"

namespace tdkr {

class Fsr3 {
public:
    // SPIR-V devices only (the passes are compiled to it); ok() is false elsewhere.
    Fsr3(SDL_GPUDevice* device, const std::string& shader_dir);
    ~Fsr3();
    Fsr3(const Fsr3&) = delete;
    Fsr3& operator=(const Fsr3&) = delete;
    bool ok() const { return ok_; }

    struct Frame {
        SDL_GPUTexture* color = nullptr;   // render size: the finished frame
        uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
        glm::mat4 proj{1};                 // the projection without the jitter
        float sharpness = 0.5f;            // 0..1 (0: none)
    };
    // The upscaled frame (output size, RGBA16F), from this frame and the motion, depth and
    // jitter TemporalInputs::prepare made for it.
    SDL_GPUTexture* upscale(SDL_GPUCommandBuffer* cmd, const Frame& f, const TemporalInputs& in);

private:
    enum Pass {
        PrepareInputs, LumaPyramid, ShadingChangePyramid, ShadingChange, PrepareReactivity,
        LumaInstability, Accumulate, AccumulateSharpen, Rcas, DebugView, kPasses
    };
    struct Texture {
        SDL_GPUTexture* texture = nullptr;
        uint32_t mip = 0;
    };
    void ensure_resources(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh);
    void release_resources();
    SDL_GPUTexture* make(SDL_GPUTextureFormat format, uint32_t w, uint32_t h, uint32_t levels = 1);
    void clear(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t, glm::vec4 value, uint32_t levels = 1);
    void run(SDL_GPUCommandBuffer* cmd, Pass pass, const std::vector<SDL_GPUTexture*>& srvs, const std::vector<Texture>& uavs,
             const std::vector<std::pair<const void*, uint32_t>>& constants, uint32_t groups_x, uint32_t groups_y);

    SDL_GPUDevice* device_;
    bool ok_ = false;
    std::array<SDL_GPUComputePipeline*, kPasses> pipelines_{};
    SDL_GPUSampler* sampler_ = nullptr;   // linear, clamped: every pass's SampleLevel
    SDL_GPUSampler* point_sampler_ = nullptr;   // (for the integer texture)

    uint32_t render_w_ = 0, render_h_ = 0, output_w_ = 0, output_h_ = 0;
    std::vector<SDL_GPUTexture*> owned_;
    // (the SDK's resources: render size, half of it, the output size; pairs swap each frame)
    SDL_GPUTexture *dilated_depth_ = nullptr, *dilated_motion_ = nullptr, *previous_nearest_depth_ = nullptr;
    SDL_GPUTexture* intermediate_ = nullptr;   // the farthest depth, then the luma instability
    std::array<SDL_GPUTexture*, 2> luma_{}, luma_history_{}, accumulation_{}, upscaled_{};
    SDL_GPUTexture *shading_change_ = nullptr, *farthest_depth_mip1_ = nullptr, *spd_mips_ = nullptr;
    uint32_t spd_levels_ = 0;
    SDL_GPUTexture *spd_atomic_ = nullptr, *frame_info_ = nullptr, *reactive_masks_ = nullptr, *new_locks_ = nullptr;
    SDL_GPUTexture *output_ = nullptr, *exposure_ = nullptr, *no_reactivity_ = nullptr;
    bool cleared_ = false;   // the defaults and first-frame clears done

    uint32_t frame_ = 0;          // even and odd frames swap the pairs
    float jitter_phases_ = 0;     // eases towards the size ratio's count
    float frame_index_ = 0;       // frames accumulated since a reset
    glm::vec2 previous_jitter_{0};
    glm::ivec2 previous_render_{0}, previous_output_{0};
};

}  // namespace tdkr
