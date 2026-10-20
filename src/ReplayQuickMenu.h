#pragma once

// THE REPLAY'S CONTROLS ON THE CONTROLLER: the stick for the transport and the cars, and a radial
// of eight actions under the trigger.
//
// Available only while GPL's foreground state is its replay screen and the screen's replay-state
// snapshot is valid this frame, with VR on, the VR menu closed, no tweak mode, no GPL file dialog
// over the screen and the in-car quick menu shut. Re-checked on every poll; an open radial that
// stops being available closes without firing and says why.
//
// THE STICK, either hand, while the radial is shut. One action per push, past half deflection
// along the axis pushed further; the stick has to come back near the centre before it acts again.
// What a left/right push means is decided as it starts and kept until the centre:
//   * left / right while playing, or live: rewind / fast forward -- GPL's own buttons, which
//     latch a speed per click (reverse starts at 1x, forward at 2x, up to 16x). Push again to go
//     faster; play/pause or a step stops it. Nothing happens on letting go.
//   * left / right while paused: one frame back / forward, and held out, GPL's own hold on its
//     step buttons -- the next step 8 sim ticks after the push, then one every 2 ticks.
//   * up / down: previous car / next car -- GPL's driver arrows. Up walks towards the head of
//     GPL's car list (report 99: probably the running order), down away from it.
//   * click: play / pause -- GPL's play button: it pauses from any speed and resumes forward 1x.
// The VR menu's quick recentre then keeps the face buttons and leaves the stick click alone.
//
// THE RADIAL. Hold the trigger with the ray OFF GPL's replay panel, and eight slices appear ahead
// of the eyes, chosen like the in-car quick menu's (Radial.h): up Play or Pause, down Back
// (to the car, the menu or the garage, by how the replay was reached), left and right the
// previous and next camera, up-left and up-right to the start and the end, down-left and
// down-right the previous and next session. Letting go fires; letting go in the middle fires
// nothing. With the
// ray ON the panel the trigger clicks GPL's own widgets exactly as before, and never opens this.
// The radial borrows the VR menu's quad layer from the replay panel while it is up.
//
// EVERY ACTION IS GPL'S OWN BUTTON (report 99): each fires the same argument-free gpl_fgPost*
// function the widget's handler calls, which posts one message to the sim thread. Back is Esc,
// the replay screen's only key, posted to the game window like the in-car Replay's F2 so the
// key ring is written on its own thread. Nothing here writes GPL's replay state.
//
// THREADS. The poll and the layer build both run in the present hook, on GPL's fg thread, which
// is where the replay screen's own button handlers post from (report 99 section 5).
//
// Always on in VR; there is no option.

#include "GlInterop.h"

#include <cstddef>

namespace gplvr {

// Checks every GPL function and global the controls use, once, from DllMain's install thread. A
// failed check greys what depends on it ("unsupported build") and says which in the log; nothing
// is patched either way.
void replayQuickMenuInstall();

// Once a frame from the key poll, after quickMenuPoll, on the fg thread: the stick, and the
// radial's open, steer, fire and close.
void replayQuickMenuPoll();

// True while the radial is open. The in-car quick menu stays shut, the VR menu's poll leaves the
// stick and face buttons alone, and the replay panel's pointer is not driven.
bool replayQuickMenuOpen();

// True while the controls are available, so the stick click is play/pause rather than the VR
// menu's quick recentre. Evaluated afresh at each call, so it answers for this poll.
bool replayControlsHaveStick();

// Close the radial without firing, releasing the controller. Safe when it is closed.
void replayQuickMenuCancel(const char* why);

// Build and publish the radial's image on the VR menu's quad layer, from vrMenuUpdate while the
// VR menu is closed, with the game's GL context current. True while the radial holds the layer;
// false hands it back -- hidden once if this module had it -- to the replay panel.
bool replayQuickMenuUpdate(const GlFunctions& gl);

// The GL context is going away; the next open builds its image afresh.
void replayQuickMenuReleaseGl();

// The once-a-second line, only while the controls have been used. False otherwise.
bool replayQuickMenuFormatStatus(char* out, size_t size);

}  // namespace gplvr
