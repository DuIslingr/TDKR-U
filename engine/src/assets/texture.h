// Texture decoding: PVR v2 containers (TDKR names them .tga) holding PVRTC 2/4bpp, ETC1, RGBA8888, RGB565 or I8,
// and the .etc variant that zips an ETC1 colour texture with a separate alpha texture.
// Ported from tools/textures.py (which follows PowerVR's reference PVRTDecompress).
#pragma once

#include <cstdint>
#include <vector>

#include "core/bytes.h"

namespace tdkr {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;      // width * height * 4, first row first
    bool has_alpha() const;
};

// Decode the top mip level of a PVR v2 file.
Image decode_pvr2(const Bytes& data);

// Decode a truecolor TGA (uncompressed or RLE, 24/32 bpp).
Image decode_tga(const Bytes& data);

// Decode a .pvr / .pvr2 / .etc texture file (an .etc may be a ZIP of rgb.pvr + alpha.pvr),
// or a .tga: PVR v2 in TDKR, except a few real Targa files.
Image load_texture(const Bytes& data);

}  // namespace tdkr
