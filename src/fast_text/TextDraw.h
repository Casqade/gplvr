#pragma once

// The GL side of the fast text path (docs/audit-2026-09-18/71-fast-text.md).
//
// roglv2 draws GPL's stamped text one glyph at a time, and for a destination that is the GL
// framebuffer the blit is `glRasterPos2i` + `glDrawPixels` of an 8-bit colour-index image with
// the four GL_PIXEL_MAP_I_TO_* tables rebuilt per glyph (rogl_blitSysToGl_drawPixels 0x20008C30,
// rogl_glSetPixelMapFromPalette 0x2000A8F0). That path costs about 103 us a glyph on this
// driver.
//
// This draws the same pixels as quads:
//   - every glyph bitmap is uploaded once into one R8 atlas, in its own byte values, so a texel
//     still holds roglv2's colour index and the shader takes the top nibble the way
//     GL_INDEX_SHIFT = -4 does;
//   - the 16-entry palette roglv2 would have built for the stamp set's colour is a row of a
//     small RGBA8 texture, so the fragment colour is the byte roglv2's pixel map would have
//     produced;
//   - a run of glyphs is one glDrawArrays.
//
// Everything runs on the render thread with roglv2's context current, between two of GPL's own
// 2D calls. Nothing is read back from GL while drawing: roglv2's bindings are the known values
// (no program, vertex array 0, no buffer on any target, texture unit 0 active) and are put back
// after every burst, and the screen transform is rebuilt from roglv2's display object. Texture
// units 2 and 3 are ours; roglv2 never calls glActiveTexture.

#include "GlApi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gplvr {
namespace fasttext {

// One glyph, in roglv2's 2D object space and in atlas texels: the rectangle glDrawPixels would
// have covered.
//
// The two y fields are named for the atlas rows they carry, not for up and down, because which
// of them is the higher object-space y depends on roglv2's pixel zoom -- and reading them as
// "top" and "bottom" is what put the first build's glyphs in upside down (report 71c).
// FastText.cpp derives both from the zoom factor it reads out of roglv2.
struct Quad {
    float    x0 = 0.0f;
    float    x1 = 0.0f;
    float    yV0 = 0.0f;  // object-space y of atlas row v0, the glyph's first source row
    float    yV1 = 0.0f;  // object-space y of atlas row v1, one past its last source row
    float    u0 = 0.0f;
    float    v0 = 0.0f;
    float    u1 = 0.0f;
    float    v1 = 0.0f;
    uint32_t palette = 0;  // row of the palette texture
};

struct DrawStats {
    uint32_t draws = 0;
    uint32_t quads = 0;
    uint32_t glyphUploads = 0;
    uint32_t glyphBytes = 0;
    uint32_t paletteUploads = 0;
    uint32_t vertexBytes = 0;
    int64_t  drawTicks = 0;
    int64_t  uploadTicks = 0;
};

// The rows of the atlas are packed on shelves: a glyph is a few pixels tall and the whole of
// GPL's font set is a few thousand texels, so one 1024x1024 R8 texture holds every set of a
// session with room to spare. `atlasFull` is counted and the call falls back to roglv2.
constexpr int kAtlasSize = 1024;
constexpr int kMaxPaletteRows = 64;
constexpr int kMaxGlyphSide = 256;

class TextDraw {
public:
    // Compiles the program and creates the atlas, palette and vertex buffer for the context
    // current now. The only GL reads are the version and the unpack alignment, here.
    bool init(const GlApi& gl, std::string& error);
    // `contextAlive` false when the context is gone: nothing is deleted, only forgotten.
    void release(bool contextAlive);
    bool ready() const { return ready_; }

    // Upload one glyph image, 8 bits a pixel, `pitch` bytes a row, into the atlas. False when
    // the atlas has no room left.
    bool addGlyph(const uint8_t* pixels, int width, int height, int pitch, int& atlasX,
                  int& atlasY, DrawStats& stats);
    // Everything in the atlas is forgotten: the next glyph starts a new packing. The caller
    // drops its own glyph table at the same time.
    void resetAtlas();

    // The row holding this 16-entry palette, adding it when it is new. -1 when every row is
    // taken; the caller draws what it has, calls resetPalettes and asks again.
    int paletteRow(const uint32_t entries[16]);
    // Forget the rows. Only safe once the quads that referred to them have been drawn.
    void resetPalettes();

    void append(const Quad& quad);
    size_t pending() const { return quads_.size(); }

    // Draw the pending quads under `screen` (roglv2's projection x modelview) and empty them.
    // Blend, alpha test, depth and the colour mask are left exactly as roglv2 set them, which
    // is what glDrawPixels would have been subject to.
    void draw(const float screen[16], DrawStats& stats);

    int atlasShelfTop() const { return shelfY_ + shelfHeight_; }

private:
    struct Vertex {
        float    x;
        float    y;
        float    u;
        float    v;
        uint32_t palette;
    };

    GlApi gl_;
    bool         ready_ = false;

    GLuint program_ = 0;
    GLint  uScreen_ = -1;
    GLint  uAtlas_ = -1;
    GLint  uPalette_ = -1;
    GLuint vertexArray_ = 0;
    GLuint vertexBuffer_ = 0;
    GLuint atlas_ = 0;
    GLuint palette_ = 0;
    size_t        vertexCapacity_ = 0;  // bytes
    GLint  unpackAlignment_ = 4;

    // Shelf packing: a shelf is as tall as its tallest glyph so far.
    int shelfX_ = 0;
    int shelfY_ = 0;
    int shelfHeight_ = 0;

    std::vector<Quad>     quads_;
    std::vector<Vertex>   vertices_;
    std::vector<uint32_t> paletteRows_;  // 16 entries a row, as the texture holds them
    bool                  paletteDirty_ = false;
};

}  // namespace fasttext
}  // namespace gplvr
