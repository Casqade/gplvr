#include "Joystick.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include "Hooks.h"
#include "Log.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

constexpr int kMaxDevices = 16;

struct Device {
    LPDIRECTINPUTDEVICE8A device = nullptr;
    uint16_t              vendor = 0;
    uint16_t              product = 0;
    uint8_t               ordinal = 0;
    char                  name[48] = {};
    bool                  sampled = false;  // the first sample sets `held` without edges
    bool                  held[kJoyControls] = {};
    bool                  pressed[kJoyControls] = {};
    unsigned              failures = 0;
};

const char* const kHatDirection[4] = {"Up", "Right", "Down", "Left"};
const char        kHatLetter[4] = {'U', 'R', 'D', 'L'};

bool               g_wanted = false;
bool               g_failed = false;  // DirectInput refused: never asked again
std::atomic<bool>  g_rescan{false};
LPDIRECTINPUT8A    g_input = nullptr;
Device             g_devices[kMaxDevices];
int                g_deviceCount = 0;

void releaseDevices() {
    for (int i = 0; i < g_deviceCount; ++i) {
        if (g_devices[i].device) {
            g_devices[i].device->Unacquire();
            g_devices[i].device->Release();
        }
        g_devices[i] = Device{};
    }
    g_deviceCount = 0;
}

BOOL CALLBACK enumDevice(LPCDIDEVICEINSTANCEA instance, LPVOID) {
    if (g_deviceCount >= kMaxDevices) return DIENUM_STOP;
    Device& d = g_devices[g_deviceCount];
    d = Device{};
    // HID devices carry their IDs in the product GUID: Data1 is MAKELONG(vendor, product).
    d.vendor = LOWORD(instance->guidProduct.Data1);
    d.product = HIWORD(instance->guidProduct.Data1);
    for (int i = 0; i < g_deviceCount; ++i)
        if (g_devices[i].vendor == d.vendor && g_devices[i].product == d.product) ++d.ordinal;
    _snprintf_s(d.name, sizeof(d.name), _TRUNCATE, "%s", instance->tszProductName);

    HWND window = gameWindow();
    HRESULT result = g_input->CreateDevice(instance->guidInstance, &d.device, nullptr);
    if (SUCCEEDED(result)) result = d.device->SetDataFormat(&c_dfDIJoystick2);
    if (SUCCEEDED(result))
        result = d.device->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (FAILED(result)) {
        logf("joystick: %s (%04X:%04X) could not be opened (0x%08lX) -- its buttons are not read",
             d.name, d.vendor, d.product, static_cast<unsigned long>(result));
        if (d.device) d.device->Release();
        d = Device{};
        return DIENUM_CONTINUE;
    }
    d.device->Acquire();
    logf("joystick: %s (%04X:%04X #%u) opened", d.name, d.vendor, d.product, d.ordinal);
    ++g_deviceCount;
    return DIENUM_CONTINUE;
}

void scan() {
    releaseDevices();
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    HRESULT result =
        g_input->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevice, nullptr, DIEDFL_ATTACHEDONLY);
    QueryPerformanceCounter(&end);
    logf("joystick: %d game controller(s) attached, listed in %.1f ms%s", g_deviceCount,
         1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
             static_cast<double>(frequency.QuadPart),
         FAILED(result) ? " (the listing failed)" : "");
}

bool open() {
    if (g_input) return true;
    if (g_failed || !gameWindow()) return false;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&open), &self);
    HRESULT result = DirectInput8Create(self, DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                        reinterpret_cast<void**>(&g_input), nullptr);
    if (FAILED(result)) {
        g_failed = true;
        g_input = nullptr;
        logf("joystick: DirectInput8Create refused (0x%08lX) -- wheel and joystick bindings do "
             "nothing this session",
             static_cast<unsigned long>(result));
        return false;
    }
    g_rescan.store(true);
    return true;
}

void sample(Device& d) {
    DIJOYSTATE2 state = {};
    HRESULT     result = d.device->Poll();
    if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED) {
        d.device->Acquire();
        result = d.device->Poll();
    }
    result = d.device->GetDeviceState(sizeof(state), &state);
    if (FAILED(result)) {
        // Released, not pressed: a device that stops answering must not leave a binding held.
        std::memset(d.held, 0, sizeof(d.held));
        std::memset(d.pressed, 0, sizeof(d.pressed));
        if (++d.failures == 1)
            logf("joystick: %s stopped answering (0x%08lX) -- its buttons read released", d.name,
                 static_cast<unsigned long>(result));
        d.device->Acquire();
        return;
    }
    d.failures = 0;
    bool now[kJoyControls] = {};
    for (int b = 0; b < kJoyButtons; ++b) now[b] = (state.rgbButtons[b] & 0x80) != 0;
    for (int h = 0; h < kJoyHats; ++h) {
        DWORD pov = state.rgdwPOV[h];
        if (LOWORD(pov) == 0xFFFF) continue;  // centred
        // Each direction takes 135 degrees, so a diagonal holds both its neighbours.
        bool* hat = now + kJoyButtons + h * 4;
        hat[0] = pov >= 29250 || pov <= 6750;
        hat[1] = pov >= 2250 && pov <= 15750;
        hat[2] = pov >= 11250 && pov <= 24750;
        hat[3] = pov >= 20250 && pov <= 33750;
    }
    for (int c = 0; c < kJoyControls; ++c) {
        d.pressed[c] = d.sampled && now[c] && !d.held[c];
        d.held[c] = now[c];
    }
    d.sampled = true;
}

