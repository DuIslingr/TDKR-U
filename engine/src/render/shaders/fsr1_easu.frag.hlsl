// AMD FSR 1 EASU (third_party/fidelityfx, MIT): the post-processed frame at render resolution
// upscaled to the output's. Both images upright; the pass runs per output pixel.
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HALF 0
#define FFX_FSR_EASU_FLOAT 1
#include "ffx_core.h"

[[vk::binding(0, 2)]] Texture2D<float4> source;
[[vk::binding(0, 2)]] SamplerState source_sampler;   // (one combined image sampler: -fauto-combined-image-sampler)

[[vk::binding(0, 3)]] cbuffer EasuConstants {
    uint4 con0, con1, con2, con3;   // ffxFsrPopulateEasuConstants
};

FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p) { return source.GatherRed(source_sampler, p); }
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p) { return source.GatherGreen(source_sampler, p); }
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p) { return source.GatherBlue(source_sampler, p); }

#include "fsr1/ffx_fsr1.h"

float4 main(float4 position : SV_Position) : SV_Target {
    FfxFloat32x3 color;
    ffxFsrEasuFloat(color, FfxUInt32x2(position.xy), con0, con1, con2, con3);
    return float4(color, 1);
}
