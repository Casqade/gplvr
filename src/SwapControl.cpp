#include "SwapControl.h"

#include "Hooks.h"
#include "Log.h"

#include <cstring>

namespace gplvr {
namespace {

typedef HGLRC(WINAPI* WglGetCurrentContextFn)();
typedef PROC(WINAPI* WglGetProcAddressFn)(LPCSTR);
typedef BOOL(WINAPI* WglSwapIntervalExtFn)(int);
typedef int(WINAPI* WglGetSwapIntervalExtFn)();

WglGetCurrentContextFn g_getCurrentContext = nullptr;
WglGetProcAddressFn    g_getProcAddress = nullptr;
WglSwapIntervalExtFn   g_swapInterval = nullptr;
WglGetSwapIntervalExtFn g_getSwapInterval = nullptr;

bool  g_resolveTried = false;
HGLRC g_appliedTo = nullptr;

// GetModuleHandle, not LoadLibrary: no opengl32 in the process means the D3D7
// rasterizer is running and there is no swap interval to set. Loading it ourselves
// would only manufacture a context-less failure, the same reasoning as GlInterop.
bool resolveBase() {
    if (g_resolveTried) return g_getCurrentContext && g_getProcAddress;
    g_resolveTried = true;

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) {
        logf("swap: opengl32 is not loaded -- no swap interval to set (D3D7 rasterizer?)");
        return false;
    }
    g_getCurrentContext =
        reinterpret_cast<WglGetCurrentContextFn>(GetProcAddress(gl, "wglGetCurrentContext"));
    g_getProcAddress =
        reinterpret_cast<WglGetProcAddressFn>(GetProcAddress(gl, "wglGetProcAddress"));
    if (!g_getCurrentContext || !g_getProcAddress)
        logf("swap: opengl32 is loaded but wglGetCurrentContext/wglGetProcAddress did "
             "not resolve");
    return g_getCurrentContext && g_getProcAddress;
}

// Whether the current context's WGL extension string names WGL_EXT_swap_control_tear. Asked
// through the EXT query, which takes no arguments, and then through the ARB one, which wants
// the context's device.
bool tearSupported() {
    typedef const char*(WINAPI * WglGetExtensionsStringExtFn)();
    typedef const char*(WINAPI * WglGetExtensionsStringArbFn)(HDC);
    typedef HDC(WINAPI * WglGetCurrentDcFn)();

    const char* extensions = nullptr;
    auto        queryExt = reinterpret_cast<WglGetExtensionsStringExtFn>(
        g_getProcAddress("wglGetExtensionsStringEXT"));
    if (queryExt) extensions = queryExt();
    if (!extensions) {
        auto queryArb = reinterpret_cast<WglGetExtensionsStringArbFn>(
            g_getProcAddress("wglGetExtensionsStringARB"));
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        auto    currentDc = gl ? reinterpret_cast<WglGetCurrentDcFn>(
                                     GetProcAddress(gl, "wglGetCurrentDC"))
                               : nullptr;
        HDC dc = currentDc ? currentDc() : nullptr;
        if (queryArb && dc) extensions = queryArb(dc);
    }
    return extensions && std::strstr(extensions, "WGL_EXT_swap_control_tear") != nullptr;
}

}  // namespace

void swapControlApply() {
    // RESOLVED -1: make no call, which is what vanilla does and the only outcome we cannot
    // report. Said once, because "no swap: line at all" and "the feature never installed"
    // look identical in a log while the difference is half the frame rate.
    if (g_options.swapInterval < 0) {
        static bool said = false;
        if (!said) {
            said = true;
            logf("swap: no call is made, so whatever the driver or its control panel decided "
                 "stands. WGL specifies the initial value as 1, i.e. vsync ON, and nothing in "
                 "GPL changes it -- so on a high-refresh display expect the rate to quantise "
                 "to the refresh. This is vanilla behaviour and frameRate = 0 asks for it; "
                 "swapInterval 0 or 1 in the ini overrides it and logs what the driver did.");
        }
        return;
    }
    if (!resolveBase()) return;

    HGLRC context = g_getCurrentContext();
    if (!context || context == g_appliedTo) return;

    // wglGetProcAddress is per-context by specification, so this is re-resolved for
    // each new context rather than cached across them.
    g_swapInterval =
        reinterpret_cast<WglSwapIntervalExtFn>(g_getProcAddress("wglSwapIntervalEXT"));
    g_getSwapInterval = reinterpret_cast<WglGetSwapIntervalExtFn>(
        g_getProcAddress("wglGetSwapIntervalEXT"));

    // Mark the context done either way. A driver without the extension will not grow
    // one, and retrying every frame would only repeat the log line.
    g_appliedTo = context;

    if (!g_swapInterval) {
        logf("swap: WGL_EXT_swap_control is not available -- vsync stays at the driver "
             "default and the frame rate will keep quantising to the refresh");
        return;
    }

    // Adaptive vsync is the same interval negated, and only WGL_EXT_swap_control_tear makes a
    // negative interval valid, so the extension is looked for first.
    int wanted = g_options.swapInterval;
    if (g_options.swapTear && wanted > 0) {
        if (tearSupported()) {
            wanted = -wanted;
        } else {
            logf("swap: this driver does not offer WGL_EXT_swap_control_tear, so adaptive vsync "
                 "(swapInterval = -2) falls back to plain vsync");
        }
    }
    BOOL ok = g_swapInterval(wanted);
    if (!ok && wanted < 0) {
        logf("swap: the driver refused interval %d -- asking for %d instead", wanted, -wanted);
        wanted = -wanted;
        ok = g_swapInterval(wanted);
    }

    // Report what the driver actually did, not what we asked for. Vsync can be forced
    // on in the driver control panel, in which case the call succeeds and the interval
    // stays 1 -- and that is worth seeing in the log, because the symptom (still
    // locked to 72) would otherwise look like this code never ran.
    //
    // Drivers disagree on what they report for a tearing interval: some give the negative
    // value back and some its magnitude, so either reads as the request having stood.
    if (g_getSwapInterval) {
        const int actual = g_getSwapInterval();
        bool      stood = actual == wanted || (wanted < 0 && actual == -wanted);
        logf("swap: interval %d requested on context %p -- %s, driver reports %d%s",
             wanted, context, ok ? "accepted" : "REFUSED", actual,
             stood ? "" : " (overridden in the driver control panel?)");
    } else {
        logf("swap: interval %d requested on context %p -- %s (no "
             "wglGetSwapIntervalEXT to confirm with)",
             wanted, context, ok ? "accepted" : "REFUSED");
    }
}

}  // namespace gplvr
