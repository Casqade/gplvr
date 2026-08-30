#include "MouseLook.h"

#include "Log.h"

#include <windows.h>

namespace gplvr {
namespace {

// Written by the input thread, drained by the game thread. Interlocked because those are
// two different threads and a torn read here would show up as a random flick of the view
// -- indistinguishable, at a glance, from the very defect this is meant to help find.
volatile LONG g_deltaX = 0;
volatile LONG g_deltaY = 0;

HWND   g_window = nullptr;
HANDLE g_thread = nullptr;
bool   g_attempted = false;
bool   g_ready = false;

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INPUT) {
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER));

        RAWINPUT raw = {};
        if (size && size <= sizeof(raw) &&
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) == size &&
            raw.header.dwType == RIM_TYPEMOUSE) {
            // Absolute reports come from tablets and some remote-desktop and VM mouse
            // drivers, where lLastX/lLastY are screen coordinates rather than movement.
            // Adding those as deltas would fling the view across the world on the first
            // report, so skip them rather than pretend they are the same quantity.
            if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                InterlockedExchangeAdd(&g_deltaX, raw.data.mouse.lLastX);
                InterlockedExchangeAdd(&g_deltaY, raw.data.mouse.lLastY);
            }
        }
        // Raw input requires the default handler to run for cleanup even when the
        // message has been dealt with.
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI threadProc(LPVOID) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"gplvrMouseLook";
    RegisterClassExW(&windowClass);  // a duplicate registration is not an error here

    // HWND_MESSAGE: no pixels, no taskbar entry, no z-order, and invisible to anything
    // enumerating top-level windows. It exists only to be a delivery address for WM_INPUT.
    g_window = CreateWindowExW(0, windowClass.lpszClassName, L"gplvr mouse look", 0, 0, 0,
                               0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance,
                               nullptr);
    if (!g_window) {
        logf("mouselook: could not create the message window (error %lu) -- mouse look "
             "is unavailable this run",
             GetLastError());
        return 1;
    }

    RAWINPUTDEVICE device = {};
    device.usUsagePage = 0x01;  // generic desktop controls
    device.usUsage = 0x02;      // mouse
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = g_window;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        logf("mouselook: RegisterRawInputDevices failed (error %lu) -- mouse look is "
             "unavailable this run",
             GetLastError());
        DestroyWindow(g_window);
        g_window = nullptr;
        return 1;
    }

    g_ready = true;
    logf("mouselook: raw mouse input registered. Movement now drives the camera and the "
         "cursor is not touched, so the game's own mouse handling is unaffected.");

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

}  // namespace

bool mouseLookEnsureStarted() {
    if (g_attempted) return g_ready;
    g_attempted = true;

    g_thread = CreateThread(nullptr, 0, &threadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        logf("mouselook: CreateThread failed (error %lu)", GetLastError());
        return false;
    }

    // The window and the registration happen on that thread, so "ready" is not true the
    // instant this returns. Nothing depends on it being ready -- mouseLookTake simply
    // reports no movement until it is -- so there is no handshake to wait on.
    return true;
}

bool mouseLookTake(int& deltaX, int& deltaY) {
    deltaX = static_cast<int>(InterlockedExchange(&g_deltaX, 0));
    deltaY = static_cast<int>(InterlockedExchange(&g_deltaY, 0));
    return deltaX != 0 || deltaY != 0;
}

}  // namespace gplvr
