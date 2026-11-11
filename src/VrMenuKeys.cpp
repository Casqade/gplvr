#include "VrMenuKeys.h"

#include "Hooks.h"
#include "Log.h"
#include "VrMenu.h"

#include <windows.h>

#include <atomic>

namespace gplvr {
namespace {

struct State {
    bool toggleDown = false;
    bool backDown = false;
    bool enterDown = false;
};

State g_state;

// A KEY THE MENU USED IS THE MENU'S UNTIL IT IS RELEASED. The poll and the window procedure see
// the same press in either order: an Esc that closes the menu in the poll would otherwise reach
// GPL a moment later as its own Esc, with the menu already shut. Set by whichever sees the press
// with the menu open, cleared by the key's WM_KEYUP; every WM_KEYDOWN (auto-repeat included) and
// character of an owned key stops here.
std::atomic<bool> g_owned[256];

bool held(int key) { return key && (GetAsyncKeyState(key) & 0x8000) != 0; }

bool ctrlHeld() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }

// The character TranslateMessage makes of Ctrl and the toggle key: a letter's control code, which
// for M is 13, the same as Enter's. False for a key with no control character.
bool isToggleCharacter(unsigned character) {
    int key = g_options.vrMenuKey;
    return key >= 'A' && key <= 'Z' && character == static_cast<unsigned>(key - 'A' + 1);
}

bool isMenuKey(unsigned key) {
    return key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT ||
           key == VK_RETURN || key == VK_ESCAPE;
}

void own(unsigned key) {
    if (key < 256) g_owned[key].store(true);
}

bool owned(unsigned key) { return key < 256 && g_owned[key].load(); }

}  // namespace

void vrMenuKeysLoadOptions(const char* path) {
    g_options.vrMenuKey = GetPrivateProfileIntA("gplvr", "vrMenuKey", 'M', path);
    if (g_options.vrMenuKey < 0 || g_options.vrMenuKey > 0xFE) g_options.vrMenuKey = 0;
    if (g_options.vrMenuKey)
        logf("options: vrMenuKey=0x%02X ('%c') -- Ctrl and that key open the VR menu and step "
             "back out; inside it the arrows move and change, Enter selects, Esc steps back",
             g_options.vrMenuKey,
             (g_options.vrMenuKey >= 32 && g_options.vrMenuKey < 127) ? g_options.vrMenuKey : '?');
    else
        logf("options: vrMenuKey=0 -- the VR menu has no keyboard");
}

VrMenuKeys vrMenuKeysPoll() {
    State&     s = g_state;
    VrMenuKeys keys;
    if (!g_options.vr || !g_options.vrMenuKey || GetForegroundWindow() != gameWindow()) {
        s.toggleDown = s.backDown = s.enterDown = false;
        return keys;
    }

    bool open = vrMenuActive();
    bool toggle = held(g_options.vrMenuKey) && held(VK_CONTROL);
    bool back = held(VK_ESCAPE);
    bool enter = held(VK_RETURN);
    // Esc and Enter mean something only with the menu open; with it shut they are GPL's.
    keys.toggle = toggle && !s.toggleDown;
    keys.back = open && back && !s.backDown;
    keys.enter = open && enter && !s.enterDown;
    if (keys.back) own(VK_ESCAPE);
    if (keys.enter) own(VK_RETURN);
    s.toggleDown = toggle;
    s.backDown = back;
    s.enterDown = enter;

    if (open) {
        // Both of a pair held cancel, as a stick cannot point two ways.
        keys.stickX = (held(VK_RIGHT) ? 1.0f : 0.0f) - (held(VK_LEFT) ? 1.0f : 0.0f);
        keys.stickY = (held(VK_UP) ? 1.0f : 0.0f) - (held(VK_DOWN) ? 1.0f : 0.0f);
        for (unsigned key : {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN})
            if (held(static_cast<int>(key))) own(key);
    }
    return keys;
}

bool vrMenuKeysSwallow(unsigned message, unsigned wParam) {
    if (!g_options.vr || !g_options.vrMenuKey) return false;
    if (message == WM_KEYUP) {
        if (wParam < 256) g_owned[wParam].store(false);
        return false;
    }
    unsigned menuKey = static_cast<unsigned>(g_options.vrMenuKey);
    if (message == WM_KEYDOWN) {
        // Plain, the key is GPL's; with Ctrl it is the menu's until it is released, so its
        // control character is recognised below as the toggle's and not taken for Enter.
        if (wParam == menuKey && ctrlHeld()) own(wParam);
        if (isMenuKey(wParam) && vrMenuActive()) own(wParam);
        return owned(wParam);
    }
    if (message == WM_CHAR) {
        if (isToggleCharacter(wParam) && owned(menuKey)) return true;
        if (wParam == '\r') return owned(VK_RETURN);
        if (wParam == 27) return owned(VK_ESCAPE);
    }
    return false;
}

}  // namespace gplvr
