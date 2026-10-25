#include "render/fsr3.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace tdkr {

namespace {

using F = SDL_GPUTextureFormat;
constexpr F kRG16 = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, kR16 = SDL_GPU_TEXTUREFORMAT_R16_FLOAT,
            kR32F = SDL_GPU_TEXTUREFORMAT_R32_FLOAT, kR32U = SDL_GPU_TEXTUREFORMAT_R32_UINT,
            kRGBA16 = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, kRGBA32 = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
            kR8 = SDL_GPU_TEXTUREFORMAT_R8_UNORM, kRGBA8 = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            kRG32 = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;

// Each pass: its shader, its textures (SRVs, set 0), its storage images in binding order
// (UAVs, set 1: the formats they are created in), its constant buffers (set 2), its group size.
struct PassInfo {
    const char* file;
    const char* entry;
    uint32_t samplers;
    std::vector<F> storage;
    uint32_t uniforms;
    uint32_t threads_x, threads_y;
};
const PassInfo kPassInfo[] = {
    {"fsr3_inputs.comp.spv", "main", 1, {kRG16, kR32F}, 1, 8, 8},
    {"fsr3_prepare_inputs.comp.spv", "CS", 3, {kRG16, kR32F, kR32U, kR16, kR16}, 1, 8, 8},
    {"fsr3_luma_pyramid.comp.spv", "CS", 2, {kR32U, kRGBA32, kRG16, kRG16, kRG16, kRG16, kRG16, kRG16, kR16}, 2, 256, 1},
    {"fsr3_shading_change_pyramid.comp.spv", "CS", 4, {kR32U, kRG16, kRG16, kRG16, kRG16, kRG16, kRG16}, 2, 256, 1},
    {"fsr3_shading_change.comp.spv", "CS", 1, {kR8}, 1, 8, 8},
    {"fsr3_prepare_reactivity.comp.spv", "CS", 9, {kRGBA8, kR8, kR8}, 1, 8, 8},
    {"fsr3_luma_instability.comp.spv", "CS", 7, {kRGBA16, kR16}, 1, 8, 8},
    {"fsr3_accumulate.comp.spv", "CS", 9, {kRGBA16, kRGBA16, kR8}, 1, 8, 8},
    {"fsr3_accumulate_sharpen.comp.spv", "CS", 9, {kRGBA16, kRGBA16, kR8}, 1, 8, 8},
    {"fsr3_rcas.comp.spv", "CS", 2, {kRGBA16}, 2, 64, 1},
    {"fsr3_debug_view.comp.spv", "CS", 5, {kRGBA16}, 1, 8, 8},
};

// SPIR-V's ImageFormat for a storage image's texture format (0: none).
uint32_t spirv_format(F f) {
    switch (f) {
        case kRGBA32: return 1;
        case kRGBA16: return 2;
        case kR32F: return 3;
        case kRGBA8: return 4;
        case kRG16: return 7;
        case kR16: return 9;
        case kR8: return 15;
        case kR32U: return 33;
        default: return 0;
    }
}

// glslang gives an HLSL storage image the format of its element type (float4: rgba32f) and
// SDL enables no reads or writes without a format, so each one (set 1, by binding) gets the
// format its texture is created in: a new image type (and pointer) where it differs, the
// variable and its loads moved onto it.
void set_storage_formats(std::vector<uint32_t>& code, const std::vector<F>& formats) {
    struct Instruction {
        size_t at;
        uint32_t op, words;
    };
    std::vector<Instruction> ins;
    for (size_t i = 5; i < code.size();) {
        const uint32_t words = code[i] >> 16;
        if (!words) break;
        ins.push_back({i, code[i] & 0xffff, words});
        i += words;
    }
    enum : uint32_t { OpCapability = 17, OpTypeImage = 25, OpTypePointer = 32, OpVariable = 59, OpLoad = 61, OpDecorate = 71 };
    std::unordered_map<uint32_t, uint32_t> set, binding;
    std::unordered_map<uint32_t, size_t> defined;   // a type's or variable's instruction
    for (size_t k = 0; k < ins.size(); ++k) {
        const uint32_t* w = &code[ins[k].at];
        if (ins[k].op == OpDecorate && ins[k].words >= 4) {
            if (w[2] == 34) set[w[1]] = w[3];        // DescriptorSet
            if (w[2] == 33) binding[w[1]] = w[3];    // Binding
        } else if (ins[k].op == OpTypeImage || ins[k].op == OpTypePointer) {
            defined[w[1]] = k;
        } else if (ins[k].op == OpVariable) {
            defined[w[2]] = k;
        }
    }
    auto def = [&](uint32_t id, uint32_t op) -> const Instruction* {
        const auto it = defined.find(id);
        return it != defined.end() && ins[it->second].op == op ? &ins[it->second] : nullptr;
    };
    std::map<size_t, std::vector<uint32_t>> after;                    // new types after an instruction
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> made;   // (pointer, format) -> (image, pointer)
    std::unordered_map<uint32_t, uint32_t> moved;                     // variable -> its new image type
    bool extended = false;
    for (const auto& [var, b] : binding) {
        const auto s = set.find(var);
        if (s == set.end() || s->second != 1 || b >= formats.size()) continue;
        const uint32_t format = spirv_format(formats[b]);
        const Instruction* v = def(var, OpVariable);
        const Instruction* p = v ? def(code[v->at + 1], OpTypePointer) : nullptr;
        const Instruction* img = p ? def(code[p->at + 3], OpTypeImage) : nullptr;
        if (!format || !img || code[img->at + 8] == format) continue;
        auto [it, fresh] = made.try_emplace({code[p->at + 1], format});
        if (fresh) {
            const uint32_t new_image = code[3]++, new_pointer = code[3]++;
            std::vector<uint32_t>& out = after[size_t(p - ins.data())];
            const size_t first = out.size();
            out.insert(out.end(), code.begin() + ptrdiff_t(img->at), code.begin() + ptrdiff_t(img->at + img->words));
            out[first + 1] = new_image, out[first + 8] = format;
            out.insert(out.end(), {(4u << 16) | OpTypePointer, new_pointer, code[p->at + 2], new_image});
            it->second = {new_image, new_pointer};
        }
        code[v->at + 1] = it->second.second;
        moved[var] = it->second.first;
        extended |= format == 7 || format == 9 || format == 15;   // (rg16f, r16f, r8)
    }
    if (moved.empty()) return;
    std::vector<uint32_t> out(code.begin(), code.begin() + 5);
    bool capability = false;
    for (size_t k = 0; k < ins.size(); ++k) {
        uint32_t* w = &code[ins[k].at];
        if (ins[k].op == OpLoad && moved.count(w[3])) w[1] = moved[w[3]];
        out.insert(out.end(), w, w + ins[k].words);
        if (ins[k].op == OpCapability && extended && !capability)
            out.insert(out.end(), {(2u << 16) | OpCapability, 49u}), capability = true;   // StorageImageExtendedFormats
        if (const auto a = after.find(k); a != after.end()) out.insert(out.end(), a->second.begin(), a->second.end());
    }
    code = std::move(out);
}

float halton(int32_t index, int32_t base) {
    float f = 1, result = 0;
    for (int32_t i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}

uint32_t groups(uint32_t size, uint32_t per) { return (size + per - 1) / per; }

// cbFSR3Upscaler (ffx_fsr3upscaler_callbacks_hlsl.h; Fsr3UpscalerConstants)
struct Constants {
    int32_t render_size[2], previous_render_size[2];
    int32_t upscale_size[2], previous_upscale_size[2];
    int32_t max_render_size[2], max_upscale_size[2];
    float device_to_view_depth[4];
    float jitter[2], previous_jitter[2];
    float motion_vector_scale[2], downscale_factor[2];
    float motion_vector_jitter_cancellation[2];
    float tan_half_fov, jitter_phase_count;
    float delta_time, delta_pre_exposure, view_space_to_meters, frame_index;
    float velocity_factor, reactiveness_scale, shading_change_scale, accumulation_added_per_frame, min_disocclusion_accumulation;
};
static_assert(sizeof(Constants) == 148);

struct SpdConstants {   // cbSPD
    uint32_t mips, work_groups, work_group_offset[2], render_size[2];
};

}  // namespace

Fsr3::Fsr3(SDL_GPUDevice* device, const std::string& shader_dir) : device_(device) {
    if (!(SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_SPIRV)) return;
    for (int i = 0; i < kPasses; ++i) {
        const PassInfo& p = kPassInfo[i];
        std::ifstream in(shader_dir + "/" + p.file, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() < 20 || bytes.size() % 4) {
            std::fprintf(stderr, "fsr3: no %s\n", p.file);
            return;
        }
        std::vector<uint32_t> code(bytes.size() / 4);
        std::memcpy(code.data(), bytes.data(), bytes.size());
        set_storage_formats(code, p.storage);
        SDL_GPUComputePipelineCreateInfo ci{};
        ci.code = reinterpret_cast<const Uint8*>(code.data());
        ci.code_size = code.size() * 4;
        ci.entrypoint = p.entry;
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.num_samplers = p.samplers;
        ci.num_readwrite_storage_textures = uint32_t(p.storage.size());
        ci.num_uniform_buffers = p.uniforms;
        ci.threadcount_x = p.threads_x, ci.threadcount_y = p.threads_y, ci.threadcount_z = 1;
        pipelines_[size_t(i)] = SDL_CreateGPUComputePipeline(device_, &ci);
        if (!pipelines_[size_t(i)]) {
            std::fprintf(stderr, "fsr3: %s: %s\n", p.file, SDL_GetError());
            return;
        }
    }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.max_lod = 16;
    sampler_ = SDL_CreateGPUSampler(device_, &si);
    ok_ = sampler_ != nullptr;
}

Fsr3::~Fsr3() {
    release_resources();
    for (SDL_GPUComputePipeline* p : pipelines_)
        if (p) SDL_ReleaseGPUComputePipeline(device_, p);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
}

SDL_GPUTexture* Fsr3::make(SDL_GPUTextureFormat format, uint32_t w, uint32_t h, uint32_t levels) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = format;
    // (a colour target too: cleared by a render pass)
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
               SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    ti.width = std::max(w, 1u), ti.height = std::max(h, 1u);
    ti.layer_count_or_depth = 1;
    ti.num_levels = levels;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &ti);
    if (!t) std::fprintf(stderr, "fsr3: texture %ux%u: %s\n", w, h, SDL_GetError());
    owned_.push_back(t);
    return t;
}

