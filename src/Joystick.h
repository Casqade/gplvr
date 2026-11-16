#pragma once

// Wheels, joysticks and button boxes, read for gplvr's own button bindings (HotkeyBind.h).
//
// DirectInput 8, every attached game controller, opened NON-EXCLUSIVE and in the BACKGROUND, so
// GPL's own use of the same wheel -- force feedback included -- is untouched. Nothing is opened
// until something wants it: a binding loaded from gplvr.ini or a capture in the VR menu. The
// devices are listed once and again after each WM_DEVICECHANGE; everything runs on the poll's
// thread.
//
// A button is named by the device's USB vendor and product IDs, which survive replugging and
// another USB port, and its ordinal among attached devices with the same two IDs, for two of the
// same wheel. Its control is a button (0..127) or one of four directions of a hat (POV), which
// is a button of its own here, so a hat's up and down can be two bindings.

#include <cstddef>
#include <cstdint>

namespace gplvr {

constexpr int kJoyButtons = 128;
constexpr int kJoyHats = 4;
constexpr int kJoyControls = kJoyButtons + kJoyHats * 4;  // hat h direction d: 128 + h * 4 + d

struct JoyButton {
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint8_t  ordinal = 0;
    int      control = -1;  // -1: unbound
    char     device[48] = {};
};

inline bool joyButtonBound(const JoyButton& button) { return button.control >= 0; }
bool joyButtonSame(const JoyButton& a, const JoyButton& b);

// Open DirectInput and list the devices at the next poll. Idempotent.
void joystickWant();

// From the window procedure: a device came or went.
void joystickRequestRescan();

// Once a frame, before anything asks about a button.
void joystickPoll();

// Held as of this frame's poll. False for a device that is not attached.
bool joystickHeld(const JoyButton& button);

// The first button that went down this frame, on any device. For the VR menu's capture.
bool joystickTakePress(JoyButton& out);

// "Btn 6", "Hat 1 Up"; with the device, "G27 Racing Wheel: Btn 6".
void joystickButtonName(const JoyButton& button, bool withDevice, char* out, size_t size);

// gplvr.ini's form: "046D:C24F 0 B6 G27 Racing Wheel" (buttons and hats counted from 1),
// "none" when unbound.
void joystickFormat(const JoyButton& button, char* out, size_t size);
bool joystickParse(const char* text, JoyButton& out);

}  // namespace gplvr
