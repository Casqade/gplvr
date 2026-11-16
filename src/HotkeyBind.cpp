#include "HotkeyBind.h"

#include "CameraConfig.h"
#include "Hooks.h"
#include "Log.h"
#include "Pribluda.h"
#include "TextEntry.h"
#include "VrMenu.h"
#include "Window.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gplvr {
namespace {

const HotkeyBinding kBindings[] = {
    {"vrMenuKey", "VR menu", &g_options.vrMenuKey, false, true, false},
    {"vrRecentreKey", "Recenter", &g_options.vrRecentreKey, false, false, false},
    {"vrCameraForwardKey", "Camera forward", &g_options.vrCameraForwardKey, false, false, false},
    {"vrCameraBackKey", "Camera back", &g_options.vrCameraBackKey, false, false, false},
    {"vrCameraUpKey", "Camera up", &g_options.vrCameraUpKey, false, false, false},
    {"vrCameraDownKey", "Camera down", &g_options.vrCameraDownKey, false, false, false},
    {"vrCameraLeftKey", "Camera left", &g_options.vrCameraLeftKey, false, false, false},
    {"vrCameraRightKey", "Camera right", &g_options.vrCameraRightKey, false, false, false},
    {"vrMirrorTweakKey", "Mirror setup", &g_options.vrMirrorTweakKey, true, false, true},
    {"vrMirrorModeKey", "Mirror mode", &g_options.vrMirrorModeKey, true, false, false},
    {"vrMouseLookKey", "Mouse look", &g_options.vrMouseLookKey, true, false, false},
    {"vrHudLayerKey", "Pribluda panel", &g_options.vrHudLayerKey, true, false, false},
    {"vrHudTweakKey", "Pribluda layout", &g_options.vrHudTweakKey, true, false, true},
};
constexpr int kBindingCount = static_cast<int>(sizeof(kBindings) / sizeof(kBindings[0]));

// The camera actions: which option, which direction, which way.
struct CameraAction {
    const int* value;
    int        direction;
    float      sign;
};
const CameraAction kCameraActions[] = {
    {&g_options.vrCameraForwardKey, kCameraForward, 1.0f},
    {&g_options.vrCameraBackKey, kCameraForward, -1.0f},
    {&g_options.vrCameraUpKey, kCameraUp, 1.0f},
    {&g_options.vrCameraDownKey, kCameraUp, -1.0f},
    {&g_options.vrCameraRightKey, kCameraRight, 1.0f},
    {&g_options.vrCameraLeftKey, kCameraRight, -1.0f},
};

// What the window procedure saw, for the poll to apply. 0 is nothing yet.
constexpr int kResultCancel = -1;
constexpr int kResultClear = -2;
constexpr int kResultRefusedBase = 0x1000;  // + the key: a key that cannot be a hotkey
constexpr int kResultModifierBase = 0x2000; // + the key: pressed with Ctrl or Alt

std::atomic<bool> g_capturing{false};
std::atomic<int>  g_result{0};
std::atomic<bool> g_owned[256];
std::atomic<int*> g_target{nullptr};

// The poll's alone.
JoyButton g_buttons[kBindingCount];
bool      g_last[kBindingCount] = {};
JoyButton g_heldButton;  // the button that ended a capture, until it is let go
bool      g_cameraMoving = false;
LONGLONG  g_cameraPolledAt = 0;

int indexOf(const int* value) {
    for (int i = 0; i < kBindingCount; ++i)
        if (kBindings[i].value == value) return i;
    return -1;
}

bool isModifier(unsigned key) {
    switch (key) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

// Keys no hotkey can have: the VR menu's own (Enter, Esc and the arrows work it), the capture's
// (Backspace and Delete unbind), and keys Windows keeps or toggles a lock with.
bool reservedForAll(unsigned key) {
    if (key < 0x08) return true;  // mouse buttons
    switch (key) {
        case VK_ESCAPE: case VK_RETURN: case VK_BACK: case VK_DELETE:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_APPS: case VK_CAPITAL: case VK_NUMLOCK: case VK_SNAPSHOT:
            return true;
        default:
            return isModifier(key);
    }
}

// The keys the keyboard tweak modes steer with (Hooks.cpp kMirrorTweakKeys, Pribluda's set
// within it). A mode's own toggle there would also steer the other mode.
bool steersTweakMode(unsigned key) {
    return key == VK_PRIOR || key == VK_NEXT || key == VK_HOME;
}

const char* refusal(const HotkeyBinding& binding, unsigned key) {
    if (reservedForAll(key)) return "it works the VR menu or Windows";
    if (binding.modeToggle && steersTweakMode(key)) return "the keyboard setup modes steer with it";
    return nullptr;
}

const HotkeyBinding* targetBinding() {
    int* target = g_target.load();
    return target ? hotkeyBindingOf(target) : nullptr;
}

// Clears ownership of any key no longer physically down: a release the window procedure never
// saw (the window lost the keyboard) must not hold the menu still for ever.
bool anyOwnedHeld() {
    bool held = false;
    for (int key = 0; key < 256; ++key) {
        if (!g_owned[key].load()) continue;
        if (GetAsyncKeyState(key) & 0x8000)
            held = true;
        else
            g_owned[key].store(false);
    }
    return held;
}

void endCapture() {
    g_capturing.store(false);
    g_target.store(nullptr);
}

bool keyHeld(int key) { return key && (GetAsyncKeyState(key) & 0x8000) != 0; }

bool buttonActive(int index) {
    const HotkeyBinding& binding = kBindings[index];
    if (binding.debug && !g_options.debugKeys) return false;
    return joystickHeld(g_buttons[index]);
}

// Sets the key in place of whatever the action had, and takes it from the plain hotkey that
// had it.
void assignKey(const HotkeyBinding& binding, int key, char* notice, size_t size) {
    char name[48] = {};
    hotkeyKeyName(key, binding.chord, name, sizeof(name));
    hotkeyButtonClear(binding.value);
    *binding.value = key;
    logf("hotkeys: %s = 0x%02X (%s)", binding.name, static_cast<unsigned>(key), name);
    char taken[128] = {};
    hotkeyResolveClash(binding.value, taken, sizeof(taken));
    _snprintf_s(notice, size, _TRUNCATE, "Bound to %s%s%s", name, taken[0] ? ". " : "", taken);
}

// Sets the button in place of whatever the action had, and takes it from the action that had
// it.
void assignButton(int index, const JoyButton& button, char* notice, size_t size) {
    char name[96] = {};
    joystickButtonName(button, true, name, sizeof(name));
    *kBindings[index].value = 0;
    g_buttons[index] = button;
    // Held now: it is not a press of the action it was just given.
    g_last[index] = true;
    g_heldButton = button;
    logf("hotkeys: %s's button = %s", kBindings[index].name, name);
    char taken[128] = {};
    for (int i = 0; i < kBindingCount; ++i) {
        if (i == index || !joyButtonSame(g_buttons[i], button)) continue;
        g_buttons[i] = JoyButton{};
        logf("hotkeys: %s's button unbound -- %s took it", kBindings[i].name,
             kBindings[index].name);
        _snprintf_s(taken, sizeof(taken), _TRUNCATE, "%s's button is now unbound",
                    kBindings[i].label);
    }
    _snprintf_s(notice, size, _TRUNCATE, "Bound to %s%s%s", name, taken[0] ? ". " : "", taken);
}

// The camera keys, held: the viewed car's position moves while any is down and is saved when
// the last one is let go.
void pollCamera() {
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    double seconds = g_cameraPolledAt ? static_cast<double>(counter.QuadPart - g_cameraPolledAt) /
                                            static_cast<double>(frequency.QuadPart)
                                      : 0.0;
    g_cameraPolledAt = counter.QuadPart;
    if (seconds > 0.1) seconds = 0.1;

    float move[kCameraDirections] = {};
    bool  any = false;
    bool  usable = g_options.vr && !windowInFrontEnd() && !vrMenuActive() && cameraConfigKey() &&
                   !hotkeyCaptureBusy();
    if (usable) {
        bool typingChecked = false;
        bool typing = false;
        for (const CameraAction& action : kCameraActions) {
            int  index = indexOf(action.value);
            int  key = *action.value;
            bool held = buttonActive(index);
            if (!held && keyHeld(key) && !hotkeyOwnedByMode(static_cast<unsigned>(key))) {
                if (!typingChecked) {
                    typing = gplTextEntryActive();
                    typingChecked = true;
                }
                held = !typing;
            }
            if (!held) continue;
            move[action.direction] += action.sign;
            any = true;
        }
    }
    if (any) {
        if (!g_cameraMoving) logf("camera keys: moving the camera for %s", cameraConfigKey());
        g_cameraMoving = true;
        float step = g_options.vrCameraMoveSpeed * static_cast<float>(seconds);
        for (int direction = 0; direction < kCameraDirections; ++direction)
            if (move[direction] != 0.0f)
                cameraConfigSetValue(direction,
                                     cameraConfigValue(direction) + move[direction] * step);
        return;
    }
    if (g_cameraMoving) {
        g_cameraMoving = false;
        cameraConfigSave();
    }
}

}  // namespace

int hotkeyBindingCount() { return kBindingCount; }

const HotkeyBinding& hotkeyBinding(int index) {
    if (index < 0 || index >= kBindingCount) index = 0;
    return kBindings[index];
}

const HotkeyBinding* hotkeyBindingOf(const int* value) {
    int index = indexOf(value);
    return index >= 0 ? &kBindings[index] : nullptr;
}

const JoyButton& hotkeyButton(const int* value) {
    static const JoyButton none;
    int index = indexOf(value);
    return index >= 0 ? g_buttons[index] : none;
}

void hotkeyKeyName(int key, bool chord, char* out, size_t size) {
    if (!key) {
        _snprintf_s(out, size, _TRUNCATE, "None");
        return;
    }
    char        name[40] = {};
    const char* fixed = nullptr;
    switch (key) {
        case VK_SPACE: fixed = "Space"; break;
        case VK_TAB: fixed = "Tab"; break;
        case VK_HOME: fixed = "Home"; break;
        case VK_END: fixed = "End"; break;
        case VK_INSERT: fixed = "Insert"; break;
        case VK_PRIOR: fixed = "Page Up"; break;
        case VK_NEXT: fixed = "Page Down"; break;
        case VK_PAUSE: fixed = "Pause"; break;
        case VK_SCROLL: fixed = "Scroll Lock"; break;
        case VK_MULTIPLY: fixed = "Num *"; break;
        case VK_ADD: fixed = "Num +"; break;
        case VK_SUBTRACT: fixed = "Num -"; break;
        case VK_DECIMAL: fixed = "Num ."; break;
        case VK_DIVIDE: fixed = "Num /"; break;
        default: break;
    }
    if (fixed) {
        _snprintf_s(name, sizeof(name), _TRUNCATE, "%s", fixed);
    } else if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        _snprintf_s(name, sizeof(name), _TRUNCATE, "%c", key);
    } else if (key >= VK_F1 && key <= VK_F24) {
        _snprintf_s(name, sizeof(name), _TRUNCATE, "F%d", key - VK_F1 + 1);
    } else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        _snprintf_s(name, sizeof(name), _TRUNCATE, "Num %d", key - VK_NUMPAD0);
    } else {
        // The punctuation keys, which move between keyboard layouts: the layout's own name.
        UINT scan = MapVirtualKeyA(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
        if (!scan || !GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)))
            _snprintf_s(name, sizeof(name), _TRUNCATE, "Key 0x%02X", key);
    }
    _snprintf_s(out, size, _TRUNCATE, "%s%s", chord ? "Ctrl + " : "", name);
}

