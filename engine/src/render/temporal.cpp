#include "render/temporal.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include <glm/gtc/matrix_transform.hpp>

namespace tdkr {

namespace {

float halton(int32_t index, int32_t base) {
    float f = 1, result = 0;
    for (int32_t i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}

}  // namespace

TemporalInputs::TemporalInputs(SDL_GPUDevice* device, const std::string& shader_dir, const ComputeFactory& make)
    : device_(device) {
    std::ifstream in(shader_dir + "/fsr3_inputs.comp.spv", std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 20 || bytes.size() % 4) {
        std::fprintf(stderr, "temporal upscaling: no fsr3_inputs.comp.spv\n");
        return;
    }
    std::vector<uint32_t> code(bytes.size() / 4);
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<char*>(code.data()));
    SDL_GPUComputePipelineCreateInfo counts{};
    counts.entrypoint = "main";
    counts.num_samplers = 1;
    counts.num_readwrite_storage_textures = 2;
    counts.num_uniform_buffers = 1;
    counts.threadcount_x = 8, counts.threadcount_y = 8, counts.threadcount_z = 1;
    pipeline_ = make(code, counts);
    if (!pipeline_) {
        std::fprintf(stderr, "temporal upscaling: %s\n", SDL_GetError());
        return;
    }
    SDL_GPUSamplerCreateInfo si{};
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &si);
}

TemporalInputs::~TemporalInputs() {
    for (SDL_GPUTexture* t : {motion_, depth_})
        if (t) SDL_ReleaseGPUTexture(device_, t);
    if (pipeline_) SDL_ReleaseGPUComputePipeline(device_, pipeline_);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
}

glm::vec2 TemporalInputs::next_jitter(uint32_t render_w, uint32_t output_w) {
    // ffxFsr3UpscalerGetJitterPhaseCount / GetJitterOffset
    const float ratio = float(output_w) / float(std::max(render_w, 1u));
    const int32_t phases = std::max(int32_t(8.0f * ratio * ratio), 1);
    const int32_t i = jitter_index_++ % phases;
    if (jitter_index_ >= phases * 64) jitter_index_ = 0;
    return {halton(i + 1, 2) - 0.5f, halton(i + 1, 3) - 0.5f};
}

void TemporalInputs::prepare(SDL_GPUCommandBuffer* cmd, const Frame& f) {
    const uint32_t rw = f.render_w, rh = f.render_h;
    bool reset = f.reset;
    if (rw != render_w_ || rh != render_h_ || f.output_w != output_w_ || f.output_h != output_h_ || !motion_) {
        for (SDL_GPUTexture** t : {&motion_, &depth_})
            if (*t) SDL_ReleaseGPUTexture(device_, *t), *t = nullptr;
        auto make = [&](SDL_GPUTextureFormat format) {
            SDL_GPUTextureCreateInfo ti{};
            ti.type = SDL_GPU_TEXTURETYPE_2D;
            ti.format = format;
            ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            ti.width = std::max(rw, 1u), ti.height = std::max(rh, 1u);
            ti.layer_count_or_depth = 1;
            ti.num_levels = 1;
            SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &ti);
            if (!t) std::fprintf(stderr, "temporal upscaling: texture %ux%u: %s\n", rw, rh, SDL_GetError());
            return t;
        };
        motion_ = make(SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT);
        depth_ = make(SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
        render_w_ = rw, render_h_ = rh, output_w_ = f.output_w, output_h_ = f.output_h;
        reset = true;
    }
    const uint64_t now = SDL_GetTicksNS();
    const float dt = last_ticks_ ? float(now - last_ticks_) * 1e-9f : 1.0f;
    last_ticks_ = now;
    if (dt > 0.25f) reset = true;
    const glm::vec3 camera = glm::vec3(glm::inverse(f.view)[3]);
    if (glm::distance(camera, previous_camera_) > 10.0f) reset = true;
    previous_camera_ = camera;
    const glm::mat4 view_proj = f.proj * f.view;
    if (reset) previous_view_proj_ = view_proj;
    reset_ = reset, dt_ = dt, jitter_ = f.jitter;
    reprojected_from_ = previous_view_proj_;

    struct {
        glm::mat4 reproject;
        glm::vec4 size, jitter;
    } u;
    const glm::vec2 jitter_ndc(2.0f * f.jitter.x / float(rw), -2.0f * f.jitter.y / float(rh));
    const glm::mat4 jittered = glm::translate(glm::mat4(1), glm::vec3(jitter_ndc, 0)) * view_proj;
    u.reproject = previous_view_proj_ * glm::inverse(jittered);
    u.size = {float(rw), float(rh), 1.0f / float(rw), 1.0f / float(rh)};
    u.jitter = {jitter_ndc, 0, 0};
    SDL_GPUStorageTextureReadWriteBinding out[2]{};
    out[0].texture = motion_, out[1].texture = depth_;
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, out, 2, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, pipeline_);
    const SDL_GPUTextureSamplerBinding in{f.depth, sampler_};
    SDL_BindGPUComputeSamplers(pass, 0, &in, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof u);
    SDL_DispatchGPUCompute(pass, (rw + 7) / 8, (rh + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
    previous_view_proj_ = view_proj;
}

}  // namespace tdkr
