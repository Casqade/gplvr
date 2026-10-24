#pragma once

// .mip textures (docs/audit-2026-09-09/28-dat-mip-and-sidecars.md section 3). The 16- and
// 32-bit levels are kept in their on-disk layout, which OpenGL takes as-is; the 4-bit palette
// levels are expanded to BGRA8 here, since GL has no palette formats.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace baked {

struct MipLevel {
    int                  width = 0;
    int                  height = 0;
    std::vector<uint8_t> data;  // top row first
};

struct MipImage {
    // pixfmt = 3 * bppClass + blendClass: 0-2 palette 4-bit, 3 RGB565, 4 ARGB1555, 5 ARGB4444,
    // 6 BGRX8888, 7 BGRA8888. blendClass 0 opaque, 1 colour key / 1-bit, 2 alpha.
    int pixfmt = -1;
    // MPHD average colour and coverage, B, G, R, coverage (255 = no transparent texel).
    uint8_t average[4] = {128, 128, 128, 255};
    // MPHD +0x0D: bit 0 clamps s, bit 1 clamps t (GL_CLAMP_TO_EDGE), else GL_REPEAT. roglv2's
    // mip loader 0x20017BD0 passes (flags & 1, flags & 2) to the texture init 0x200091C0 as the
    // wrap modes (report 57, ordering and decals).
    uint8_t flags = 0;
    // True when the palette levels were expanded, so `data` is BGRA8 whatever pixfmt says.
    bool expanded = false;
    std::vector<MipLevel> levels;

    int  blendClass() const { return pixfmt % 3; }
    bool translucent() const { return pixfmt == 2 || pixfmt == 5 || pixfmt == 7; }
    bool cutout() const { return pixfmt == 1 || pixfmt == 4; }
    size_t bytes() const;
};

bool parseMip(const uint8_t* data, size_t size, MipImage& out, std::string& error);

}  // namespace baked
