#pragma once

// Baked cars (docs/audit-2026-09-18/105-baked-renderer-rewrite.md sections 4-4.3): GPL draws every
// car through one vtable slot, gpl_carRecordDraw 0x486E50 (report 103 section 11), which goes
// through gplvr. Each call poses the car as GPL does; in a baked main or mirror pass the car's first
// call walks the posed node graph in GPL's memory with GPL's own decisions -- gates against the eye
// in each frame, SETREG into a copy of the pass's register file, LOD and register selectors, the
// class-visible mask of each MATERIAL, BOXCULL and SPHERECULL against the frustum -- into ONE draw
// list per car per pass, from objects CarBake.h bakes out of GPL's memory, merged into ranges under
// one frame (the poses from the car's root) and one MATERIAL state each. The walk runs without the
// cell's clip planes (a car in N cells is called N times), and its list is kept in a per-pass arena
// that CarRenderer draws at the end of the pass. GPL's draw of the car still runs on every call, but
// its body dispatch (the call at 0x48703F) is skipped, so only the pose, the helmet tint and the
// blob shadow remain GPL's. Any car call outside a baked pass of its kind goes to GPL whole.

#include <cstdint>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

// Called once, on the render thread before a main walk, when the baked renderer is on. Every check
// comes before the first patch; logs what it installed or why not (GPL then draws every car).
void carWalkInstall();

// bakedPassBegin opens a new pass's arena (`mirror`: a mirror pass); bakedPassEnd reads its lists,
// one per car drawn by gplvr, in the order GPL first called each car. In VR the first eye's lists
// are kept for the second eye's pass of the same kind (CarShare.h), and a car's kept list
// (CarProgram.h) stands in for its walk where it holds. `pane`: a mirror pass's glass
// (stereoMirrorPaneOf, -1 not known), which keys its cars' kept lists (report 105 section 4.54).
void carWalkPassBegin(bool mirror, int pane);
const baked::CarDrawList* carWalkPassLists(uint32_t& count);

}  // namespace gplvr
