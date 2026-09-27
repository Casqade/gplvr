#pragma once

// THE SIM TICK CLOCK AND THE DISPLAY-TIME MODEL, hooked at the tick wait.
//
// THE PACING IS GONE, 2026-09-27, and the name is a leftover. This file used to hold a wait
// that delayed each frame's latch so the frame was published just before the presenter's --
// see docs/audit-2026-09-14/43-render-pacing.md, which remains the record of it. That wait
// never ran. FlatFrames takes the frame loop's wait over and draws one frame per published
// view set, which solves the same problem earlier and better, and the pace stood down
// whenever it was installed: measured over 322 seconds, 304 reporting windows, `0 paced` in
// every one. The only configuration that would have reached it was FlatFrames failing to
// patch gpl_fgFrameSnapshotBegin, and an exe where that fails is not one to have a quietly
// different frame scheduler on -- it is one to stop and look at.
//
// What was underneath it is what the file is now, and it is load-bearing. The hook at the
// fgDisableBG call inside gpl_fgWaitForTickAndSnapshot is where a tick's arrival can be timed
// accurately, so it feeds:
//
//   * the SIM TICK CLOCK -- when tick N arrived, fitted from getBGCount readings. The blend
//     divides by this grid, so Interpolation asks it for every target it computes.
//   * the DISPLAY-TIME MODEL -- when the frame whose latch has just run will be SEEN,
//     quantised to the presenter's latches. This is what interpolation targets, and the
//     reason it is a display time and not a wall clock.
//   * the latch-to-publish cost, which the model above needs and which only a hook at the
//     latch can measure.
//
// Everything here runs on the game's fg thread. The presenter's clock is read from the
// control block under its view-sequence check.

#include "Discovery.h"

namespace gplvr {

// Locate the fgDisableBG call site in the tick wait. Nothing is patched: RenderProfile scans
// for the same call and has to see the stock bytes. VR only.
void renderPacingDiscover(const Binding& binding);

// Note for anyone adding to this file: the names still say "pacing" because renaming fourteen
// functions across six files is a mechanical change worth making on its own, after a build.

// Redirect that site, chaining to whatever it calls now -- fgDisableBG, or the profiler's
// marker thunk if profileStages installed one. Call after renderProfileDiscover.
void renderPacingInstall();

// This frame's headset copy has just been published (Producer, after frameSequence moves).
void renderPacingNotePublished();

// The end of the frame hook's publish block. `trackedStereo` is whether this frame was a
// tracked stereo world frame, which is what the next frame's pacing decision reads.
void renderPacingEndFrame(bool trackedStereo);

// The tick wait's fgDisableBG call site, 0 unless the hook installed there. Interpolation
// finds the fgEnableBG call that closes the latch relative to it.
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
// presenter's clock: the first presenter latch the frame can make after its measured cost,
// mapped to that latch's display; `fromPresenter` true.
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

// The once-a-second latch line. False when the window held no tracked stereo frame.
bool renderPacingFormat(char* out, size_t size);

}  // namespace gplvr