void hotkeyBindingText(const int* value, char* out, size_t size) {
    int index = indexOf(value);
    if (index < 0) {
        out[0] = '\0';
        return;
    }
    const JoyButton& button = g_buttons[index];
    if (!joyButtonBound(button)) {
        hotkeyKeyName(*value, kBindings[index].chord, out, size);
        return;
    }
    // The device's name cut to fit the value column.
    JoyButton shortened = button;
    if (std::strlen(shortened.device) > 16) shortened.device[16] = '\0';
    joystickButtonName(shortened, true, out, size);
}

void hotkeyResolveClash(const int* value, char* notice, size_t size) {
    notice[0] = '\0';
    const HotkeyBinding* changed = hotkeyBindingOf(value);
    if (!changed || changed->chord || !*value) return;
    for (const HotkeyBinding& other : kBindings) {
        if (&other == changed || other.chord || *other.value != *value) continue;
        *other.value = 0;
        logf("hotkeys: %s unbound -- %s took its key", other.name, changed->name);
        _snprintf_s(notice, size, _TRUNCATE, "%s's key is now unbound", other.label);
    }
}

void hotkeyBindingIniText(const HotkeyBinding& binding, char* out, size_t size) {
    int index = indexOf(binding.value);
    if (index >= 0 && joyButtonBound(g_buttons[index])) {
        joystickFormat(g_buttons[index], out, size);
        return;
    }
    if (!*binding.value) {
        _snprintf_s(out, size, _TRUNCATE, "none");
        return;
    }
    _snprintf_s(out, size, _TRUNCATE, "vkey_%d", *binding.value);
}

