#pragma once

// The Windows mouse pointer, drawn INTO the frame, because the headset never sees it.
//
// Windows composites the pointer onto the desktop, not into the game's back buffer, so
// everything the presenter copies -- back buffer or off-screen framebuffer -- is a
// picture with no cursor in it. In a headset that makes GPL's entire front end
// unusable: the menus respond to a pointer nobody can see.
//
// WHAT IS DRAWN
//
// The REAL cursor, not a drawing of one. GetCursorInfo gives the HCURSOR Windows is
// showing at this instant, GetIconInfo gives its colour bitmap, its mask and its
// hotspot, and GetDIBits turns those into pixels that go up as a GL texture. So it is
// the user's own arrow, at their own size and theme, and it follows the shape as it
// changes -- I-beam, resize, busy -- because the texture is rebuilt whenever the HCURSOR
// handle changes and only then. The hotspot is honoured, so the point that clicks is the
// point drawn at the pointer's position.
//
// A cursor whose pixels cannot be read draws NOTHING, and the log names the step that
// refused. A plain dot stood in here for a while -- scaffolding from when the mask
// handling was unproven -- but a stand-in pointer that is not the user's own pointer is
// worse than an honest gap that says what went wrong.
//
// WHY HERE AND NOT IN THE PRESENTER OR IN A QUAD LAYER
//
// Three places could carry an overlay, and this is the one with the fewest moving parts
// and the fewest agreements between components:
//
//   - here, into whatever GL surface the frame was rendered into, after the game has
//     finished with it and before the presenter copies it. One code path covers BOTH
//     the off-screen world frame and the 640x480 menu frame, because it does not care
//     which is bound -- it draws into the current framebuffer, in the current viewport.
//     The menu is exactly the case this exists for, and the menu is the case the
//     off-screen path is deliberately SUSPENDED for, so a solution living inside the
//     off-screen target would have missed it entirely.
//   - in the presenter, into the D3D11 texture before submit. That means a second
//     graphics API, a lock/unlock around the interop object, and the mono/side-by-side
//     decision duplicated on the far side of the shared block.
//   - as an OpenXR quad layer. The most isolated from the game, and the most machinery:
//     another swapchain, another layer in the submit, and the same screen-to-eye mapping
//     anyway. PicoStreamingXR's quad-layer support is also unverified here, and an
//     unverified runtime feature is not the place for the fix to "the user cannot reach
//     the menus".
//
// The cost of being inside the game's context is that GL state has to come back exactly
// as it was found. That is handled with glPushAttrib/glPopAttrib and a matrix push/pop
// per stack, and -- the part this project has already paid nine test runs for -- the
// matrix MODE is read on the way in and put back on the way out rather than assumed:
// rogl_displaySetDestination calls glLoadIdentity/glOrtho with no glMatrixMode of its
// own and relies on rogl_displaySetupGlState having left the mode at GL_PROJECTION. The
// mode found on entry is logged the first time, so the assumption is evidence rather
// than folklore.

#include "GlInterop.h"

namespace gplvr {

// Draw the pointer into the currently bound framebuffer, in the current viewport.
//
// `sideBySide` is true when this frame holds two eyes, in which case one cursor is drawn
// per eye rect; false for a single image shown to both eyes, which is what a menu is.
//
// Drawn while Windows says the pointer is showing. Every refusal says which one it was: a
// silent return here reads as "the feature does not work" and costs a whole session.
//
// It also SAVES the pixels it is about to cover, into a scratch texture; the restore
// below puts them back. GPL's menu does not redraw the whole back buffer and roglv2
// preserves it across the swap, so without that pair the pointer paints a trail instead
// of moving. Observed in game, not predicted.
void cursorOverlayDraw(const GlFunctions& gl, bool sideBySide);

// Put back what the cursor was drawn over, leaving the game's own buffer exactly as it
// was found. Must be called once per cursorOverlayDraw, in the SAME frame: after the
// frame has been transferred to the headset, and before the game presents.
//
// The saved pixels are microseconds old and came from the same framebuffer in the same
// frame, so they cannot be stale -- which is why the restore belongs there and not at
// the top of the next frame.
//
// **A deliberate consequence: the flat window no longer shows the overlay wherever the
// window IS the render target -- which is the menus.** It does not need to. Windows draws
// the real pointer over the window already, and the entire point of this overlay is that
// the headset cannot see that one. Do not "fix" it by moving the restore after the
// present: that puts the trail straight back.
void cursorOverlayRestore(const GlFunctions& gl);

// Forget the cursor and backing-store textures because the GL context they belong to is
// going away. The names are dropped rather than deleted -- glDeleteTextures against a dead or replaced context
// either does nothing or destroys somebody else's object of the same name. The next
// frame that draws rebuilds it from the HCURSOR.
void cursorOverlayReleaseGl();

// The Windows pointer's own shape -- the texture cursorOverlayDraw keeps -- with its hotspot at
// (x, y) and `scale` units of the current 2D transform per screen pixel, drawn into whatever is
// bound. For a surface of gplvr's own rather than the frame: the replay panel draws the pointer on
// its canvas with this. No backing store, because that canvas is rebuilt whole. False when
// the shape cannot be read, and the caller draws something else.
bool cursorOverlayDrawShape(const GlFunctions& gl, float x, float y, float scale);

}  // namespace gplvr