const Device* find(const JoyButton& button) {
    for (int i = 0; i < g_deviceCount; ++i) {
        const Device& d = g_devices[i];
        if (d.vendor == button.vendor && d.product == button.product &&
            d.ordinal == button.ordinal)
            return &d;
    }
    return nullptr;
}

}  // namespace

bool joyButtonSame(const JoyButton& a, const JoyButton& b) {
    return joyButtonBound(a) && a.control == b.control && a.vendor == b.vendor &&
           a.product == b.product && a.ordinal == b.ordinal;
}

void joystickWant() {
    if (g_wanted) return;
    g_wanted = true;
    logf("joystick: wanted -- wheels and joysticks are read for gplvr's button bindings");
}

void joystickRequestRescan() { g_rescan.store(true); }

void joystickPoll() {
    if (!g_wanted || !open()) return;
    if (g_rescan.exchange(false)) scan();
    for (int i = 0; i < g_deviceCount; ++i) sample(g_devices[i]);
}

bool joystickHeld(const JoyButton& button) {
    if (!joyButtonBound(button) || button.control >= kJoyControls) return false;
    const Device* d = find(button);
    return d && d->held[button.control];
}

bool joystickTakePress(JoyButton& out) {
    for (int i = 0; i < g_deviceCount; ++i) {
        const Device& d = g_devices[i];
        for (int c = 0; c < kJoyControls; ++c) {
            if (!d.pressed[c]) continue;
            out = JoyButton{};
            out.vendor = d.vendor;
            out.product = d.product;
            out.ordinal = d.ordinal;
            out.control = c;
            _snprintf_s(out.device, sizeof(out.device), _TRUNCATE, "%s", d.name);
            return true;
        }
    }
    return false;
}

void joystickButtonName(const JoyButton& button, bool withDevice, char* out, size_t size) {
    if (!joyButtonBound(button)) {
        _snprintf_s(out, size, _TRUNCATE, "None");
        return;
    }
    char control[24] = {};
    if (button.control < kJoyButtons) {
        _snprintf_s(control, sizeof(control), _TRUNCATE, "Btn %d", button.control + 1);
    } else {
        int hat = (button.control - kJoyButtons) / 4;
        int direction = (button.control - kJoyButtons) % 4;
        _snprintf_s(control, sizeof(control), _TRUNCATE, "Hat %d %s", hat + 1,
                    kHatDirection[direction]);
    }
    if (withDevice)
        _snprintf_s(out, size, _TRUNCATE, "%s: %s", button.device[0] ? button.device : "Device",
                    control);
    else
        _snprintf_s(out, size, _TRUNCATE, "%s", control);
}

void joystickFormat(const JoyButton& button, char* out, size_t size) {
    if (!joyButtonBound(button)) {
        _snprintf_s(out, size, _TRUNCATE, "none");
        return;
    }
    char control[8] = {};
    if (button.control < kJoyButtons) {
        _snprintf_s(control, sizeof(control), _TRUNCATE, "B%d", button.control + 1);
    } else {
        int hat = (button.control - kJoyButtons) / 4;
        int direction = (button.control - kJoyButtons) % 4;
        _snprintf_s(control, sizeof(control), _TRUNCATE, "H%d%c", hat + 1, kHatLetter[direction]);
    }
    _snprintf_s(out, size, _TRUNCATE, "%04X:%04X %u %s %s", button.vendor, button.product,
                button.ordinal, control, button.device);
}

bool joystickParse(const char* text, JoyButton& out) {
    out = JoyButton{};
    unsigned vendor = 0;
    unsigned product = 0;
    unsigned ordinal = 0;
    char     control[8] = {};
    int      consumed = 0;
    if (sscanf_s(text, " %x:%x %u %7s %n", &vendor, &product, &ordinal, control,
                 static_cast<unsigned>(sizeof(control)), &consumed) < 4)
        return false;
    if (vendor > 0xFFFF || product > 0xFFFF || ordinal > 255) return false;
    int value = 0;
    if ((control[0] == 'B' || control[0] == 'b') && sscanf_s(control + 1, "%d", &value) == 1 &&
        value >= 1 && value <= kJoyButtons) {
        out.control = value - 1;
    } else if ((control[0] == 'H' || control[0] == 'h') && control[1] >= '1' &&
               control[1] < '1' + kJoyHats) {
        int direction = -1;
        for (int d = 0; d < 4; ++d)
            if (control[2] == kHatLetter[d] || control[2] == kHatLetter[d] + ('a' - 'A'))
                direction = d;
        if (direction < 0) return false;
        out.control = kJoyButtons + (control[1] - '1') * 4 + direction;
    } else {
        return false;
    }
    out.vendor = static_cast<uint16_t>(vendor);
    out.product = static_cast<uint16_t>(product);
    out.ordinal = static_cast<uint8_t>(ordinal);
    _snprintf_s(out.device, sizeof(out.device), _TRUNCATE, "%s", text + consumed);
    // The name runs to the end of the line; trailing blanks are not part of it.
    size_t length = std::strlen(out.device);
    while (length && (out.device[length - 1] == ' ' || out.device[length - 1] == '\t'))
        out.device[--length] = '\0';
    return true;
}

}  // namespace gplvr