void Fsr3::release_resources() {
    for (SDL_GPUTexture* t : owned_)
        if (t) SDL_ReleaseGPUTexture(device_, t);
    owned_.clear();
    cleared_ = false;
}

void Fsr3::ensure_resources(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    release_resources();
    render_w_ = rw, render_h_ = rh, output_w_ = ow, output_h_ = oh;
    const uint32_t hw = std::max(rw / 2, 1u), hh = std::max(rh / 2, 1u);
    motion_ = make(kRG16, rw, rh);
    depth_ = make(kR32F, rw, rh);
    dilated_depth_ = make(kR32F, rw, rh);
    dilated_motion_ = make(kRG16, rw, rh);
    previous_nearest_depth_ = make(kR32U, rw, rh);
    intermediate_ = make(kR16, rw, rh);
    for (int i = 0; i < 2; ++i) {
        luma_[size_t(i)] = make(kR16, rw, rh);
        luma_history_[size_t(i)] = make(kRGBA16, rw, rh);
        accumulation_[size_t(i)] = make(kR8, rw, rh);
        upscaled_[size_t(i)] = make(kRGBA16, ow, oh);
    }
    shading_change_ = make(kR8, hw, hh);
    farthest_depth_mip1_ = make(kR16, hw, hh);
    spd_levels_ = std::max(uint32_t(std::floor(std::log2(float(std::max(hw, hh))))) + 1, 6u);
    spd_mips_ = make(kRG16, hw, hh, spd_levels_);
    spd_atomic_ = make(kR32U, 1, 1);
    frame_info_ = make(kRGBA32, 1, 1);
    reactive_masks_ = make(kRGBA8, rw, rh);
    new_locks_ = make(kR8, ow, oh);
    output_ = make(kRGBA16, ow, oh);
    exposure_ = make(kRG32, 1, 1);        // 0: an exposure of 1
    no_reactivity_ = make(kR8, 1, 1);     // the reactive and transparency masks: none
}

