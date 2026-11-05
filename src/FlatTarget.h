#pragma once

// The flat world frame, drawn off-screen and copied into the window once, at the swap.
//
// Without it every step of a frame lands in the window's own back buffer: GPL's mirror pass
// into the top-left corner, the world over it, the HUD, pribluda. A present path that shows
// that buffer out of step with SwapBuffers -- the NVIDIA "OpenGL present method" set to "Prefer
// layered on DXGI Swapchain" does, in vanilla GPL too (report 109) -- then shows half-built
// pictures: the mirror in the corner, cars and cockpit flickering. With it, the window's buffer
// receives one finished frame per swap and nothing else.
//
// It also decouples the frame's size from the window's. GPL picks its render size when it
// enters a race; a window moved to another monitor mid-race (Win+Shift+Arrow) gets the frame
// scaled to fit instead of cropped, until the next race entry.
//
// The framebuffer is bound for the whole frame, from just after the present to the swap, so
// pribluda -- which draws inside roglv2's present, from the call it patches at roglv2+0x9B83,
// just before SwapBuffers -- draws into it as well. The copy is made in roglv2's SwapBuffers
// import. Flat world modes only: the front end belongs to MenuScale, VR to RenderTarget.

#include <windows.h>

namespace gplvr {

// Hook roglv2's SwapBuffers import. Call after the profiler's driver thunks, so the chain is
// this, then the profiler's swap timer, then the driver.
void flatTargetInstall();

// Bind the framebuffer for the next frame. Called right after the game presents, next to
// menuScaleBeginFrame; inert unless `flatOffscreenFrame` is on and the engine is in a world
// mode in flat mode.
void flatTargetBeginFrame();

// Drop the framebuffer because the display is about to be rebuilt underneath it.
void flatTargetSuspend();

// True while this frame is being drawn off-screen.
bool flatTargetActive();

// Map a point in window client pixels into the frame's own pixels while the frame is scaled
// into the window (after a monitor move), so clicks on the replay screen's buttons land. False
// at 1:1 and outside world modes, and the point is then untouched.
bool flatTargetMapClientPoint(POINT& point);

}  // namespace gplvr
