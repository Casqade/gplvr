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

// When the tick after the newest one observed is due, in QPC ticks.
//
// This is the deadline for anything that runs in the tick's idle tail, and it is NOT "now plus
// a tick": by the time the frame has been drawn and presented, most of the tick is already
// gone. Budgeting from `now` is how the extra frames came to believe they had 24 ms when they
// had 13, started renders that could not finish, and pushed the game's own frame late.
bool renderPacingNextTickDue(double& qpc);

// When the frame whose latch has just run reaches the display, in QPC ticks. With the
// presenter's clock: the presenter latch the frame aims for (paced) or the first it can make
// after its measured cost (unpaced or over budget), mapped to that latch's display;
// `fromPresenter` true.
// Without it: the latch plus the measured latch-to-publish cost.
bool renderPacingDisplayTime(long long now, long long& display, bool& fromPresenter);

// The same, for a frame that is not the one whose latch just ran -- a frame drawn between two
// sim ticks. `anchorQpc` is when that frame starts, and the answer is the display IT reaches.
//
// This distinction is the difference between smooth and juddering, and it is not obvious. The
// answer is quantised to the presenter's latches, so two frames headed for the same display get
// the same one and two headed for different displays get different ones -- which is exactly
// right, and is what makes the world's sampled instant depend on when a frame will be SEEN
// rather than on when it happened to be drawn. Anchoring on the latch instead quantises to the
// sim tick, so every frame between two ticks blended to one instant; anchoring on nothing and
// using the wall clock lets the world's instant jitter by however much the draw-to-display
// interval jitters, which is roadside objects stepping as you pass them.
bool renderPacingDisplayTimeAt(long long anchorQpc, long long now, long long& display,
                               bool& fromPresenter);

// The once-a-second pacing line. False when the window held no tracked stereo frame.
bool renderPacingFormat(char* out, size_t size);

}  // namespace gplvr
