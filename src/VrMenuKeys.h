#pragma once

// The VR menu from the keyboard.
//
// The keys stand in for a controller, so every rule of the menu applies to them unchanged: the
// arrows are the thumbstick (up and down move the highlight, wrapping through the tab strip and
// the button line; left and right change a value, or move along the strip and the button line),
// Enter selects, and Ctrl + vrMenuKey or Esc steps back one level as a grip tap does. Ctrl +
// vrMenuKey opens the menu when it is closed. With Ctrl so the plain key still types into GPL and
// stays free for the debug hotkeys (vrMirrorTweakKey is M too). The quick action menu has no
// keyboard.
//
// GPL takes its keyboard from window messages, so while the menu is open those keys are kept from
// it (vrMenuKeysSwallow), and Ctrl + vrMenuKey always is in VR.

namespace gplvr {

struct VrMenuKeys {
    bool  toggle = false;  // Ctrl + vrMenuKey went down this poll
    bool  back = false;    // Esc went down this poll
    bool  enter = false;   // Enter went down this poll
    float stickX = 0.0f;   // -1 Left, +1 Right, while held
    float stickY = 0.0f;   // +1 Up, -1 Down, while held
};

// vrMenuKey, logged.
void vrMenuKeysLoadOptions(const char* path);

// Once per VR menu poll. Nothing while GPL's window is not in front, so keys typed elsewhere do
// not drive the menu.
VrMenuKeys vrMenuKeysPoll();

// From the game's window procedure: true when this message is one of the menu's keys and must
// not reach GPL. Only presses and their characters; a release always goes through, so GPL's key
// table stays right for a key held across the menu opening or closing.
bool vrMenuKeysSwallow(unsigned message, unsigned wParam);

}  // namespace gplvr
