#pragma once

// .srb sprites (report 67): gpl_spriteLoadSrb 0x4D63D0 reads an `SRB ` chunk holding SRHD (six
// floats: world width, world height, texture width and height in pixels, anchor column and row)
// and SRH2 (36 bytes, byte 0 = the eye-facing flag), then the embedded MIP. The SRHD handler
// 0x4D6230 keeps the anchor as fractions of the size, which is what the quad corners are made of.
// Shared by the track bake (Scene.cpp) and the car bake (CarScene.cpp).

#include "baked/Mip.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace baked {

struct SrbSprite {
    float width = 0.0f;     // world, used when the node gives no size
    float height = 0.0f;
    float leftNear = 0.0f;  // anchor column / pixel width - 1: the corner on the right as seen
    float upLow = 0.0f;     // anchor row / pixel height - 1: the bottom corner
    float aspect = 1.0f;    // texture width / height, the width of a node-sized sprite
    bool  facing = false;   // SRH2 byte 0: gpl_spriteDrawScreenAligned 0x4D69F0
};

// Parses a whole .srb into its header and its embedded texture. False with a reason for a file
// the sprite cannot be drawn from. `keepIndices` as parseMip's.
bool parseSrb(const uint8_t* data, size_t size, SrbSprite& out, MipImage& image,
              std::string& error, bool keepIndices = false);

}  // namespace baked
