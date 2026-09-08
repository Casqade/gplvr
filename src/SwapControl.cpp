#include "SwapControl.h"

#include "Hooks.h"
#include "Log.h"

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

}  // namespace

void swapControlApply() {
    // -1 means leave the driver default alone, and is the one setting that costs
    // nothing at all: not even the module lookup.
    if (g_options.swapInterval < 0) return;
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

    const int wanted = g_options.swapInterval;
    const BOOL ok = g_swapInterval(wanted);

    // Report what the driver actually did, not what we asked for. Vsync can be forced
    // on in the driver control panel, in which case the call succeeds and the interval
    // stays 1 -- and that is worth seeing in the log, because the symptom (still
    // locked to 72) would otherwise look like this code never ran.
    if (g_getSwapInterval) {
        const int actual = g_getSwapInterval();
        logf("swap: interval %d requested on context %p -- %s, driver reports %d%s",
             wanted, context, ok ? "accepted" : "REFUSED", actual,
             actual == wanted ? "" : " (overridden in the driver control panel?)");
    } else {
        logf("swap: interval %d requested on context %p -- %s (no "
             "wglGetSwapIntervalEXT to confirm with)",
             wanted, context, ok ? "accepted" : "REFUSED");
    }
}

}  // namespace gplvr
