#pragma once

// Frames drawn between two sim ticks (`frameRate`).
//
// GPL's frame loop gpl_fgEventLoop 0x4F3570 opens every iteration with
// gpl_fgFrameSnapshotBegin 0x4EF5F0, which reaches gpl_fgWaitForTickAndSnapshot 0x4E8F20:
//
//     while (bgIsRunning()) { if (getBGCount() != gpl_fgLastLatchedBgCount) break; Sleep(1); }
//     fgDisableBG(); ...the 21 latch callbacks...; fgEnableBG();
//
// That poll is the frame cap. The loop refuses to start a frame until the simulation has
// produced a new tick, so flat GPL draws exactly one frame per tick -- 36 of them a second on a
// stock install, and the 3.6-4.9 ms `pre.tick` of report 72 is the renderer sitting in the
// Sleep(1) waiting for permission to start.
//
// This lets the loop go round while the sim has nothing new to say. When no tick has arrived and
// the frame's slice of the tick has come, gpl_fgFrameSnapshotBegin returns at once: no wait, no
// lock, no latch. The records still hold the last latched tick, Interpolation advances the blend
// to the present instant, and the loop draws and presents as usual. The world then moves at the
// display rate while the simulation goes on ticking at its own, for the one tick of latency
// interpolation has always cost.
//
// THE FRAME IS ALSO HELD OPEN, and that half is not optional. gpl_fgFrameSnapshotEnd 0x4EF650 at
// the bottom of the same loop iteration calls gpl_fgSnapshotRelease 0x4E8FA0, which zeroes each
// slot's data pointer in the frame descriptor and clears its valid byte -- so between End and the
// next Begin the game has no frame at all. gpl_drivingViewWidgetDraw 0x423100 asks its getter for
// one, gets 0, and draws GPL's "no signal" screen instead: a black clear and a noise texture on a
// randomly rotated quad, with a hiss started on a sound channel. So End is skipped alongside
// Begin, and gpl_fgFrameSnapshotBegin's own prologue -- `if (dword_94D714) End()` -- performs the
// release when the next real frame starts. The frame's lifetime does not grow: in stock GPL it
// already spans a whole tick, because Begin is where the frame waits.
//
// WHAT THIS IS NOT. It does not touch fgDisableBG/fgEnableBG, so the sim/render lock protocol is
// exactly GPL's; an extra frame simply never enters it. It reads no simulation state -- it
// redraws records latched normally under the stock lock -- so it needs no sim/render ring (report
// 72 section 6.5, never built past stage 2 and since removed).
//
// BOTH MODES. `frameRate` decides whether the wait is skipped and what, if anything, gates a
// frame afterwards: 1 draws one frame per display frame (per published view set in VR, per
// refresh in flat mode, where SwapBuffers does the blocking), 2 draws them as fast as they can
// be drawn, 0 leaves the loop exactly as GPL wrote it.
//
// THE FRAME CAP. In flat mode `frameLimit` gives every frame a slot and the loop waits for it
// at the top of the iteration, before the latch. It is there for variable-refresh displays: kept
// a few frames under the refresh rate, the display follows the game's frames, and SwapBuffers
// never has to block for a refresh.
//
// THE FRAME CLOCK DECIDES THE LATCH in flat mode. Each frame takes its instant on the flat frame
// clock (Interpolation) first; a tick that has arrived but is due after that instant is left for
// the next frame, and a tick due by it that has not arrived yet is waited for, briefly. The blend
// then always finds its target between the two ticks the frame latched. See latchByFrameClock.

#include <cstddef>

namespace gplvr {

// Redirect gpl_fgEventLoop's calls to gpl_fgFrameSnapshotBegin and gpl_fgFrameSnapshotEnd, both
// or neither. Where RenderProfile's markers already own those calls, this goes in behind them
// through the slots they jump through, so the markers keep marking every frame.
//
// Call after interpolationInstall -- an extra frame with nothing to re-blend draws a duplicate of
// the frame before it, so this declines to install when the blend did not.
void flatFramesInstall();

// True when the frame loop's wait is ours. In VR this is frameRate = 2 free-running, and it
// makes the extra-render tail in the present hook redundant: every frame now goes through GPL's
// own loop, with its mirror pass and its HUD, instead of being a partial repeat after the
// present. The tail stays as the fallback for when these call sites could not be taken.
bool flatFramesActive();

// Where this frame sits in the tick: 0 for one that latched a new sim tick, then 1, 2, ... for
// the frames drawn between that tick and the next. Always 0 when the loop's wait is GPL's own,
// which is what makes `every Nth frame` mean `every frame` there.
int flatFramesTickFrameIndex();

// The once-a-second `frames:` line: frames drawn, how many were between ticks, and what stopped
// the rest. False when no frame was drawn in the window.
bool flatFramesFormat(char* out, size_t size, double seconds);

}  // namespace gplvr
