#pragma once

// What Renderer.cpp and RendererMerged.cpp share: texture units and the constants of the decal,
// overlay and soft-edge passes. Not part of the renderer's interface.

#include "baked/GlApi.h"
#include "baked/Mip.h"

#include <cstdint>

namespace baked {

// The order number a host's SolidHook writes where it draws (the cars). The order test of the
// walk-order phase compares a fragment over such a pixel by depth alone: cars move, so GPL's
// painter order between a car and the scenery is a per-frame distance sort that real depth
// reproduces. Item order numbers are a pass's visible count, far below it.
constexpr uint32_t kHookOrder = 0x80000000u;

// GPL's per-vertex lighting as GLSL functions, for any vertex shader that lights GPL's lit
// polygon types (0x1D 0x1E 0x21 0x22; the cars now, the track's lit items next). From
// gpl_normalLightCached 0x4CFA70 (report 103 section 4, IDA task "R2 stage 2b"): the corner's
// colour is its shade (0x1E / 0x22) times I = d < 0 ? ka : ka + kd d, d = N.L; the specular
// S = s ks / (s + n - s^2 n), s = N.H, 0 for s < 0, is GL's secondary colour (raster mode 7),
// drawn only while gpl_specularOn is set and n > 0. N, L and H in one frame, unit. `lighting`:
// x lighting on, y specular on, z ka, w kd. GPL's light colour never tints (its white flag
// 0x56FE60 is 1 and nothing clears it).
constexpr const char* kGplLightingGlsl = R"(
float gplDiffuse(vec3 normal, vec3 light, vec4 lighting) {
    float d = dot(normal, light);
    return d * lighting.w < 0.0 ? lighting.z : lighting.z + lighting.w * d;
}
float gplSpecular(vec3 normal, vec3 halfVector, vec4 lighting, float exponent, float factor) {
    float s = dot(normal, halfVector);
    if (lighting.y < 0.5 || exponent <= 0.0 || s < 0.0) return 0.0;
    return s * factor / (s + exponent - s * s * exponent);
}
)";

struct TextureFormat {
    GLenum internal;
    GLenum format;
    GLenum type;
    int    bytesPerTexel;
};

// The GL upload format of a .mip's levels, as they lie in MipImage.
inline TextureFormat textureFormatFor(const MipImage& image) {
    if (image.indexed) return {gl::kR8, gl::kRed, gl::kUnsignedByte, 1};
    if (image.expanded) return {gl::kRgba8, gl::kBgra, gl::kUnsignedByte, 4};
    switch (image.pixfmt) {
    case 3: return {gl::kRgb5, gl::kRgb, gl::kUnsignedShort565, 2};
    case 4: return {gl::kRgb5A1, gl::kBgra, gl::kUnsignedShort1555Rev, 2};
    case 5: return {gl::kRgba4, gl::kBgra, gl::kUnsignedShort4444Rev, 2};
    case 6: return {gl::kRgb8, gl::kBgra, gl::kUnsignedByte, 4};
    default: return {gl::kRgba8, gl::kBgra, gl::kUnsignedByte, 4};
    }
}

constexpr GLenum kTextureUnit = gl::kTexture0 + 1;
// The order test's inputs: the solid surfaces' order and depth, and the per-item order table.
constexpr GLenum kSolidOrderUnit = gl::kTexture0 + 2;
constexpr GLenum kSolidDepthUnit = gl::kTexture0 + 3;
constexpr GLenum kItemOrderUnit = gl::kTexture0 + 4;
// The merged program's per-batch table (Renderer::buildBatchTable).
constexpr GLenum kBatchTableUnit = gl::kTexture0 + 5;
// The UV checker of debug view 8 (Renderer::uvCheckerTexture), for every program.
constexpr GLenum kCheckerUnit = gl::kTexture0 + 8;
constexpr uint32_t kNoItem = 0xFFFFFFFFu;

// Overlays (report 59). An overlay is drawn after the base surfaces, level by level, each level's
// depth written just before its colour and offset towards the eye, so of two overlays on the same
// ground the later level wins as in GPL, and one on other ground stays behind whatever is nearer
// (report 105 section 4.10). It meets its road the same way a decal does, through its own
// per-vertex gap (report 63).
//
// The normal lift is kept as a knob at zero: glPolygonOffset's `units` is implementation-defined
// for a float depth buffer, so if a coplanar decal is ever seen fighting its base, a millimetre
// or two along the polygon normal is the fallback. It costs 2 mm x distance / eye height of
// screen shift, 8 mm at 3 m ahead from a 0.8 m cockpit eye.
constexpr float kOverlayLift = 0.0f;
// glPolygonOffset(1, 1), sign flipped for reversed-Z (nearer is larger there): the tie-breaker
// between a decal and the road it lies on. In lift mode the two are coplanar to float precision
// -- 0.04 mm at spa67, 0.13 mm at Isle of Man's 10 km coordinates -- and one pixel of depth slope
// is more than that beyond a few centimetres from the eye.
constexpr float kOverlayOffsetFactor = 1.0f;
constexpr float kOverlayOffsetUnits = 1.0f;

// Decals and overlays (docs/audit-2026-09-18/63-per-vertex-decal-gap.md). The track compiler
// sinks nothing: a decal's offset from its road is the road piece's own flatness error, signed
// and bounded, 0 where the two share a vertex. The bake cuts each decal along the road triangles
// under it and stores the exact vertical gap per vertex (Vertex::gap), and both modes use it:
//
// lift -- each decal vertex under its road is raised onto it; one above it stays put. The decal
//   then lies on the road it is painted on and wins by glPolygonOffset alone. The cost is the one
//   report 61 measured: a raise `h` moves the decal `h x distance / eye height` along the ground
//   on screen against GPL, 0.48 m at 10 m for the worst start-grid corner (38.5 mm).
// bias -- the decal stays where the author put it and its depth is moved onto the road plane
//   along each view ray, exactly (see the vertex shader), plus kDecalBiasMargin. It can be hidden
//   only where the ray meets the road on a neighbouring road triangle before its own: 0.4 % of
//   rays over a buried piece at 15 m in the spa67 model, none at Isle of Man.
//
// Margin of the bias mode, metres of view depth: the decal can show through an occluder only
// where the occluder is this close in front of the road. The spa67 model has 11 of 2,604 rays at
// 15 m that need more than 2 cm and 9 that need more than 5 cm, so a larger margin buys nothing.
constexpr float kDecalBiasMargin = 0.02f;

// The alpha at and above which a cutout or sprite fragment is solid: it writes depth in the base
// pass. Everything below is its soft edge, drawn blended and without depth after everything else
// (report 67c). A depth-writing texel that is partly transparent blends with whatever the buffer
// holds when it is drawn and then hides what comes later behind it, a halo; a 1-bit texture's
// filtered alpha is 1 exactly away from its edge, so the core loses nothing.
constexpr float kCoreAlpha = 0.99f;

// Vertex tag bits above batch | level << 24 (Renderer::upload): a face of a two-sided panel,
// drawn by the merged program only while it faces the eye, and whether its face points the other
// way from its stored normal, which faces up (Scene::oneSided, report 105 section 4.17).
constexpr uint32_t kTagOneSided = 0x10000000u;
constexpr uint32_t kTagFlipped = 0x20000000u;

}  // namespace baked
