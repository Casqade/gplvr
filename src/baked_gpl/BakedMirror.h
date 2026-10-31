#pragma once

// The baked mirror pass (docs/audit-2026-09-18/105-baked-renderer-rewrite.md section 4.18,
// bakedMirrors = 1). gpl_sceneRender draws the rear view before the main view, from the camera in
// p[43]: the register file switches to gpl_renderRegistersMirror (0x487766), the surface-class
// masks to the mirror's (0x48777B, 0x487789), the selector camera to the mirror's position
// (0x487809), the mirror camera is bound (0x487868; gpl_cameraBind writes its gpl_lodScreenScale
// at 0x4C7B71), the background is submitted (0x4878D4), the two draw distances are scaled by
// gpl_mirrorDistanceScale, and the gpl_worldRenderSegmentRange the main pass calls walks the
// track (0x487A4C). Then the drawn marks are cleared (0x487A78), and bitmapDraw flushes roglv2 and
// copies the camera's rectangle into mirror.mip (0x487C58) before the main camera is bound.
//
// This module redirects that one call. Around the walk it opens and closes a baked pass of the
// mirror (BakedPass.h, bakedPassBeginMirror and bakedPassEndMirror): GPL's mirror walk decides
// what is visible, the node filter marks it, the car hook lists the mirror's cars, and the end of
// the walk draws them into the mirror camera's rectangle of the framebuffer bound for drawing --
// the back buffer's corner in flat mode, MirrorTarget's framebuffer in VR -- through the private
// target and its own depth, which the residual program tests GPL's leftovers against in the flush
// bitmapDraw makes. mirror.mip is never touched: the copy-out reads the rectangle as before, so
// the per-glass regions, the per-eye copies and mirrorUpdateRate work as they did.
//
// Mirror detail 0 has no walk (a sorted loop calls the cars, 0x487BC2): nothing of it is baked.

#include "baked/CarRenderer.h"
#include "baked/Renderer.h"

#include <cstddef>
#include <cstdint>

namespace gplvr {

// From bakedPassBind once the pass is bound. Does nothing unless bakedMirrors is on; logs what it
// did and why not.
void bakedMirrorInstall();

// Why a mirror walk was left to GPL, counted per second.
enum MirrorRefusal : int {
    kMirrorNotBound = 0,   // bakedRenderer off, or the baked pass not bound
    kMirrorNoParams,       // no scene parameter block yet
    kMirrorNoCamera,       // p[43] is null
    kMirrorNoFilter,       // the node filter could not be installed
    kMirrorDrawingOff,     // bakedRendererKey turned the baked drawing off
    kMirrorNoScene,        // no baked scene for this track yet
    kMirrorUploadStopped,  // the upload stopped for this scene
    kMirrorRendererDown,   // the renderer is not up in this context
    kMirrorUploading,      // the main passes have not finished the GPU upload
    kMirrorDirectPath,     // p[85] bit 0 clear: GPL draws into the mip, no rectangle in GL
    kMirrorNoRect,         // the mirror camera's surface has no rectangle
    kMirrorCapture,        // the vanilla half of a capture
    kMirrorByGpl,          // debug view 15
    kMirrorRefusalCount,
};
void bakedMirrorRefused(MirrorRefusal why);

// One baked mirror pass, from bakedPassEndMirror. `drawn` false when the renderer did not draw it
// (the cars and flagmen of a pass that has lists are then lost: `cars.listDraws` says how many).
struct BakedMirrorPass {
    bool                drawn = false;
    bool                fog = false;
    int                 rect[4] = {0, 0, 0, 0};
    uint32_t            dispatches = 0;  // the visible list's entries: GPL's baked-item dispatches
    uint32_t            repeats = 0;     // of them, items reached again in the pass
    baked::DrawStats    stats;
    bool                hadLists = false;
    baked::CarPassStats cars;
    double              cpuMilliseconds = 0.0;  // the pass's end: draw and composite
};
void bakedMirrorAccount(const BakedMirrorPass& pass);

// Whether the flush at a baked mirror pass's start, which puts the background GPL has just
// submitted (0x4878D4) into the framebuffer before the rectangle is copied in, reached roglv2
// as a flush; `known` false when the residual hooks that see flushes are not installed.
void bakedMirrorBackgroundFlush(bool known, bool flushed);

// Debug view 15: the baked items GPL dispatched in one of its own mirror walks, for the line's
// comparison with the baked passes' dispatches.
void bakedMirrorObserved(size_t dispatches);

// Baked mirror passes drawn since the last report: the residual and flagman lines divide by the
// main and the mirror passes together.
unsigned bakedMirrorPasses();

// Once a second, from the baked pass's report: the mirror's lines, with its GPU times (null when
// the context has no timer queries).
void bakedMirrorReport(double seconds, const baked::GpuPassTimes* gpu);

}  // namespace gplvr
