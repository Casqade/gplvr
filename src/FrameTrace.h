#pragma once

// A per-frame record over short windows, for two artifacts seen on the monitor and in the headset
// that last a frame or three:
//   - the first race frames: one squashed with the left mirror black, then one or two at the right
//     size with the bottom fifth black (about 2560 of 3172 rows drawn);
//   - GPL's "no signal" static on a refused car reset: a normal race pair with noise in the left
//     eye's top left, while every static frame logged as held.
//
// A window is armed where those begin: the eye targets rebuilt, the render size changed, or GPL's
// static drawn in a race. For each frame in it, one or two lines say:
//   - per eye pass: the GL viewport, scissor box and enable and draw framebuffer its polygons were
//     flushed with, and its surface's own size against its rect;
//   - the static, if drawn: the camera and surface it went to, and that surface's size;
//   - what reached the headset: held for the static, held by the presenter at the wrong size,
//     published flat, as a pair, or as a held pair, with the size rendered and the stretch flag.

namespace gplvr {

// Arms or extends the window. Capped per session; says so when the last one is spent.
void frameTraceArm(int frames, const char* why);
bool frameTraceArmed();

// After an eye pass's own render, its bins flushed (Stereo).
void frameTraceEye(int eye, void* surface, int surfaceWidth, int surfaceHeight, const int rect[4]);

// A frame GPL drew its static on (Hooks): the camera bound, its surface and what that surface is.
void frameTraceStatic(void* camera, void* surface, int width, int height, const char* what);

// presenterFrame's size check (Producer): the size rendered, the size wanted, held or not.
void frameTracePresenter(int width, int height, int wantWidth, int wantHeight, bool held);

// The frame's publish decision (Hooks), which ends the frame's record.
void frameTracePublish(const char* outcome, bool sideBySide, bool stretch);

}  // namespace gplvr
