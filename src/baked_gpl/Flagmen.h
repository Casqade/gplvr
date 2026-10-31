#pragma once

// The flagmen and the starter in the baked main passes (docs/audit-2026-09-18/105-baked-renderer-
// rewrite.md sections 4.11 and 4.13). GPL's track loader (gpl_trackScanBindFlagmen 0x481E50)
// replaces the child of every XFORM above a `flagger` or `starter` object with a MATERIAL of its
// own whose word 3 is a colormap; every frame gpl_flagmanBindModel 0x481910 sets that colormap's
// 16 colours and points the MATERIAL's word 1 at one BILLBOARD of a frame object (waving.3do and
// its siblings, loaded through the resource cache) or at gpl_nullNode. Each frame is one 64x64
// sprite of 4-bit palette indices with no colormap of its own, so roglv2 colours it from the
// flagman's.
//
// gplvr takes the flagman at its XFORM, which the track bake leaves to GPL (Scene.cpp): the node
// filter hands each such dispatch to flagmanDispatch, which reads the MATERIAL live, finds the
// BILLBOARD's baked leaf through the car bake (CarBake.h; the sprite comes from objs\objs.dat),
// takes the 16 colours GPL last gave the colormap -- recorded at the five colormapSetColor calls
// of gpl_flagmanBindModel, which this module wraps -- and records a one-sprite CarDrawList with
// the XFORM's pose and the walk's place. CarRenderer paints it in the walk-order phase there, the
// indices looked up in that palette. Anything it cannot resolve is left to GPL, counted.

#include <cstddef>
#include <cstdint>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

// From bakedPassBind, after carWalkInstall. Does nothing unless bakedRenderer, bakedFlagmen and
// bakedCars = 3; logs what it installed or why not.
void flagmenInstall();

// bakedPassBegin opens a new pass; bakedPassEnd reads its lists, in the order GPL's walk reached
// the flagmen.
void flagmenPassBegin();
const baked::CarDrawList* flagmenPassLists(uint32_t& count);

// From the node filter while a baked pass (main or mirror) is collected, for an XFORM inside the
// track's PRIM that is no baked item. True when the dispatch was taken (gplvr draws the flagman,
// or it draws nothing, or debug view 12 had GPL draw it already): the filter then skips GPL's
// handler. False for any other XFORM, and for a flagman left to GPL.
bool flagmanDispatch(const unsigned char* node, void* stock);

// Debug view 12: GPL draws every flagman, and each draw is compared with what gplvr would have
// drawn, against gpl_statGfxPolysDrawn.
void flagmenSetGplDraws(bool gplDraws);

// Once a second: the per-second line, without its "baked flagmen: " prefix. False when there is
// nothing to say. Resets the counters.
bool flagmenFormat(char* out, size_t size, unsigned passes);

}  // namespace gplvr
