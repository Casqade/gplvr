#pragma once

// Render pacing: start each frame's latch so the frame is published just before the
// presenter's latch, and every displayed frame is the same number of sim ticks newer than
// the last.
//
// GPL's loop polls for a new sim tick and latches the moment one arrives, so at 144 Hz sim
// against a 72 Hz display the frames the headset shows are 1, 2 or 3 ticks apart
// irregularly: world judder, and a projection pose whose age changes frame to frame. The
// wait here sits at the fgDisableBG call inside gpl_fgWaitForTickAndSnapshot, after the new
// tick has arrived and before the BG lock is taken, so the sim keeps stepping through it and
// the draw still follows its latch immediately. docs/audit-2026-09-14/43-render-pacing.md.
//
// Everything here runs on the game's fg thread. The presenter's clock is read from the
// control block under its view-sequence check.

#include "Discovery.h"

namespace gplvr {

// Locate the fgDisableBG call site in the tick wait. Nothing is patched: RenderProfile scans
// for the same call and has to see the stock bytes. VR only.
void renderPacingDiscover(const Binding& binding);

// Redirect that site, chaining to whatever it calls now -- fgDisableBG, or the profiler's
// marker thunk if profileStages installed one. Call after renderProfileDiscover.
void renderPacingInstall();

// This frame's headset copy has just been published (Producer, after frameSequence moves).
void renderPacingNotePublished();

// The end of the frame hook's publish block. `trackedStereo` is whether this frame was a
// tracked stereo world frame, which is what the next frame's pacing decision reads.
void renderPacingEndFrame(bool trackedStereo);

// True when this frame's latch was paced, or started at once because the frame is over budget.
// Extra renders stay off while it is.
bool renderPacingEngaged();

// The tick wait's fgDisableBG call site, 0 unless pacing installed there. Interpolation finds
// the fgEnableBG call that closes the latch relative to it.
uintptr_t renderPacingTickWaitSite();

// A getBGCount reading taken at `qpc`, for the sim tick clock. The pacing wait feeds it too.
void renderPacingObserveTick(long long qpc, int count);

// When sim tick `count` arrived, in QPC ticks, from that clock. False before its first reading
// at the current sim rate.
bool renderPacingTickArrival(int count, double& qpc);

// When the frame whose latch has just run reaches the display, in QPC ticks. With the
// presenter's clock: the presenter latch the frame aims for (paced) or the first it can make
// after its measured cost (unpaced or over budget), mapped to that latch's display;
// `fromPresenter` true.
// Without it: the latch plus the measured latch-to-publish cost.
bool renderPacingDisplayTime(long long now, long long& display, bool& fromPresenter);

// The once-a-second pacing line. False when the window held no tracked stereo frame.
bool renderPacingFormat(char* out, size_t size);

}  // namespace gplvr