void hotkeyBindingsLoad(const char* path) {
    // The default is the default-constructed Options' value, at the same offset from its base.
    static const Options defaults;
    int buttons = 0;
    for (int i = 0; i < kBindingCount; ++i) {
        const HotkeyBinding& binding = kBindings[i];
        auto offset = reinterpret_cast<const char*>(binding.value) -
                      reinterpret_cast<const char*>(&g_options);
        int fallback = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(&defaults) +
                                                     offset);
        g_buttons[i] = JoyButton{};
        *binding.value = fallback;

        char text[160] = {};
        GetPrivateProfileStringA("gplvr", binding.name, "", text, sizeof(text), path);
        const char* start = text;
        while (*start == ' ' || *start == '\t') ++start;
        if (!*start) continue;  // not in the file: the default

        // vkey_<code>, or a bare code as older files have it, decimal or 0x hex.
        const char* code = _strnicmp(start, "vkey_", 5) == 0 ? start + 5 : start;
        char*       end = nullptr;
        // Decimal unless written 0x: base 0 would read a leading zero as octal.
        bool        hex = code[0] == '0' && (code[1] == 'x' || code[1] == 'X');
        long        key = std::strtol(code, &end, hex ? 16 : 10);
        bool        numeric = end != code;
        while (numeric && (*end == ' ' || *end == '\t')) ++end;
        if (_stricmp(start, "none") == 0 || (numeric && !*end && key == 0)) {
            *binding.value = 0;
            continue;
        }
        if (numeric && !*end) {
            if (key > 0 && key <= 0xFE) {
                *binding.value = static_cast<int>(key);
                continue;
            }
            logf("options: %s = %s is not a key code (1..254) -- the default 0x%02X", binding.name,
                 start, static_cast<unsigned>(fallback));
            continue;
        }
        if (code == start && joystickParse(start, g_buttons[i])) {
            *binding.value = 0;
            char readable[96] = {};
            joystickButtonName(g_buttons[i], true, readable, sizeof(readable));
            logf("options: %s = %s", binding.name, readable);
            ++buttons;
            continue;
        }
        g_buttons[i] = JoyButton{};
        logf("options: %s = \"%s\" is neither vkey_<code> nor a button (VID:PID ordinal "
             "B<n>|H<n><U|R|D|L> name) -- the default 0x%02X",
             binding.name, start, static_cast<unsigned>(fallback));
    }
    if (buttons) joystickWant();
}

