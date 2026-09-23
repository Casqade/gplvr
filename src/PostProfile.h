#pragma once

// `post` on the flat frame line, broken up (docs/audit-2026-09-18/71-fast-text.md).
//
// `post` is sceneRender's return to the present hook. It holds roglv2's flush of the batches the
// scene left open (the world, the mirror), GPL's HUD and dash drawing, and wherever the driver
// makes the CPU wait for the GPU. This times the frame's four spans on the CPU clock -- pre,
// scene, post, present -- with every one of GPL's 2D entry points inside post timed and counted
// separately, and puts GL_TIMESTAMP marks (GpuTimer.h) at the same points, so the GPU line tiles
// the frame the same way and says how far the GPU runs behind the CPU: the answer to CPU- or
// GPU-bound. It is what measured the 250-310 us a glyph that the fast text path replaces.
//
// Flat mode only (FrameBudget.h owns the GPU marks when profileStages is on), and only with
// `profilePost = 1`: the GPU timer reads GL_TIMESTAMP once a frame, which is a sync point for a
// threaded driver.

namespace gplvr {

// GPL's seven 2D rasterizer entry points, in the order both this file and the fast text path
// use. GPL keeps the resolved address of each in a global of its own, and
// gpl_rasterResolveEntryPoints 0x4E21B0 re-fills all seven at every mode change.
enum RasterKind {
    kRasterBitmapDraw = 0,
    kRasterBitmapScale,
    kRasterStampDraw,
    kRasterDebugText,
    kRasterFillRect,
    kRasterDrawLine,
    kRasterAaLine,
    kRasterKinds
};

// The export name each global is filled from, so a slot can be resolved by name rather than by
// address (Discovery.h rasterGlobal).
extern const char* const kRasterNames[kRasterKinds];

// The time of one 2D entry point, for whoever holds the thunk. `postProfileRasterEnter` also
// opens the frame's 2D group on the GPU clock, and returns 0 when nothing is being measured.
long long postProfileRasterEnter(int kind);
void      postProfileRasterLeave(int kind, long long start);

bool postProfileActive();

void postProfileSceneBegin();
void postProfileSceneEnd();
void postProfilePresentBegin();
void postProfilePresentEnd();

// GPL's own 2D drawing after the scene -- the dash, the HUD, the mirror copy -- goes through
// seven rasterizer entry points whose addresses GPL keeps in globals (gpl_rasterResolveEntryPoints
// 0x4E21B0 re-resolves them at every mode change). They are thunked so the time and the call
// count of each are known, and the GPU clock is marked at the first of them in a frame. Called
// from the frame poll; it re-installs itself after a mode change.
void postProfilePollFrame();

// Once a second from the frame report, with the measured present-to-present period.
void postProfileReport(double periodMs);

}  // namespace gplvr