void Fsr3::clear(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* t, glm::vec4 value, uint32_t levels) {
    for (uint32_t level = 0; level < levels; ++level) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = t;
        ct.mip_level = level;
        ct.clear_color = {value.r, value.g, value.b, value.a};
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr));
    }
}

void Fsr3::run(SDL_GPUCommandBuffer* cmd, Pass pass, const std::vector<SDL_GPUTexture*>& srvs, const std::vector<Texture>& uavs,
               const std::vector<std::pair<const void*, uint32_t>>& constants, uint32_t groups_x, uint32_t groups_y) {
    std::vector<SDL_GPUStorageTextureReadWriteBinding> rw(uavs.size());
    for (size_t i = 0; i < uavs.size(); ++i) rw[i].texture = uavs[i].texture, rw[i].mip_level = uavs[i].mip;
    SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cmd, rw.data(), uint32_t(rw.size()), nullptr, 0);
    SDL_BindGPUComputePipeline(cp, pipelines_[pass]);
    std::vector<SDL_GPUTextureSamplerBinding> tex;
    for (SDL_GPUTexture* t : srvs) tex.push_back({t, sampler_});
    if (!tex.empty()) SDL_BindGPUComputeSamplers(cp, 0, tex.data(), uint32_t(tex.size()));
    for (size_t i = 0; i < constants.size(); ++i)
        SDL_PushGPUComputeUniformData(cmd, uint32_t(i), constants[i].first, constants[i].second);
    SDL_DispatchGPUCompute(cp, std::max(groups_x, 1u), std::max(groups_y, 1u), 1);
    SDL_EndGPUComputePass(cp);
}