void hotkeyButtonClear(const int* value) {
    int index = indexOf(value);
    if (index < 0 || !joyButtonBound(g_buttons[index])) return;
    g_buttons[index] = JoyButton{};
    logf("hotkeys: %s's button unbound", kBindings[index].name);
}

void hotkeyDebugKeysChanged() {
    logf("hotkeys: debugKeys %s", g_options.debugKeys ? "on -- the debug hotkeys are polled"
                                                      : "off -- the debug hotkeys do nothing");
    if (g_options.debugKeys) return;
    if (mirrorTweakActive()) mirrorTweakExit(" -- debug keys switched off", false);
    if (pribludaTweakActive()) pribludaTweakExit(" -- debug keys switched off");
    if (g_options.vrMouseLook) {
        g_options.vrMouseLook = false;
        logf("hotkeys: mouse look switched off with the debug keys, which had its only switch");
    }
}

void hotkeyPollFrame() {
    joystickPoll();
    pollCamera();
}

int hotkeyActionKey(const int* value) {
    const HotkeyBinding* binding = hotkeyBindingOf(value);
    if (!binding) return 0;
    if (value == &g_options.vrRecentreKey) return recentreKey();
    return binding->debug ? debugKey(*value) : *value;
}

bool hotkeyActionPressed(const int* value) {
    int index = indexOf(value);
    if (index < 0) return false;
    int  key = hotkeyActionKey(value);
    bool viaKey = keyHeld(key);
    bool viaButton = buttonActive(index);
    bool down = viaKey || viaButton;
    bool pressed = down && !g_last[index];
    g_last[index] = down;
    if (!pressed || hotkeyCaptureBusy()) return false;
    if (viaButton) return true;
    if (hotkeyOwnedByMode(static_cast<unsigned>(key))) return false;
    return hotkeyPressAllowed(key, kBindings[index].name);
}

