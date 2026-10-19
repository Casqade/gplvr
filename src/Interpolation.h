#pragma once

// World interpolation: every car, the view and the pit board are drawn as they were at the
// frame's display time, blended between the two newest sim ticks the latch copied.
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

// Redirect gpl_simFrameSetup's three gpl_carLatchRenderRecord calls and its view, mirror and pit
// board record latches, and the fgEnableBG call after the latch. Every site is checked against its
// stock bytes first; nothing is patched unless all match. Installs in VR, and in flat mode when
// frameRate asks for frames between the ticks. Call after renderPacingInstall, which
// locates the tick wait in VR.
void interpolationInstall();

// True while the hooks are installed and the blend is on for this mode.
bool interpolationActive();

// Re-blend the records of the last latch to the present instant, for a frame drawn between two
// sim ticks (FlatFrames.h). The blend always works from the two kept copies rather than from
// what it last left in the record, so repeating it with a later target is exact.
//
// False when there is nothing to blend -- no tick model yet, or the last latch left no usable
// pair -- in which case the extra frame draws the same world as the one before it.
bool interpolationExtraFrame();

// Which records the blend rewrites, as a bitmask. All of them, always; the bisect that cycled
// these lives in docs/audit-2026-09-18/85-instrument-archive.md if it is ever wanted again.
enum InterpolateParts {
    kPartCars = 1,
    kPartView = 2,
    kPartMirror = 4,
    kPartPitBoard = 8,
    kPartAll = 15,
};

// The viewed car's world position as the blend left it, and the same car's position at the
// newest latched tick. False when no car is being viewed (a pit or TV camera), which is also the
// case where the camera genuinely is somewhere else and must keep its own answers.
bool interpolationViewedCarPosition(float blended[3], float newestTick[3]);

// The viewed car's own blended distance along the track, in metres (car record byte 12). The
// value gpl_cameraTrackPositionFromWorld is supposed to be computing, taken from the sim
// instead of from a search that can answer a segment out. False when there is no viewed car.
bool interpolationViewedCarDlong(float& metres);

// The view's own blended dlong along the track, and whether the view history snapped this frame.
// A snap is a camera cut, a car reset or a tick gap: the value legitimately jumps there.
bool interpolationViewDlong(float& metres, bool& snapped);

// THE SAME CAMERA, THE SAME FIELD, AT THE NEWEST LATCHED TICK -- what the blend started from.
// The pair with interpolationViewDlong above is the only honest "with blending and without it"
// for the one quantity the world walk is centred on: same body, same frame, one value the sim
// wrote and one the blend wrote over it. False until a tick has been latched.
//
// It is NOT interpolationViewedCarDlong. That is a different body and is itself blended.
bool interpolationViewDlongAtTick(float& metres);

// The once-a-second interp: line. False when the window latched no world frame.
bool interpolationFormat(char* out, size_t size);

}  // namespace gplvr
