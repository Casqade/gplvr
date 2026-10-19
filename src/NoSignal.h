#pragma once

// GPL's "no signal" static, as a fact the present hook can ask about.
//
// gpl_drivingViewWidgetDraw (0x423100) asks its getter for the latched view. When there is
// none -- a camera or driver switch, a replay cut, until gpl_viewUpdate validates the new view
// -- it never enters gpl_simFrameRender: it fills the bound camera's surface black and draws the
// 256x256 gpl_noSignalNoiseTexture over it. That camera is whichever was bound LAST, and the
// static lands on its surface and nowhere else; the rest of the frame keeps the previous picture.
// On the replay screen it is the 3D view's (windowViewSurface): a dump of the whole 6344x3172
// frame showed the static at 0,0 2560x1440, over the left eye.
//
// The widget's byte at +8 is set to 1 by that branch (0x423165) and cleared by the render
// branch (0x423130), so after the draw returns it says which of the two this call took. The
// draw is reached through one .rdata vtable slot, 0x537604, which is thunked here after
// checking it holds the stock target. Flat mode too: it only observes.

namespace gplvr {

void noSignalInstall();

// Whether GPL drew its static at any point since noSignalEndFrame.
bool noSignalDrawnThisFrame();

// The rasterizer surface handle the static was last drawn on this frame, or null when that is
// not known. The same handle space as stereoEyeSurface.
void* noSignalSurface();

// The camera that was bound when the static was last drawn this frame, or null.
void* noSignalCamera();

// From the present hook, once the frame's publish decision is made.
void noSignalEndFrame();

}  // namespace gplvr
