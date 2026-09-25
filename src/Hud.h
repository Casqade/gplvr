#pragma once

// GPL's in-race HUD, once per eye.
//
// CONFIRMED against gpl_simFrameRender (0x0048B240) on 2026-09-12: the block after
// gpl_sceneRender returns is twenty-odd gpl_rasterDrawDebugText calls -- Last, Best,
// Overall, Driver, the lap times, the flag state, Mrshls and twelve marshal posts -- and
// the whole block sits behind `gpl_optShowTimes`, read from the player profile's
// ShowTimes key. With that off nothing here is called and the thunk is inert.
//
// Every call's destination is gpl_cameraGetSurface(mainCamera), which by then is the full
// side-by-side buffer, so a stereo frame gets one HUD spanning both eyes. The frame-time
// readout at the end of the same function is the exception: it targets
// screenGetBackBuffer() and is left alone.
//
// The whole HUD is about twenty `drawDebugText` calls made inline inside
// `gpl_simFrameRender` after `gpl_sceneRender` returns -- the lap times, the driver
// names, the flag state, the marshal column. There is no other 2D in a race: the mirror,
// the pit board, the flags and the marshals are all world geometry inside sceneRender and
// are already stereo-correct.
//
// In stereo those twenty calls run ONCE, into the full side-by-side surface, after both
// eye batches have been flushed. Left-anchored items land in the left eye, right-anchored
// items in the right eye, and the centre block straddles the seam. No item appears in
// both eyes, so there is no disparity to be wrong -- there is simply half a HUD in each
// eye, in different places.
//
// The fix needs no `drawHud()` to call twice, because the destination is an ARGUMENT.
// `drawDebugText` is one of the rasterizer function-pointer globals gplvr already
// resolves by name, its first argument is the destination surface, and the two eye
// surfaces are the same handles `bitmapInstanceAlloc` returned to Stereo. So the thunk
// re-issues each call once per eye against the eye's own surface, and a sub-rect instance
// carries its absolute origin, so nothing but the horizontal anchor needs remapping.

#include "Discovery.h"

namespace gplvr {

// Thunk the `drawDebugText` global. Inert without stereo: every call falls through to the
// original with its own destination.
void hudBind(const Binding& binding);

// Forget the calls recorded for the previous frame. From the scene-render hook, which
// runs before the HUD block that follows it in gpl_simFrameRender.
// Put the thunk back if something re-resolved the rasterizer globals over it, adopting
// whatever it found as the new original. gpl_rasterResolveEntryPoints runs on every mode
// change, so this has to be checked per frame rather than installed once.
void hudEnsureThunk();

void hudBeginFrame();

// The frame's stamped text, flushed. From the present hook, after GPL has issued every
// stampDraw of the frame: logs new or changed strings under a per-second cap, and once a
// second a summary of stamp calls, strings and (with vrSkipStampText) dropped calls.
void hudEndFrame();

// Re-issue this frame's HUD. Called after each EXTRA render, which re-enters
// gpl_sceneRender underneath a HUD that has already been drawn and paints the world over
// it -- so without this the HUD survives only on frames with no extra render, which at
// `frameRate = 2` into a 90 Hz headset is one frame in three.
void hudReplay();

// True while the bitmapDraw global holds the thunk, which is the only place the mirror
// panes' copy offset can be applied. Stereo keeps the stock single mirror image without it.
bool hudBitmapDrawHooked();

}  // namespace gplvr
