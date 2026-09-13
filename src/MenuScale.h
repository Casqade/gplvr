#pragma once

// GPL's front end is 640x480 and cannot be laid out at any other size: 85 fixed .pbf
// images and widgets carrying absolute coordinates. Once the window stops being 640x480
// -- which is the entire point of the borderless window -- the menu becomes a small
// island in the corner of a large black rectangle.
//
// So the game keeps drawing at 640x480, into a framebuffer of exactly that size, and the
// result is scaled into the window: aspect-fit, with black bars on whichever pair of
// edges the window's shape leaves over. The game never learns it is not 640x480, so
// nothing about the layout, the art, the clip rects or rogl_clipRectToBitmap changes.
//
// NEGATIVE RESULT, so nobody tries it again: glViewport cannot do this. roglv2 blits the
// front end with glDrawPixels, which the viewport does not affect -- only its raster
// position goes through the transform. A viewport scale would move part of the menu and
// stretch the rest.
//
// The pointer is the other half, and it is one hook rather than the two the plan
// expected. gpl_mouseReadClientDelta returns `client - lastPosition` and its caller adds
// that straight back onto `lastPosition`, so the "delta" is an identity and the quantity
// the game actually stores IS the client position, clamped to the mode rect. Mapping the
// POINT through the inverse of the blit therefore gives the game a front-end coordinate
// for free; scaling the delta, which is what the plan called for, would not have worked
// at all. There is exactly one GetCursorPos and one ScreenToClient in the whole image,
// both inside that function, so hooking gplc67's ScreenToClient import reaches the
// pointer path and nothing else. Clicks carry their own copy of the position in lParam
// and are remapped in the window proc.

#include "GlInterop.h"

#include <windows.h>

namespace gplvr {

struct Binding;

// Hook gplc67's ScreenToClient, which is the pointer half, and resolve the two entry
// points the pre-blit flush needs. Inert unless the front end asks for it.
void menuScaleInstall(const Binding& binding);

// Bound right after the game presents, so the front end is drawn into it from its very
// first call of the next frame. Inert unless the engine is in
// the 640x480 front end.
void menuScaleBeginFrame();

// Unbind and scale into the window. Called before anything reads the window's back
// buffer -- the VR transfer included, which is what lets the headset show a scaled menu
// as well as the monitor.
void menuScaleEndFrameAndBlit();

// Drop the framebuffer because the surface underneath it is about to be rebuilt.
void menuScaleSuspend();

// True while the front end really is being scaled -- the option is on, the framebuffer
// exists and the engine is in the menu. Everything downstream keys off this rather than
// off the option, so a target that failed to build behaves exactly like the option
// being off.
bool menuScaleActive();

// Make roglv2 issue whatever it still has queued, NOW.
//
// polyDraw only appends to the ZBuffer drawer's bins; the flush runs at a 2D mode switch,
// at polySetDest, or as a last resort inside screenDisplay. So anything still queued when
// gplvr reads or copies the framebuffer is missing from what gplvr took, and appears in
// the window afterwards when roglv2 finally gets round to it. This issues one empty 2D
// call, which switches rogl_batchOpen's mode and flushes the polygons on its way in.
//
// It lives here because MenuScale is what resolves drawDebugText and screenGetBackBuffer,
// and it was written for the front end's blit -- but the hazard is not the menu's, it is
// anyone who looks at the framebuffer before the game has finished with it.
void rasterFlushPending();

// The rectangle the scaled front end occupies inside the window client area, in client
// pixels. False when nothing is being scaled, and `out` is then untouched.
bool menuScaleClientRect(RECT& out);

// Report, on change, whether a draw is reaching the front end's framebuffer or the
// window. Inert unless scaling is running. `when` names the call site in the log.
void menuScaleReportBinding(const char* when);

// Map a point in window client pixels into the front end's own 640x480 space, clamped
// to it. False when nothing is being scaled, in which case the point is untouched.
bool menuScaleMapClientPoint(POINT& point);

}  // namespace gplvr