glm::vec2 Fsr3::next_jitter(uint32_t render_w, uint32_t output_w) {
    // ffxFsr3UpscalerGetJitterPhaseCount / GetJitterOffset: a Halton (2, 3) sequence, longer
    // the more the frame is upscaled
    const float ratio = float(output_w) / float(std::max(render_w, 1u));
    const int32_t phases = std::max(int32_t(8.0f * ratio * ratio), 1);
    const int32_t i = jitter_index_++ % phases;
    if (jitter_index_ >= phases * 64) jitter_index_ = 0;
    return {halton(i + 1, 2) - 0.5f, halton(i + 1, 3) - 0.5f};
}

SDL_GPUTexture* Fsr3::prepare(SDL_GPUCommandBuffer* cmd, const Frame& f) {
    const uint32_t rw = f.render_w, rh = f.render_h, ow = f.output_w, oh = f.output_h;
    bool reset = f.reset;
    if (rw != render_w_ || rh != render_h_ || ow != output_w_ || oh != output_h_ || owned_.empty()) {
        ensure_resources(rw, rh, ow, oh);
        reset = true;
    }
    // A pause in the frames (a menu, the upscaler off) or a jump of the camera is a cut too.
    const uint64_t now = SDL_GetTicksNS();
    const float dt = last_ticks_ ? float(now - last_ticks_) * 1e-9f : 1.0f;
    last_ticks_ = now;
    if (dt > 0.25f) reset = true;
    const glm::vec3 camera = glm::vec3(glm::inverse(f.view)[3]);
    if (glm::distance(camera, previous_camera_) > 10.0f) reset = true;
    previous_camera_ = camera;
    if (!cleared_) {
        for (SDL_GPUTexture* t : {exposure_, no_reactivity_, luma_[0], luma_[1], accumulation_[0], accumulation_[1], new_locks_})
            clear(cmd, t, glm::vec4(0));
        cleared_ = true;
        reset = true;
    }
    const glm::mat4 view_proj = f.proj * f.view;
    if (reset) previous_view_proj_ = view_proj, previous_jitter_ = f.jitter, previous_render_ = {rw, rh},
               previous_output_ = {ow, oh};
    reset_ = reset, dt_ = dt;
    reprojected_from_ = previous_view_proj_;

    // The camera's motion everywhere, and the depth as FSR reads it.
    {
        struct {
            glm::mat4 reproject;
            glm::vec4 size, jitter;
        } u;
        const glm::vec2 jitter_ndc(2.0f * f.jitter.x / float(rw), -2.0f * f.jitter.y / float(rh));
        const glm::mat4 jittered = glm::translate(glm::mat4(1), glm::vec3(jitter_ndc, 0)) * view_proj;
        u.reproject = previous_view_proj_ * glm::inverse(jittered);
        u.size = {float(rw), float(rh), 1.0f / float(rw), 1.0f / float(rh)};
        u.jitter = {jitter_ndc, 0, 0};
        run(cmd, MotionVectors, {f.depth}, {{motion_}, {depth_}}, {{&u, uint32_t(sizeof u)}}, groups(rw, 8), groups(rh, 8));
        previous_view_proj_ = view_proj;
    }
    return motion_;
}

