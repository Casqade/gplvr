#pragma once

// N GLASSES, N REFLECTIONS, ONE mirror.mip -- BY MOVING THE UVs INSTEAD OF MAKING TEXTURES.
//
// WHY NOT A TEXTURE PER GLASS. It is the obvious answer and it is not available: a bitmap's
// class is fixed when it is constructed, `bitmapAlloc` only ever builds a system-memory one, and
// the GL texture name at root+84 is written solely by `BitmapOpenGLTexture`'s constructor from a
// `TextureOpenGL` handed to it. So a mip gplvr creates can never be a texture roglv2 will draw
// with -- it binds texture 0 and the glass comes out black. That was established in the roglv2
// IDB after four test rounds, and was built and removed as `mirrorTextures`;
// docs/audit-2026-09-18/87-three-mirrors.md has the record and the IDB comment on bitmapAlloc
// (0x20001660) the addresses.
//
// WHAT IS ACTUALLY IN THE WAY is narrower than it looked. mirror.mip is one 512x512 texture and
// the glasses' AUTHORED UVs overlap on it -- the 330P4 reads u 0.268-1.000, 0.000-0.732 and
// 0.001-0.999, so texels 137..375 are read by all three and whatever is painted there is seen
// by every one of them. The overlap is a property of the UVs, not of the texture, and the UVs
// are data in the model's polygon nodes -- 2n floats at polyNode+16, after the four header
// words, which is where addPoly reads them from.
//
// So each glass is given a DISJOINT region of mirror.mip by rewriting its UVs, and one pane is
// rendered into each region:
//
//     [0] left   108x109 mm  u 0.268-1.000  ->  u 0.000-0.496 v 0.000-0.654   254x335 texels
//     [1] right  110x107 mm  u 0.000-0.732  ->  u 0.496-1.000 v 0.000-0.654   258x335
//     [2] centre 103x29 mm   u 0.001-0.999  ->  u 0.000-1.000 v 0.654-1.000   512x177
//
// Each region is shaped to its glass: every glass gets the same area at its own aspect, packed
// onto shelves and then stretched to tile the texture. The shape decides sharpness only --
// aimPane writes the compensating aspect into the camera, so the geometry is right in any rect.
//
// Every mechanism this uses is already running: mirror.mip HAS a GL texture, the copy-out lands
// in it and the glasses sample it -- the split reflection everyone has been looking at is that
// pipeline working. Nothing here creates a texture, so none of the questions that killed the
// other route apply.
//
// WITHIN a glass nothing moves. The remap is linear over the whole authored range, so the
// relative layout of a wing's 19 facets is preserved exactly and the reflection maps onto the
// glass as it was authored -- only which part of the texture it reads changes.
//
// WHAT IT COSTS. Resolution: three glasses share one mirror.mip, so on the 330P4 each wing gets
// about half the texture's width. `RasterV2:MirrorSize` sets mirror.mip's size and the pane code
// reads that size from the camera rather than assuming it, so a larger mirror.mip buys it back.
//
// THE MODEL IS WRITTEN TO, SO IT IS RESTORED. The authored UVs are saved before the first
// rewrite and put back on release, since a model reload can move or replace these nodes. The
// cockpit-pointer check that catches a reload is `mirrorGeometryCockpit`.
//
// When this refuses a car -- its geometry was not derived, or a glass cannot be moved whole --
// the panes fall back to the two halves of mirror.mip and the car's own UVs, which is how every
// car was drawn before.

#include "MirrorGeometry.h"

namespace gplvr {

// Where one glass's reflection is written in mirror.mip, in texels. Rendered into by a pane
// camera of exactly this size, copied out to (x, y).
struct MirrorTextureRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// The surfaces to lay out, from the geometry walk. Null forgets them and restores any UVs that
// were rewritten. Call it when the team changes.
void mirrorUvSetSurfaces(int team, const MirrorSurfaceSet* set);

// Once a frame from the mirror path, with mirror.mip's own size. Applies the layout on the first
// call and afterwards verifies the UVs still hold what was written -- a model reload puts the
// authored values back, which is indistinguishable from never having applied them.
void mirrorUvEnsure(int baseWidth, int baseHeight);

// Put the authored UVs back and forget the layout. Safe to call when nothing was applied.
void mirrorUvRelease();

// How many glasses have a region, or 0 when the layout is not in force. `planePanes` renders one
// pane per region, each copied into mirror.mip itself -- so the copy-out destination stays the
// bitmap GPL passes and no new texture is involved anywhere.
int  mirrorUvCount();
bool mirrorUvRect(int index, MirrorTextureRect& out);

// Regions given from outside instead of the layout's: one per derived glass in the order the
// geometry walk derived them, in the UVs this writes (v up), overlapping or not -- the shared view
// (MirrorShared.h) gives every glass its part of one picture. Taking a set up puts the authored
// UVs back and the next mirrorUvEnsure applies it; clearing goes back to the layout the same way.
struct MirrorUvRegion {
    float u0, u1, v0, v1;
};
void mirrorUvSetRegions(const MirrorUvRegion regions[], int count);
void mirrorUvClearRegions();

// Rewrites the glasses' UVs onto `regions` at once, from the authored values kept at apply: the
// shared view moves them before each eye's pass, since each eye's picture is laid out from that
// eye. False when a set of `count` is not applied.
bool mirrorUvMoveRegions(const MirrorUvRegion regions[], int count);

}  // namespace gplvr
