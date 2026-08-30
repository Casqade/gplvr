#pragma once

namespace gplvr {

// Mouse deltas for the debug look override, read through Raw Input.
//
// Why Raw Input rather than GetCursorPos: the obvious implementation reads the cursor
// each frame and warps it back to the middle, and that fights the game for ownership of
// the pointer -- the menus need it, and roglv2 clips it to its own window. Raw Input
// reports the device's own movement without the cursor being involved at all, the same
// way GetAsyncKeyState reads keys without consuming them. Nothing the game does with the
// mouse changes, which is the only way this can be switched on mid-session and judged
// against the same run with it off.
//
// RIDEV_INPUTSINK, so the deltas keep arriving while the game window holds the focus.

// Brings up the message-only window and registers the device. Safe to call every frame;
// it does the work once and remembers whether it failed, so a machine where Raw Input is
// unavailable logs one line rather than one per frame.
bool mouseLookEnsureStarted();

// Drains the accumulated movement since the last call, in raw device counts. Returns
// false if nothing has moved, so a caller can skip the trigonometry.
bool mouseLookTake(int& deltaX, int& deltaY);

}  // namespace gplvr