SDL_GPUTexture* Fsr3::upscale(SDL_GPUCommandBuffer* cmd, const Frame& f) {
    const uint32_t rw = f.render_w, rh = f.render_h, ow = f.output_w, oh = f.output_h;
    const bool reset = reset_;
    const float dt = dt_;
    const size_t odd = frame_ & 1, even = 1 - odd;   // the SDK's resource pairs: read [odd], write [even]

    Constants c{};
    c.render_size[0] = int32_t(rw), c.render_size[1] = int32_t(rh);
    c.previous_render_size[0] = previous_render_.x, c.previous_render_size[1] = previous_render_.y;
    c.upscale_size[0] = int32_t(ow), c.upscale_size[1] = int32_t(oh);
    c.previous_upscale_size[0] = previous_output_.x, c.previous_upscale_size[1] = previous_output_.y;
    c.max_render_size[0] = int32_t(render_w_), c.max_render_size[1] = int32_t(render_h_);
    c.max_upscale_size[0] = int32_t(output_w_), c.max_upscale_size[1] = int32_t(output_h_);
    {   // setupDeviceDepthToViewSpaceDepthParams, reversed and finite
        const float at0 = f.proj[3][2] / f.proj[2][2], at1 = f.proj[3][2] / (f.proj[2][2] + 1.0f);   // distances at depth 0, 1
        const float lo = std::max(at0, at1), hi = std::min(at0, at1);   // (inverted: swapped)
        const float q = hi / (lo - hi);
        const float aspect = float(rw) / float(rh), cot_half_fov = f.proj[1][1];
        c.device_to_view_depth[0] = -q;
        c.device_to_view_depth[1] = q * lo;
        c.device_to_view_depth[2] = aspect / cot_half_fov;
        c.device_to_view_depth[3] = 1.0f / cot_half_fov;
        c.tan_half_fov = aspect / cot_half_fov;   // horizontal
    }
    c.jitter[0] = f.jitter.x, c.jitter[1] = f.jitter.y;
    c.previous_jitter[0] = previous_jitter_.x, c.previous_jitter[1] = previous_jitter_.y;
    c.motion_vector_scale[0] = c.motion_vector_scale[1] = 1;   // (already in UV units)
    c.downscale_factor[0] = float(rw) / float(ow), c.downscale_factor[1] = float(rh) / float(oh);
    const float ratio = float(ow) / float(rw);
    const float phases = float(int32_t(8.0f * ratio * ratio));
    if (reset || jitter_phases_ == 0) jitter_phases_ = phases;
    else if (phases > jitter_phases_) jitter_phases_ += 1;
    else if (phases < jitter_phases_) jitter_phases_ -= 1;
    c.jitter_phase_count = jitter_phases_;
    c.delta_time = std::clamp(dt, 0.0f, 1.0f);
    c.delta_pre_exposure = 1;
    c.view_space_to_meters = 1;
    frame_index_ = reset ? 0 : frame_index_ + 1;
    c.frame_index = frame_index_;
    c.velocity_factor = 1, c.reactiveness_scale = 1, c.shading_change_scale = 1;
    c.accumulation_added_per_frame = 1.0f / 3.0f, c.min_disocclusion_accumulation = -1.0f / 3.0f;

    if (reset) {
        clear(cmd, accumulation_[odd], glm::vec4(0));
        clear(cmd, frame_info_, {-1, 1, 0, 0});
    }
    clear(cmd, previous_nearest_depth_, glm::vec4(0));   // (reversed: the far plane)
    clear(cmd, spd_atomic_, glm::vec4(0));
    clear(cmd, spd_mips_, glm::vec4(0), spd_levels_);

    SpdConstants spd{};
    const uint32_t spd_x = (rw - 1) / 64 + 1, spd_y = (rh - 1) / 64 + 1;   // ffxSpdSetup over the render size
    spd.work_groups = spd_x * spd_y;
    spd.mips = uint32_t(std::min(std::floor(std::log2(float(std::max(rw, rh)))), 12.0f));
    spd.render_size[0] = rw, spd.render_size[1] = rh;
    const bool sharpen = f.sharpness > 0;
    struct {
        uint32_t config[4];
    } rcas{};
    rcas.config[0] = std::bit_cast<uint32_t>(std::exp2(-(2.0f - 2.0f * std::clamp(f.sharpness, 0.0f, 1.0f))));   // FsrRcasCon

    const std::pair<const void*, uint32_t> cb{&c, uint32_t(sizeof c)}, cb_spd{&spd, uint32_t(sizeof spd)},
        cb_rcas{&rcas, uint32_t(sizeof rcas)};
    auto spd_mips = [&](std::vector<Texture> v) {
        for (uint32_t m = 0; m < 6; ++m) v.push_back({spd_mips_, m});
        return v;
    };
    SDL_GPUTexture* luma = luma_[odd];   // this frame's (the SDK: odd frames LUMA_2)
    SDL_GPUTexture* previous_luma = luma_[even];
    const uint32_t gx = groups(rw, 8), gy = groups(rh, 8), ox = groups(ow, 8), oy = groups(oh, 8);
    run(cmd, PrepareInputs, {motion_, depth_, f.color},
        {{dilated_motion_}, {dilated_depth_}, {previous_nearest_depth_}, {intermediate_}, {luma}}, {cb}, gx, gy);
    std::vector<Texture> pyramid = spd_mips({{spd_atomic_}, {frame_info_}});
    pyramid.push_back({farthest_depth_mip1_});
    run(cmd, LumaPyramid, {luma, intermediate_}, pyramid, {cb, cb_spd}, spd_x, spd_y);
    run(cmd, ShadingChangePyramid, {luma, previous_luma, dilated_motion_, exposure_}, spd_mips({{spd_atomic_}}), {cb, cb_spd},
        spd_x, spd_y);
    run(cmd, ShadingChange, {spd_mips_}, {{shading_change_}}, {cb}, groups(rw / 2, 8), groups(rh / 2, 8));
    run(cmd, PrepareReactivity,
        {previous_nearest_depth_, dilated_motion_, dilated_depth_, no_reactivity_, no_reactivity_, accumulation_[odd],
         shading_change_, luma, exposure_},
        {{reactive_masks_}, {new_locks_}, {accumulation_[even]}}, {cb}, gx, gy);
    run(cmd, LumaInstability,
        {exposure_, reactive_masks_, dilated_motion_, frame_info_, luma_history_[odd], farthest_depth_mip1_, luma},
        {{luma_history_[even]}, {intermediate_}}, {cb}, gx, gy);
    run(cmd, sharpen ? AccumulateSharpen : Accumulate,
        {exposure_, reactive_masks_, dilated_motion_, upscaled_[odd], no_reactivity_, farthest_depth_mip1_, luma, intermediate_,
         f.color},
        {{upscaled_[even]}, {output_}, {new_locks_}}, {cb}, ox, oy);
    if (sharpen) run(cmd, Rcas, {exposure_, upscaled_[even]}, {{output_}}, {cb, cb_rcas}, groups(ow, 16), groups(oh, 16));

    if (std::getenv("TDKR_FSR3_DEBUG"))   // FSR's own view of its inputs and masks
        run(cmd, DebugView, {reactive_masks_, dilated_motion_, dilated_depth_, upscaled_[even], exposure_}, {{output_}}, {cb}, ox, oy);
    previous_jitter_ = f.jitter;
    previous_render_ = {rw, rh}, previous_output_ = {ow, oh};
    frame_ = (frame_ + 1) % 16;
    return output_;
}

}  // namespace tdkr
