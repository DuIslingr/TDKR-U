// AMD FSR 1 RCAS (third_party/fidelityfx, MIT): robust contrast-adaptive sharpening of the
// upscaled frame (or, at native resolution, of the frame itself), per output pixel.
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HALF 0
#define FSR_RCAS_F 1
#include "ffx_core.h"

[[vk::binding(0, 2)]] Texture2D<float4> source;
[[vk::binding(0, 2)]] SamplerState source_sampler;

[[vk::binding(0, 3)]] cbuffer RcasConstants {
    uint4 con;   // FsrRcasCon: x the sharpness as exp2(-stops)
};

FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p) { return source.Load(FfxInt32x3(p, 0)); }
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

#include "fsr1/ffx_fsr1.h"

float4 main(float4 position : SV_Position) : SV_Target {
    FfxFloat32 r, g, b;
    FsrRcasF(r, g, b, FfxUInt32x2(position.xy), con);
    return float4(r, g, b, 1);
}
