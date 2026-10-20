#pragma once

// THE IN-CAR QUICK MENU: a radial of four actions under the trigger, and the pause it owns.
//
// HOLD THE TRIGGER in the car, either hand, with the VR menu closed, and four slices appear
// ahead of the eyes: Reset car at the top, Back to pits at the bottom, Replay on the right and
// Pause or Resume on the left. Turning the hand about 15 degrees from where it pointed at the
// press, or pushing the stick, picks a slice -- whichever of the two is pushed further -- and
// LETTING GO fires it. Letting go in the middle fires nothing. Back to pits in a race also wants
// the slice held for half a second, shown by a ring filling round it, before a release counts.
//
// EVERY ACTION IS ONE GPL ALREADY HAS, reached the way GPL reaches it (report 97):
//   * Reset car posts message 36 through gpl_fgPostResetCar, after GPL's own Shift+R predicate.
//     The sim re-checks nothing, so the slice is greyed from exactly that predicate and says why.
//   * Back to pits follows GPL's Esc table (0x54E008): the immediate leave (message 53) when GPL
//     would leave at once, GPL's own Enter-confirm function otherwise, which posts message 37 --
//     or, while paused, posts the unpause and lets GPL's frame post 37 once it lands.
//   * Replay posts an F2 press to the game window, so GPL's keyboard proc puts 0xC600 into its
//     key ring on the thread that ring belongs to (see "threads" below).
//   * Pause and Resume post messages 42 and 43.
//
// PAUSE BELONGS TO A RULE, not to the slice: the game is paused while the VR menu is open OR the
// driver chose Pause here, and never in multiplayer. Opening the VR menu requests a pause;
// closing it requests the unpause only when this module's pause is the one holding the game and
// the driver has not chosen Pause. A pause GPL made itself -- its own Pause key, an instant
// replay, whose return leaves the sim paused -- is never undone by the VR menu closing, and a
// driver who unpauses with GPL's own key is not paused again until something asks afresh. The
// slice itself follows GPL's actual state: it reads Resume whenever the game is paused, whoever
// paused it, and choosing it posts the unpause. gpl_sessionState is never written.
//
// THREADS. Everything here runs on GPL's fg thread: the key poll and the menu's layer build both
// run from the present hook, which `tick: 'present' runs on the fg task` places there, and the
// fg thread is the one GPL's driving screen posts its own messages from. GPL's KEY ring is
// different: its producer is the window proc, which runs in the message pump the MAIN thread
// drives (gpl_tickInterrupt), so a word pushed into it from here would race GPL's own writes.
// The F2 press therefore goes through the window's message queue instead.
//
// Always on in VR; there is no option.

#include "GlInterop.h"

#include <cstddef>

namespace gplvr {

// Checks every GPL function and global the actions use, once, from DllMain's install thread. A
// failed check greys the slices that depend on it ("unsupported build") and says which in the
// log; nothing is patched either way.
void quickMenuInstall();

// Once a frame from the key poll, after vrMenuPoll, on the fg thread. Opens, steers, fires and
// closes the radial, and keeps the pause rule. Closes the radial without firing the moment it
// stops being available.
void quickMenuPoll();

// True while the radial is open. The VR menu's poll leaves the face buttons and the stick alone
// then: the stick is the radial's.
bool quickMenuOpen();

// A key press and its release, posted to the game window's queue, so GPL's keyboard proc sees
// them on the main thread its key ring belongs to (see "threads" above). The replay menu's Back
// uses it for Esc. `down` and `up` are 1 for each post Windows took; false when there is no game
// window, and nothing was posted.
bool quickMenuPostKey(UINT vk, UINT scanCode, int& down, int& up);

// Close it without firing, releasing the controller. Safe when it is closed.
void quickMenuCancel(const char* why);

// Build and publish the radial's image on the VR menu's quad layer, from vrMenuUpdate while the
// VR menu is closed, with the game's GL context current. True while the radial holds the layer;
// false hands it back -- hidden once if this module had it -- to the replay panel and GPL's
// dialogs.
bool quickMenuUpdate(const GlFunctions& gl);

// The GL context is going away; the next open builds its image afresh.
void quickMenuReleaseGl();

// The once-a-second line, only while the radial has been used. False otherwise.
bool quickMenuFormatStatus(char* out, size_t size);

}  // namespace gplvr
