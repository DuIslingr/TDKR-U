// What the temporal upscalers (FSR 3, DLSS, MetalFX) take besides the frame: the camera's
// sub-pixel jitter, and each frame's motion vectors and depth. The camera's motion is worked
// out from the depth here (render/shaders/fsr3_inputs.comp); the renderer draws what moved
// on its own over it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

namespace tdkr {

// A frame for DLSS or MetalFX.
struct TemporalUpscale {
    SDL_GPUTexture* color = nullptr;   // render size: the finished frame
    SDL_GPUTexture* scene_depth = nullptr;   // its depth buffer (D32F, reversed)
    uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
    int mode = 1;                      // Upscale::mode
};

class TemporalInputs {
public:
    // A compute pipeline from SPIR-V, in whatever form the device takes (the counts as
    // SDL_GPUComputePipelineCreateInfo's; code, size and format left empty).
    using ComputeFactory =
        std::function<SDL_GPUComputePipeline*(const std::vector<uint32_t>& spirv, const SDL_GPUComputePipelineCreateInfo& counts)>;
    TemporalInputs(SDL_GPUDevice* device, const std::string& shader_dir, const ComputeFactory& make);
    ~TemporalInputs();
    TemporalInputs(const TemporalInputs&) = delete;
    TemporalInputs& operator=(const TemporalInputs&) = delete;
    bool ok() const { return pipeline_ != nullptr; }

    // The camera's sub-pixel offset for the next frame, in render pixels (x right, y down):
    // a Halton (2, 3) sequence, longer the more the frame is upscaled (FSR's; DLSS and MetalFX
    // take any such sequence).
    glm::vec2 next_jitter(uint32_t render_w, uint32_t output_w);

    struct Frame {
        SDL_GPUTexture* depth = nullptr;   // the scene's, reversed (near 1)
        uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
        glm::mat4 view{1}, proj{1};        // the projection without the jitter
        glm::vec2 jitter{0};               // next_jitter's, as drawn
        bool reset = false;                // a cut: no history
    };
    // This frame's motion and depth. A cut too: the first frame, a new size, a pause in the
    // frames (a menu, the upscaler off for a while), a jump of the camera.
    void prepare(SDL_GPUCommandBuffer* cmd, const Frame& f);

    // RG16F, render size: UV units, from this frame back to the previous one (FSR's sign;
    // DLSS and MetalFX scale it to pixels). A colour target too, for what moved on its own.
    SDL_GPUTexture* motion() const { return motion_; }
    SDL_GPUTexture* depth() const { return depth_; }   // R32F, render size
    bool reset() const { return reset_; }
    float dt() const { return dt_; }                    // seconds since the previous frame
    glm::vec2 jitter() const { return jitter_; }
    const glm::mat4& reprojected_from() const { return reprojected_from_; }   // the previous view-projection

private:
    SDL_GPUDevice* device_;
    SDL_GPUComputePipeline* pipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture *motion_ = nullptr, *depth_ = nullptr;
    uint32_t render_w_ = 0, render_h_ = 0, output_w_ = 0, output_h_ = 0;
    int32_t jitter_index_ = 0;
    glm::vec2 jitter_{0};
    glm::mat4 previous_view_proj_{1}, reprojected_from_{1};
    glm::vec3 previous_camera_{0};
    uint64_t last_ticks_ = 0;
    bool reset_ = true;
    float dt_ = 0;
};

}  // namespace tdkr