bool hotkeyButtonHeld(const int* value) {
    int index = indexOf(value);
    return index >= 0 && buttonActive(index);
}

void hotkeyCaptureBegin(int* value) {
    const HotkeyBinding* binding = hotkeyBindingOf(value);
    if (!binding) return;
    joystickWant();
    g_target.store(value);
    g_result.store(0);
    g_capturing.store(true);
    logf("hotkeys: waiting for a key or button for %s", binding->name);
}

void hotkeyCaptureCancel(const char* why) {
    if (!g_capturing.load()) return;
    const HotkeyBinding* binding = targetBinding();
    logf("hotkeys: %s left as it was%s", binding ? binding->name : "binding", why ? why : "");
    endCapture();
}

bool hotkeyCaptureActive() { return g_capturing.load(); }

bool hotkeyCaptureTarget(const int* value) {
    return g_capturing.load() && g_target.load() == value;
}

bool hotkeyCaptureBusy() {
    if (joyButtonBound(g_heldButton) && !joystickHeld(g_heldButton)) g_heldButton = JoyButton{};
    return g_capturing.load() || anyOwnedHeld() || joyButtonBound(g_heldButton);
}

bool hotkeyCapturePoll(char* notice, size_t size) {
    notice[0] = '\0';
    if (!g_capturing.load()) return false;
    const HotkeyBinding* binding = targetBinding();
    int                  index = binding ? indexOf(binding->value) : -1;
    if (index < 0) {
        endCapture();
        return false;
    }
    int result = g_result.exchange(0);
    if (!result) {
        JoyButton button;
        if (!joystickTakePress(button)) return false;
        assignButton(index, button, notice, size);
        endCapture();
        return true;
    }

    if (result == kResultCancel) {
        logf("hotkeys: %s left as it was", binding->name);
        _snprintf_s(notice, size, _TRUNCATE, "Binding cancelled");
        endCapture();
        return true;
    }
    if (result == kResultClear) {
        *binding->value = 0;
        hotkeyButtonClear(binding->value);
        logf("hotkeys: %s unbound", binding->name);
        _snprintf_s(notice, size, _TRUNCATE, "Unbound");
        endCapture();
        return true;
    }
    if (result >= kResultModifierBase) {
        _snprintf_s(notice, size, _TRUNCATE,
                    "Hotkeys are single keys: press it without Ctrl or Alt, Esc cancels");
        return true;
    }
    if (result >= kResultRefusedBase) {
        unsigned    key = static_cast<unsigned>(result - kResultRefusedBase);
        const char* why = refusal(*binding, key);
        char        name[48] = {};
        hotkeyKeyName(static_cast<int>(key), false, name, sizeof(name));
        _snprintf_s(notice, size, _TRUNCATE, "%s can't be a hotkey: %s. Press another key",
                    name, why ? why : "reserved");
        return true;
    }
    assignKey(*binding, result, notice, size);
    endCapture();
    return true;
}

bool hotkeyCaptureSwallow(unsigned message, unsigned wParam, unsigned lParam) {
    bool capturing = g_capturing.load();
    switch (message) {
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // Releases go on to the game, which keeps its key table right.
            if (wParam < 256) g_owned[wParam].store(false);
            return false;
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            if (capturing) return true;
            for (const std::atomic<bool>& owned : g_owned)
                if (owned.load()) return true;
            return false;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            break;
        default:
            return false;
    }
    if (wParam >= 256) return capturing;
    if (g_owned[wParam].load()) return true;
    if (!capturing) return false;
    // A repeat of a key held when the capture began: the Enter that pressed the row.
    if (lParam & (1u << 30)) return true;
    g_owned[wParam].store(true);
    if (isModifier(wParam)) return true;  // waiting for the key it is held with
    if (g_result.load()) return true;     // the poll has not taken the last one yet

    const HotkeyBinding* binding = targetBinding();
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    int  result = 0;
    if (wParam == VK_ESCAPE || (ctrl && wParam == static_cast<unsigned>(g_options.vrMenuKey)))
        result = kResultCancel;
    else if (wParam == VK_BACK || wParam == VK_DELETE)
        result = kResultClear;
    else if (ctrl || alt)
        result = kResultModifierBase + static_cast<int>(wParam);
    else if (!binding || refusal(*binding, wParam))
        result = kResultRefusedBase + static_cast<int>(wParam);
    else
        result = static_cast<int>(wParam);
    g_result.store(result);
    return true;
}

}  // namespace gplvr
