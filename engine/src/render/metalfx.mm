// MetalFX on macOS (TDKR_METALFX: the patched SDL's Metal objects, the MetalFX framework).
#include "render/metalfx.h"

#if defined(__APPLE__) && defined(TDKR_METALFX)

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <cstdio>

namespace tdkr {

struct MetalFx::State {
    id<MTLDevice> device = nil;
    id scaler = nil;   // id<MTLFXTemporalScaler> (macOS 13)
    uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
    SDL_GPUTexture* output = nullptr;
};

namespace {

id<MTLTexture> native_texture(SDL_GPUDevice* device, SDL_GPUTexture* t, MTLPixelFormat* format) {
    void* image = nullptr;
    Uint32 f = 0;
    if (!t || !SDL_GetGPUNativeTexture(device, t, &image, nullptr, &f)) return nil;
    if (format) *format = MTLPixelFormat(f);
    return (__bridge id<MTLTexture>)image;
}

}  // namespace

MetalFx::MetalFx(SDL_GPUDevice* device) : device_(device) {
    if (@available(macOS 13.0, *)) {
        const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device_);
        void* native = SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_METAL_DEVICE_POINTER, nullptr);
        if (!native) {
            why_ = "METAL ONLY";
            return;
        }
        id<MTLDevice> mtl = (__bridge id<MTLDevice>)native;
        if (![MTLFXTemporalScalerDescriptor supportsDevice:mtl]) {
            why_ = "NOT SUPPORTED ON THIS GPU";
            return;
        }
        state_ = std::make_unique<State>();
        state_->device = mtl;
        ok_ = true;
    } else {
        why_ = "MACOS 13 OR LATER";
    }
}

MetalFx::~MetalFx() {
    if (!state_) return;
    SDL_WaitForGPUIdle(device_);
    if (state_->output) SDL_ReleaseGPUTexture(device_, state_->output);
    state_->scaler = nil;
}

SDL_GPUTexture* MetalFx::upscale(SDL_GPUCommandBuffer* cmd, const TemporalUpscale& f, const TemporalInputs& in) {
    if (!ok_) return nullptr;
    if (@available(macOS 13.0, *)) {
        State& s = *state_;
        id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)SDL_GetGPUNativeCommandBuffer(cmd);
        MTLPixelFormat color_format = MTLPixelFormatInvalid, depth_format = MTLPixelFormatInvalid,
                       motion_format = MTLPixelFormatInvalid;
        id<MTLTexture> color = native_texture(device_, f.color, &color_format);
        id<MTLTexture> depth = native_texture(device_, f.scene_depth, &depth_format);
        id<MTLTexture> motion = native_texture(device_, in.motion(), &motion_format);
        if (!buffer || !color || !depth || !motion) return nullptr;

        bool reset = in.reset();
        if (!s.scaler || s.render_w != f.render_w || s.render_h != f.render_h || s.output_w != f.output_w ||
            s.output_h != f.output_h) {
            if (s.output) SDL_ReleaseGPUTexture(device_, s.output), s.output = nullptr;
            SDL_GPUTextureCreateInfo ti{};   // (shader read, render target and shader write: what the scaler asks)
            ti.type = SDL_GPU_TEXTURETYPE_2D;
            ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            ti.width = f.output_w, ti.height = f.output_h;
            ti.layer_count_or_depth = 1, ti.num_levels = 1;
            s.output = SDL_CreateGPUTexture(device_, &ti);
            if (!s.output) return nullptr;
            MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
            d.inputWidth = f.render_w, d.inputHeight = f.render_h;
            d.outputWidth = f.output_w, d.outputHeight = f.output_h;
            d.colorTextureFormat = color_format;
            d.depthTextureFormat = depth_format;
            d.motionTextureFormat = motion_format;
            d.outputTextureFormat = MTLPixelFormatRGBA16Float;
            d.autoExposureEnabled = NO;   // (an LDR frame)
            s.scaler = [d newTemporalScalerWithDevice:s.device];
            if (!s.scaler) {
                std::fprintf(stderr, "metalfx: no temporal scaler for %ux%u -> %ux%u\n", f.render_w, f.render_h, f.output_w, f.output_h);
                return nullptr;
            }
            s.render_w = f.render_w, s.render_h = f.render_h, s.output_w = f.output_w, s.output_h = f.output_h;
            reset = true;
        }
        id<MTLFXTemporalScaler> scaler = s.scaler;
        scaler.colorTexture = color;
        scaler.depthTexture = depth;
        scaler.motionTexture = motion;
        scaler.outputTexture = native_texture(device_, s.output, nullptr);
        scaler.inputContentWidth = f.render_w;
        scaler.inputContentHeight = f.render_h;
        const glm::vec2 jitter = in.jitter();   // (render pixels, the motion vectors' axes)
        scaler.jitterOffsetX = jitter.x;
        scaler.jitterOffsetY = jitter.y;
        scaler.motionVectorScaleX = float(f.render_w);   // UV units to pixels
        scaler.motionVectorScaleY = float(f.render_h);
        scaler.depthReversed = YES;
        scaler.reset = reset;
        SDL_GPUTexture* const read[] = {f.color, f.scene_depth, in.motion()};
        SDL_GPUTexture* const write[] = {s.output};
        SDL_BeginGPUNativeAccess(cmd, read, 3, write, 1);
        [scaler encodeToCommandBuffer:buffer];
        SDL_EndGPUNativeAccess(cmd, read, 3, write, 1);
        return s.output;
    }
    return nullptr;
}

}  // namespace tdkr

#endif
