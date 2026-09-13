#pragma once

// The game's half of the VR channel.
//
// Creates the shared control block, launches gplvr-xr.exe as the presenter, waits to be
// told which adapter the OpenXR runtime wants, then hands it one shared D3D11 texture
// per frame containing both eyes.
//
// It runs entirely from the frame hook on the game's render thread, and it is a state
// machine rather than a blocking sequence: the presenter takes a second or two to bring
// SteamVR up, and the game must not stop while that happens. Every step that can fail
// logs once and drops the whole thing back to inactive, because a mod that stalls the
// game waiting for a headset is worse than one that quietly does not start.

#include "presenter/SharedPresentation.h"

namespace gplvr {

// Called once, after discovery. False means VR is off or the presenter could not be
// launched; the game carries on as a flat stereo mod.
bool presenterStart();

// Called from the frame hook, before the game presents, so the render target still
// holds the finished frame. Drives the handshake, then copies and publishes.
void presenterFrame(bool sideBySide, bool stretchToFill);

// The off-screen render target, which is what decouples the eye resolution from the
// game window. These own the GL entry points, which is why they live here rather than
// being called on RenderTarget directly from the frame hook.
//
//   presenterEndFrame     stop drawing off-screen, before the copy and the present
//   presenterBlitToWindow show the off-screen frame in the game window
//   presenterBeginFrame   start drawing off-screen again, after the present
//
// All three are inert unless vr and vrOffscreen are on and the target actually built.
void presenterEndFrame();

// The frame transfer's cost. Noted per transfer, taken and reset by the reporter, so the
// once-a-second line can say how much of the tick the transport is using -- the only
// per-frame term that has never been measured.
void presenterNoteTransfer(long long ticks);
bool presenterTakeTransferCost(double& totalMs, int& count);

void presenterBlitToWindow(int eye);
void presenterBeginFrame();

// Draw the mouse pointer into the frame that is about to be copied and presented.
//
// Here rather than called on CursorOverlay directly from the frame hook, for the same
// reason the three above are: this side owns the resolved GL entry points and the frame
// hook has no GlFunctions of its own. `sideBySide` says whether this frame holds two
// eyes, which is what decides between one cursor and one per eye.
//
// It also saves the pixels it covers; presenterRestoreUnderCursor puts them back.
void presenterDrawCursor(bool sideBySide);

// Put back the pixels the cursor was drawn over, before the game presents.
//
// Exactly one of these per presenterDrawCursor, in the same frame, AFTER the frame has
// gone to the headset. GPL's menu does not redraw the whole back buffer and roglv2
// preserves it across the swap, so a cursor left in there stays for the rest of the
// session and the pointer paints a trail instead of moving.
//
// Gated only on the GL entry points and deliberately NOT on the presenter's stage, so a
// save can never be left unmatched by a stage change landing between the two calls.
void presenterRestoreUnderCursor();

void presenterStop();

// The most recent eye views the presenter published, or false if none yet. This is what
// Phase 3 will feed into the scene parameter block; nothing consumes it today beyond the
// log line that proves the return path works.
//
// `motion`, when given, receives the head velocity and display clock published with them.
bool presenterLatestViews(SharedView views[2], SharedPose& head,
                          SharedMotion* motion = nullptr);

// Publish the seat -- the recentred head pose, heading only, in the presenter's LOCAL
// space -- so a layer can be placed relative to it. Called on every recentre.
void presenterPublishSeat(const SharedPose& seat);

// True once a seat has been published to the presenter.
bool presenterSeatPublished();

// Pribluda's overlay, drawn into a shared texture the presenter shows as a quad layer.
//
// presenterHudBegin makes the next texture slot the GL draw target, cleared transparent,
// with the aspect of aspectWidth x aspectHeight; the size chosen comes back in
// width/height. False means there is no headset channel, no framebuffer support or no
// texture, and nothing was bound. presenterHudEnd must follow every successful Begin in
// the same frame: it restores the previous draw target and hands the texture back to
// Direct3D. The image is published with the frame, after the transfer has drained GL, so
// the presenter never copies a panel that is still being drawn.
bool presenterHudBegin(int aspectWidth, int aspectHeight, int& width, int& height);
void presenterHudEnd();

// The presenter's view-publication counter, or 0 if it is not publishing.
//
// Extra renders pace themselves off this rather than off a slice of the game's tick.
// Slicing produced 108 Hz into a 90 Hz display, which does not divide: about every
// fifth frame is discarded and the leftovers beat at 18 Hz, which is visible as exactly
// the stepping the extra renders were meant to remove. Locking to this counter makes
// each rendered frame correspond to one display frame.
int32_t presenterViewSequence();

// False when the runtime forbids the application changing the field of view. Defaults
// to true until the presenter has reported, so nothing is disabled on a guess.
bool presenterFovMutable();

}  // namespace gplvr
