#pragma once

// Baked cars, R2 stage 1 (docs/audit-2026-09-18/105-baked-renderer-rewrite.md section 4): the car
// control walk and its contract test. GPL draws every car through one vtable slot,
// gpl_carRecordDraw 0x486E50 (report 103 section 11). With bakedCars = 1 that slot goes through
// gplvr: each call poses the car as GPL does, walks the posed node graph in GPL's memory with
// GPL's own decisions -- gates against the eye in each frame, SETREG into a copy of the pass's
// register file, LOD and register selectors, the class-visible mask of each MATERIAL, BOXCULL and
// SPHERECULL against the frustum and the cell's clip planes -- and counts the polygons it would
// draw. Then GPL draws the car, and the walk's count is checked against what GPL's own polygon
// counter (gpl_statGfxPolysDrawn, one per polygon emitted) moved by, minus the blob shadow, which
// is counted apart around its two calls. A walk that matches on every call is the per-car,
// per-pass decision R2 draws from; nothing on screen changes in this stage.
//
// Stage 2a (bakedCars = 2, report 105 section 4.2): the same walk also fills one draw list per
// car draw -- the leaves it reached, from objects CarBake.h bakes out of GPL's memory, merged into
// ranges under one frame (the poses from the car's root) and one MATERIAL state each -- and checks
// that the list holds exactly the polygons the walk counted. Nothing is drawn from it yet.
//
// Stage 2b (bakedCars = 3, report 105 section 4.3): in a baked main pass each car gets ONE list
// per pass, walked without the cell's clip planes on its first call (a car in N cells is called
// N times), kept in a per-pass arena that CarRenderer draws at the end of the pass; GPL's draw of
// the car still runs on every call, but its body dispatch (the call at 0x48703F) is skipped, so
// only the pose, the helmet tint and the blob shadow remain GPL's. Mirror passes, the cockpit and
// any car call outside a baked main pass go to GPL whole, counted but not walked: levels 1 and 2
// are the ones that verify every draw against GPL's counter. With bakedMirrors = 1 a baked mirror
// pass takes its cars the same way (report 105 section 4.18), counted apart, and debug view 15
// walks GPL's mirror cars beside its draw and checks them as level 1 does.

#include <cstdint>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

// Called from bakedPassBind once the pass is bound. Does nothing unless bakedRenderer and
// bakedCars are on; logs what it installed or why not.
void carWalkInstall();

// Stage 2b: bakedPassBegin opens a new pass's arena (`mirror`: a mirror pass); bakedPassEnd reads
// its lists, one per car drawn by gplvr, in the order GPL first called each car. In VR the
// first eye's lists are kept for the second eye's pass of the same kind (CarShare.h), and in
// every main pass a car's kept list (CarProgram.h) stands in for its walk where it holds. `pane`:
// a mirror pass's glass (stereoMirrorPaneOf, -1 not known), which keys its cars' kept lists
// (report 105 section 4.54).
void carWalkPassBegin(bool mirror, int pane);
const baked::CarDrawList* carWalkPassLists(uint32_t& count);

}  // namespace gplvr
