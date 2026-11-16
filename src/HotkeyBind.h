#pragma once

// gplvr's hotkeys: what each one does, the keyboard key OR the wheel or joystick button that
// does it, and the VR menu's binding (its Controls tab: General and Debug subtabs).
//
// THE TABLE. Every action a player can bind, with what kind of key it takes: vrMenuKey is
// pressed with Ctrl, the debug keys act only with debugKeys (debugKey()), and the two keys that
// enter a keyboard tweak mode may not sit on a key those modes steer with. ONE BINDING PER
// ACTION: a key or a button, never both, in the one gplvr.ini line (below); binding one unbinds
// the other, and while a button is bound the action's key option reads 0. The keys are plain
// keys polled with GetAsyncKeyState, so two on one key would both fire; binding a key that
// another plain hotkey has takes it from that one, which is then unbound. Buttons follow the
// same rule among themselves.
//
// CAPTURE. Pressing a binding's row starts a capture: the next key pressed on the keyboard, seen
// in the game's window procedure, or the next button pressed on a wheel or joystick (Joystick.h)
// becomes the binding. Esc (or the grip, or Ctrl + the menu key) cancels, Backspace or Delete
// unbinds, and a key that cannot be a hotkey is refused with the capture left running. While the capture runs every key press and character stops in the window procedure,
// so the game never sees them; the key or button that ended it stays the capture's until it is
// released, so its repeats, its character and the action it was just bound to do nothing.

#include "Joystick.h"

#include <cstddef>

namespace gplvr {

struct HotkeyBinding {
    const char* name;   // the gplvr.ini key
    const char* label;  // what the player reads in a notice
    int*        value;
    bool        debug;       // acts only with debugKeys
    bool        chord;       // pressed with Ctrl
    bool        modeToggle;  // enters a keyboard tweak mode
};

int                  hotkeyBindingCount();
const HotkeyBinding& hotkeyBinding(int index);

// The binding whose option is `value`, or null.
const HotkeyBinding* hotkeyBindingOf(const int* value);

// The button bound in place of a binding's key; unbound while the action has a key.
const JoyButton& hotkeyButton(const int* value);

// What a key is called on the menu: "M", "Home", "F5", "Ctrl + M"; "None" for 0.
void hotkeyKeyName(int key, bool chord, char* out, size_t size);

// A binding's row value: "3", "G27 Racing Wheel: Btn 6", "None".
void hotkeyBindingText(const int* value, char* out, size_t size);

// After `value` was set to its key by anything but a capture (a row reset): the plain hotkey
// that already had that key is unbound. Writes what it did to `notice`, or "" when nothing.
void hotkeyResolveClash(const int* value, char* notice, size_t size);

// THE INI FORM. One line per action, under the binding's name: `vkey_<n>` for virtual key code
// n (a bare number, as older files have it, reads the same), the button in Joystick.h's form
// ("046D:C24F 0 B6 G27 Racing Wheel"), or `none`. A line that is missing keeps the default; one
// that reads as neither keeps the default and says so. Read by loadOptions before anything else
// looks at a key.
void hotkeyBindingsLoad(const char* path);
void hotkeyBindingIniText(const HotkeyBinding& binding, char* out, size_t size);

// Unbind a binding's button (a row's reset; a button's default is none).
void hotkeyButtonClear(const int* value);

// debugKeys was switched. Off, the keyboard tweak modes are left unsaved and mouse look is
// switched off, since nothing could switch them back.
void hotkeyDebugKeysChanged();

// -- the actions, on the key poll ------------------------------------------------------------

// Once a frame, before any action is asked about: the joysticks, then the camera keys.
void hotkeyPollFrame();

// True on the poll a binding's key or button goes down and the press is the action's: not
// while a capture is busy, not for a key a keyboard tweak mode owns or that GPL's text field is
// taking (TextEntry.h), not for a debug action without debugKeys.
bool hotkeyActionPressed(const int* value);

// The key as the action polls it: debugKey() for a debug action, recentreKey() for recentre.
int hotkeyActionKey(const int* value);

// Whether the binding's button is held, debugKeys applied. For VrMenuKeys' toggle.
bool hotkeyButtonHeld(const int* value);

// -- capture, on the VR menu's poll -----------------------------------------------------------

void hotkeyCaptureBegin(int* value);
void hotkeyCaptureCancel(const char* why);
bool hotkeyCaptureActive();
bool hotkeyCaptureTarget(const int* value);

// Capturing, or the key or button that ended a capture still held: the menu and every hotkey
// stay still.
bool hotkeyCaptureBusy();

// Applies what the window procedure or a joystick took. True when there is something to tell
// the player, in `notice`.
bool hotkeyCapturePoll(char* notice, size_t size);

// -- capture, in the game's window procedure --------------------------------------------------

// True for a message the game must not see.
bool hotkeyCaptureSwallow(unsigned message, unsigned wParam, unsigned lParam);

}  // namespace gplvr
