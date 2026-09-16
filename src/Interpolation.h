#pragma once

// World interpolation: every car and the view are drawn as they were at the frame's display
// time, blended between the two newest sim ticks the latch copied.
//
// GPL's latch copies one tick into the car render records and the view record, and the draw
// takes the cars' and the camera's pose from nothing else (docs/audit-2026-09-14/44-car-draw.md),
// so a displayed frame steps by however many ticks its latch happened to be after the last one.
// Here each latched record is kept for two latches, and right after the latch -- the fgEnableBG
// call that closes gpl_fgWaitForTickAndSnapshot, before the event pump and the draw -- its
// continuous fields are rewritten with a blend at the frame's display time less one tick. The
// drawn world then advances by one display period per displayed frame at any display rate, for
// one sim tick of added latency. Discrete fields (handles, flags, hints) keep the newest tick.
//
// Everything here runs on the game's fg thread. The latch writes the records under the BG lock
// and bg never reads or writes them afterwards, so rewriting them before the draw takes no lock.

#include <cstddef>
#include <cstdint>

namespace gplvr {

// Redirect gpl_simFrameSetup's three gpl_carLatchRenderRecord calls and its view and mirror
// record latches, and the fgEnableBG call after the latch. Every site is checked against its
// stock bytes first; nothing is patched unless all match. VR only; call after
// renderPacingInstall, which locates the tick wait.
void interpolationInstall();

// True while the hooks are installed and vrInterpolate is on.
bool interpolationActive();

// The once-a-second interp: line. False when the window latched no world frame.
bool interpolationFormat(char* out, size_t size);

}  // namespace gplvr
