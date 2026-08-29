#include "Hooks.h"

#include "GlCapture.h"
#include "Log.h"
#include "Stereo.h"
#include "VrCamera.h"
#include "presenter/Producer.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace gplvr {

Options g_options;

namespace {

// screenDisplay is called as `push surface; call [global]; add esp,4`.
typedef int(__cdecl* ScreenDisplayFn)(void* surface);
// sceneRender is called as `lea eax,[obj+0x51]; push eax; call rel32; add esp,4`.
typedef void(__cdecl* SceneRenderFn)(void* params);

ScreenDisplayFn g_originalScreenDisplay = nullptr;
SceneRenderFn   g_originalSceneRender = nullptr;

LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_windowStart{};

unsigned  g_frames = 0;
unsigned  g_sceneHookCalls = 0;  // times our thunk ran (frames that render the world)
unsigned  g_sceneOrigCalls = 0;  // times we called the engine's sceneRender
long long g_sceneTicks = 0;      // total time inside the original, all invocations
long long g_sceneTicksWorst = 0;
bool      g_stereoKeyLast = false;
bool      g_vrStarted = false;
bool      g_vrRecentreKeyLast = false;
bool      g_vrDumpKeyLast = false;
bool      g_vrFovProbeKeyLast = false;
bool      g_vrRotateResetKeyLast = false;
bool      g_vrRotateSweeping = false;
DWORD     g_vrRotateLastTick = 0;

// Set by the scene hook when stereoRender actually split the frame into two eyes.
// Read and cleared by the frame hook.
//
// This is NOT the same as "the stereo option is on". Menus are drawn straight to the
// back buffer without a world render, so nothing splits them -- but telling the
// presenter side-by-side anyway made it cut the menu in half and blow one half up to
// fill the headset. What matters is whether THIS frame has two eyes in it.
bool      g_stereoRenderedFrame = false;

std::vector<uintptr_t> g_pendingCallSites;
volatile bool          g_sceneHookPending = false;

typedef void(__cdecl* CameraBindFn)(void*);
CameraBindFn           g_originalCameraBind = nullptr;
std::vector<uintptr_t> g_pendingBindCallSites;

void reportOncePerSecond() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const long long elapsed = now.QuadPart - g_windowStart.QuadPart;
    if (elapsed < g_frequency.QuadPart) return;

    const double seconds = static_cast<double>(elapsed) / g_frequency.QuadPart;
    // Per *original* invocation, so the number is directly comparable between mono
    // and stereo.
    const double msPerRender =
        g_sceneOrigCalls
            ? (1000.0 * g_sceneTicks / g_frequency.QuadPart) / g_sceneOrigCalls
            : 0.0;
    const double worstMs = 1000.0 * g_sceneTicksWorst / g_frequency.QuadPart;

    logf("fps %.1f | worldFrames %.1f/s, renders %.1f/s (x%.2f), %.2f ms each, "
         "%.2f ms worst | mode %s",
         g_frames / seconds, g_sceneHookCalls / seconds, g_sceneOrigCalls / seconds,
         g_sceneHookCalls ? static_cast<double>(g_sceneOrigCalls) / g_sceneHookCalls : 0.0,
         msPerRender, worstMs, g_options.stereo ? "stereo" : "single");

    g_windowStart = now;
    g_frames = 0;
    g_sceneHookCalls = 0;
    g_sceneOrigCalls = 0;
    g_sceneTicks = 0;
    g_sceneTicksWorst = 0;
}

void pollToggle() {
    if (g_options.stereoKey) {
        const bool down = (GetAsyncKeyState(g_options.stereoKey) & 0x8000) != 0;
        if (down && !g_stereoKeyLast) {
            g_options.stereo = !g_options.stereo;
            logf("stereo toggled %s%s", g_options.stereo ? "ON" : "off",
                 (g_options.stereo && !stereoAvailable()) ? " (but unavailable -- see the "
                                                            "discovery lines above)"
                                                          : "");
            // Hand the eye surfaces back while stereo is off rather than holding two
            // rasterizer allocations the game is not using.
            if (!g_options.stereo) stereoRelease();
        }
        g_stereoKeyLast = down;
    }

    // Recentre: whatever pose the driver is in becomes the seat. Needed because
    // OpenXR's LOCAL space has its origin on the floor, not at the head -- measured,
    // see STATUS.md -- so without this the view starts a standing height too high.
    if (g_options.vr && g_options.vrRecentreKey) {
        const bool down = (GetAsyncKeyState(g_options.vrRecentreKey) & 0x8000) != 0;
        if (down && !g_vrRecentreKeyLast) vrRequestRecentre();
        g_vrRecentreKeyLast = down;
    }

    // Dump the head matrix and the eye offsets once. Press it while LOOKING at whatever
    // looks wrong: a matrix that has stopped being a rotation shears the world in
    // proportion to how far the head is turned, so facing forward would tell you nothing.
    if (g_options.vr && g_options.vrDumpKey) {
        const bool down = (GetAsyncKeyState(g_options.vrDumpKey) & 0x8000) != 0;
        if (down && !g_vrDumpKeyLast) vrRequestDump();
        g_vrDumpKeyLast = down;
    }

    // Sweep the fixed debug rotation while a key is held.
    //
    // Rate-based rather than per-frame, so the sweep is the same speed whether the game
    // is managing 36 fps or stalling, and so it stays comparable between runs.
    if (g_options.vr && vrDebugRotationActive()) {
        const DWORD now = GetTickCount();
        DWORD       elapsed = now - g_vrRotateLastTick;
        g_vrRotateLastTick = now;
        // A first call, or a long stall, must not teleport the angle.
        if (elapsed > 250) elapsed = 0;

        const float seconds = static_cast<float>(elapsed) / 1000.0f;
        const float step = g_options.vrDebugRotateRate * seconds;

        const auto held = [](int key) {
            return key && (GetAsyncKeyState(key) & 0x8000) != 0;
        };

        float yaw = 0.0f;
        float pitch = 0.0f;
        if (held(g_options.vrDebugYawLeftKey)) yaw += step;
        if (held(g_options.vrDebugYawRightKey)) yaw -= step;
        if (held(g_options.vrDebugPitchUpKey)) pitch += step;
        if (held(g_options.vrDebugPitchDownKey)) pitch -= step;

        if (yaw != 0.0f || pitch != 0.0f) {
            vrDebugRotateBy(yaw, pitch);
            g_vrRotateSweeping = true;
        } else if (g_vrRotateSweeping) {
            // Report where the sweep stopped, so a screenshot has an angle against it.
            g_vrRotateSweeping = false;
            float atYaw = 0.0f, atPitch = 0.0f;
            vrDebugRotationAngles(atYaw, atPitch);
            logf("vr: debug rotation now yaw %.1f deg, pitch %.1f deg", atYaw, atPitch);
        }

        const bool reset = held(g_options.vrDebugRotateResetKey);
        if (reset && !g_vrRotateResetKeyLast) {
            vrDebugRotateReset();
            logf("vr: debug rotation reset to the starting angles");
        }
        g_vrRotateResetKeyLast = reset;
    }

    // Render one frame twice over, same viewpoint, two frusta an octave apart, and dump
    // both halves. Whether the fov we write reaches the picture stops being a matter of
    // opinion at that point.
    if (g_options.vr && g_options.vrFovProbeKey) {
        const bool down = (GetAsyncKeyState(g_options.vrFovProbeKey) & 0x8000) != 0;
        if (down && !g_vrFovProbeKeyLast) stereoRequestFovProbe();
        g_vrFovProbeKeyLast = down;
    }
}

void applyPendingSceneRenderHook();  // defined below
void enforceNotTopmost();            // defined below

int __cdecl screenDisplayHook(void* surface) {
    // We are on the game's render thread, between frames: sceneRender has already
    // returned and is not on any stack, so its call sites are safe to rewrite.
    applyPendingSceneRenderHook();

    ++g_frames;
    pollToggle();
    enforceNotTopmost();
    reportOncePerSecond();

    // Before the original, not after: the back buffer still holds the finished frame
    // at this point, which is the whole reason the transport probe lives here.
    if (g_options.glProbe) glProbeFrame();

    // Same reason, same moment: the shared texture is filled from the back buffer
    // before the game presents it. presenterStart needs a current GL context, which is
    // why it starts here rather than at discovery time.
    if (g_options.vr) {
        if (!g_vrStarted) {
            g_vrStarted = true;
            presenterStart();
        }
        // A frame the eyes were not rendered from a head pose -- stereo off, a menu, a
        // failed acquire -- must not go out carrying the LAST pose that was. Otherwise
        // the presenter submits a frozen pose and the runtime warps the image by an
        // angle that grows the further you look away from it, which reads as the picture
        // drifting and distorting as you pan.
        if (!g_stereoRenderedFrame) vrClearRendered();

        presenterFrame(g_stereoRenderedFrame, vrFrustumInjected());
        g_stereoRenderedFrame = false;
    }

    return g_originalScreenDisplay ? g_originalScreenDisplay(surface) : 0;
}

// One timed invocation of the engine's sceneRender.
void __cdecl callOriginalSceneRender(void* params) {
    LARGE_INTEGER begin, end;
    QueryPerformanceCounter(&begin);
    g_originalSceneRender(params);
    QueryPerformanceCounter(&end);

    const long long ticks = end.QuadPart - begin.QuadPart;
    ++g_sceneOrigCalls;
    g_sceneTicks += ticks;
    if (ticks > g_sceneTicksWorst) g_sceneTicksWorst = ticks;
}

bool g_loggedOrientation = false;
bool g_loggedMirror = false;

// sceneRender overwrites the camera's fov with the game's own, 27 bytes before it calls
// cameraBind. This hook sits on those two calls, so it runs after that write and before
// cameraBind rebuilds the projection -- the only window where an eye frustum survives.
void __cdecl cameraBindHook(void* camera) {
    stereoOnCameraBind(camera);
    if (g_originalCameraBind) g_originalCameraBind(camera);
}

void __cdecl sceneRenderHook(void* params) {
    ++g_sceneHookCalls;

    auto scene = static_cast<SceneParams*>(params);

    if (!g_loggedOrientation) {
        g_loggedOrientation = true;
        const float* m = scene->orientation;
        logf("orientation matrix: [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
             m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        logf("eye world position:  (%.3f, %.3f, %.3f)", scene->eyeX, scene->eyeY,
             scene->eyeZ);
        logf("main camera %p, mirror camera %p", scene->camera, scene->mirrorCamera);
    }

    // The engine creates the mirror camera lazily, and leaves p[43] null when mirrors are
    // off. Say so the first time one appears, because otherwise stereoMirror looks broken
    // when in fact there is simply no mirror pass to place.
    if (scene->mirrorCamera && !g_loggedMirror) {
        g_loggedMirror = true;
        logf("mirror camera appeared: %p -- stereoMirror=%d now has something to place",
             scene->mirrorCamera, g_options.stereoMirror);
    }

    // If stereo is on and its targets are up it owns the whole frame: two renders, one
    // per half of the buffer. It returns false if it could not build the eye targets,
    // and then we render normally -- a failed stereo setup must never cost you the game.
    if (g_options.stereo && stereoRender(scene, &callOriginalSceneRender)) {
        g_stereoRenderedFrame = true;
        return;
    }

    callOriginalSceneRender(params);
}

// -- ChangeDisplaySettings -------------------------------------------------
//
// The pass-through paths below MUST NOT call the imported ChangeDisplaySettings* /
// ClipCursor names directly: those go through this DLL's own import table, which the
// IAT patcher would happily redirect back into these hooks -- infinite recursion.
// Resolve the real entry points from user32 once and call those.

HMODULE g_selfModule = nullptr;

// Belt and braces. If a pass-through path ever re-enters its own hook we would spin a
// core forever; this turns that into one log line and a harmless return.
thread_local int t_apiDepth = 0;

struct ApiGuard {
    bool reentered;
    ApiGuard() : reentered(t_apiDepth++ > 0) {}
    ~ApiGuard() { --t_apiDepth; }
};

template <typename Fn>
Fn realUser32(const char* name) {
    static HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return reinterpret_cast<Fn>(GetProcAddress(user32, name));
}

LONG WINAPI changeDisplaySettingsAHook(DEVMODEA* mode, DWORD flags) {
    ApiGuard guard;
    if (guard.reentered) {
        logf("re-entry into changeDisplaySettingsAHook -- passing through inert");
        return DISP_CHANGE_SUCCESSFUL;
    }

    if (g_options.blockDisplayChange) {
        logf("blocked ChangeDisplaySettingsA(%s, flags=%08X)",
             mode ? "mode" : "restore-defaults", flags);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return realUser32<decltype(&ChangeDisplaySettingsA)>("ChangeDisplaySettingsA")(
        mode, flags);
}

LONG WINAPI changeDisplaySettingsWHook(DEVMODEW* mode, DWORD flags) {
    ApiGuard guard;
    if (guard.reentered) {
        logf("re-entry into changeDisplaySettingsWHook -- passing through inert");
        return DISP_CHANGE_SUCCESSFUL;
    }

    if (g_options.blockDisplayChange) {
        logf("blocked ChangeDisplaySettingsW(flags=%08X)", flags);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return realUser32<decltype(&ChangeDisplaySettingsW)>("ChangeDisplaySettingsW")(
        mode, flags);
}

LONG WINAPI changeDisplaySettingsExAHook(LPCSTR device, DEVMODEA* mode, HWND window,
                                         DWORD flags, LPVOID param) {
    ApiGuard guard;
    if (guard.reentered) {
        logf("re-entry into changeDisplaySettingsExAHook -- passing through inert");
        return DISP_CHANGE_SUCCESSFUL;
    }

    if (g_options.blockDisplayChange) {
        if (mode)
            logf("blocked ChangeDisplaySettingsExA -> %ux%u @%u Hz, %u bpp, flags=%08X",
                 mode->dmPelsWidth, mode->dmPelsHeight, mode->dmDisplayFrequency,
                 mode->dmBitsPerPel, flags);
        else
            logf("blocked ChangeDisplaySettingsExA -> restore defaults, flags=%08X", flags);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return realUser32<decltype(&ChangeDisplaySettingsExA)>("ChangeDisplaySettingsExA")(
        device, mode, window, flags, param);
}

LONG WINAPI changeDisplaySettingsExWHook(LPCWSTR device, DEVMODEW* mode, HWND window,
                                         DWORD flags, LPVOID param) {
    ApiGuard guard;
    if (guard.reentered) {
        logf("re-entry into changeDisplaySettingsExWHook -- passing through inert");
        return DISP_CHANGE_SUCCESSFUL;
    }

    if (g_options.blockDisplayChange) {
        logf("blocked ChangeDisplaySettingsExW(flags=%08X)", flags);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return realUser32<decltype(&ChangeDisplaySettingsExW)>("ChangeDisplaySettingsExW")(
        device, mode, window, flags, param);
}

void applyPendingSceneRenderHook() {
    if (!g_sceneHookPending) return;
    g_sceneHookPending = false;

    for (uintptr_t site : g_pendingCallSites) {
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&sceneRenderHook) - (site + 5));
        if (!patchBytes(reinterpret_cast<void*>(site + 1), &rel, sizeof(rel))) {
            logf("sceneRender hook: FAILED to patch call site %08X",
                 static_cast<unsigned>(site));
            continue;
        }
        logf("sceneRender hook: call site %08X redirected to %08X",
             static_cast<unsigned>(site),
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&sceneRenderHook)));
    }
    g_pendingCallSites.clear();

    for (uintptr_t site : g_pendingBindCallSites) {
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&cameraBindHook) - (site + 5));
        if (!patchBytes(reinterpret_cast<void*>(site + 1), &rel, sizeof(rel))) {
            logf("cameraBind hook: FAILED to patch call site %08X",
                 static_cast<unsigned>(site));
            continue;
        }
        logf("cameraBind hook: call site %08X redirected to %08X",
             static_cast<unsigned>(site),
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&cameraBindHook)));
    }
    g_pendingBindCallSites.clear();
}

BOOL WINAPI clipCursorHook(const RECT* rect) {
    ApiGuard guard;
    if (guard.reentered) {
        logf("re-entry into clipCursorHook -- passing through inert");
        return TRUE;
    }

    if (g_options.blockClipCursor) {
        if (rect)
            logf("blocked ClipCursor(%d,%d..%d,%d)", rect->left, rect->top, rect->right,
                 rect->bottom);
        return TRUE;
    }
    return realUser32<decltype(&ClipCursor)>("ClipCursor")(rect);
}

// roglv2 believes it is exclusive fullscreen and pushes its window above everything,
// which hides the alt-tab switcher. Demote TOPMOST to NOTOPMOST on the way through.
BOOL WINAPI setWindowPosHook(HWND window, HWND insertAfter, int x, int y, int cx, int cy,
                             UINT flags) {
    ApiGuard guard;
    if (guard.reentered) return TRUE;

    if (!g_options.allowWindowTopmost && insertAfter == HWND_TOPMOST) {
        logf("demoted SetWindowPos(HWND_TOPMOST) to HWND_NOTOPMOST");
        insertAfter = HWND_NOTOPMOST;
    }
    return realUser32<decltype(&SetWindowPos)>("SetWindowPos")(window, insertAfter, x, y, cx,
                                                              cy, flags);
}

// Demoting SetWindowPos is not enough on its own: the sim window comes back topmost by
// some other route (it is created by gplc67, not roglv2, so a WS_EX_TOPMOST at creation
// is the likely culprit). Rather than chase every path, re-assert it from the frame
// hook -- once a second is plenty and costs nothing.
//
// The game exports its own window handle, which is exact; fall back to enumerating our
// thread's windows if that export is missing.
BOOL CALLBACK pickThreadWindow(HWND window, LPARAM out) {
    if (!IsWindowVisible(window)) return TRUE;
    *reinterpret_cast<HWND*>(out) = window;
    return FALSE;
}

HWND gameWindow() {
    using GetMainWindowHandleFn = HWND(__cdecl*)();
    static auto exported = reinterpret_cast<GetMainWindowHandleFn>(GetProcAddress(
        GetModuleHandleA(nullptr), "?GetMainWindowHandle@@YAPAUHWND__@@XZ"));
    if (exported) {
        if (HWND window = exported()) return window;
    }
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), &pickThreadWindow,
                      reinterpret_cast<LPARAM>(&found));
    return found;
}

void enforceNotTopmost() {
    // g_frames is reset every second by the reporter, so it cannot be used for pacing.
    static unsigned tick = 0;
    if ((tick++ % 36) != 0) return;  // roughly once a second

    HWND window = gameWindow();
    if (!window) return;

    const LONG style = GetWindowLongA(window, GWL_STYLE);
    const LONG exStyle = GetWindowLongA(window, GWL_EXSTYLE);
    RECT rect = {};
    GetWindowRect(window, &rect);

    // Report the state rather than only acting on it -- the last two attempts at this
    // were guesses, and a window that is merely desktop-sized and focused looks exactly
    // like a topmost one from the outside.
    static LONG lastStyle = 0, lastExStyle = 0;
    static RECT lastRect = {};
    if (style != lastStyle || exStyle != lastExStyle ||
        memcmp(&rect, &lastRect, sizeof(rect)) != 0) {
        lastStyle = style;
        lastExStyle = exStyle;
        lastRect = rect;

        char className[64] = {};
        GetClassNameA(window, className, sizeof(className));
        logf("window %p '%s' style=%08lX exStyle=%08lX rect=%ld,%ld..%ld,%ld "
             "topmost=%d foreground=%d desktop=%dx%d",
             window, className, style, exStyle, rect.left, rect.top, rect.right,
             rect.bottom, (exStyle & WS_EX_TOPMOST) != 0, window == GetForegroundWindow(),
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }

    if (g_options.allowWindowTopmost) return;

    if ((exStyle & WS_EX_TOPMOST) != 0) {
        logf("demoting WS_EX_TOPMOST on %p", window);
        SetWindowLongA(window, GWL_EXSTYLE, exStyle & ~WS_EX_TOPMOST);
        realUser32<decltype(&SetWindowPos)>("SetWindowPos")(
            window, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // A borderless window exactly covering the screen can be given fullscreen
    // exclusivity by Windows, and then overlays -- including the alt-tab switcher --
    // never composite over it. Being one pixel short defeats that.
    if (g_options.shrinkWindowByOnePixel) {
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (rect.right - rect.left >= screenW && rect.bottom - rect.top >= screenH) {
            logf("shrinking %p by one pixel to defeat fullscreen optimisation", window);
            realUser32<decltype(&SetWindowPos)>("SetWindowPos")(
                window, nullptr, rect.left, rect.top, screenW - 1, screenH - 1,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }
}

struct Replacement {
    const char* name;
    void*       replacement;
};

const Replacement kApiReplacements[] = {
    {"ChangeDisplaySettingsA", reinterpret_cast<void*>(&changeDisplaySettingsAHook)},
    {"ChangeDisplaySettingsW", reinterpret_cast<void*>(&changeDisplaySettingsWHook)},
    {"ChangeDisplaySettingsExA", reinterpret_cast<void*>(&changeDisplaySettingsExAHook)},
    {"ChangeDisplaySettingsExW", reinterpret_cast<void*>(&changeDisplaySettingsExWHook)},
    {"ClipCursor", reinterpret_cast<void*>(&clipCursorHook)},
    {"SetWindowPos", reinterpret_cast<void*>(&setWindowPosHook)},
};

}  // namespace

void loadOptions(const char* fileName) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) slash[1] = '\0';
    strncat_s(path, fileName, _TRUNCATE);

    // Floats have to come back as text; GetPrivateProfileInt cannot represent them.
    // Declared up here because the float options are no longer all in one block at
    // the end -- the debug rotation ones sit with the keys they belong to.
    char text[64] = {};

    g_options.stereo = GetPrivateProfileIntA("gplvr", "stereo", 0, path) != 0;
    g_options.stereoSwapEyes =
        GetPrivateProfileIntA("gplvr", "stereoSwapEyes", 0, path) != 0;
    g_options.stereoMirror = GetPrivateProfileIntA("gplvr", "stereoMirror", 1, path);
    g_options.stereoKey = GetPrivateProfileIntA("gplvr", "stereoKey", '1', path);
    g_options.blockDisplayChange =
        GetPrivateProfileIntA("gplvr", "blockDisplayChange", 0, path) != 0;
    g_options.blockClipCursor =
        GetPrivateProfileIntA("gplvr", "blockClipCursor", 0, path) != 0;
    g_options.installHooks = GetPrivateProfileIntA("gplvr", "installHooks", 1, path) != 0;
    g_options.glProbe = GetPrivateProfileIntA("gplvr", "glProbe", 0, path) != 0;
    g_options.vr = GetPrivateProfileIntA("gplvr", "vr", 0, path) != 0;
    g_options.vrRecentreKey = GetPrivateProfileIntA("gplvr", "vrRecentreKey", '3', path);
    g_options.vrDumpKey = GetPrivateProfileIntA("gplvr", "vrDumpKey", '4', path);
    g_options.vrFovProbeKey =
        GetPrivateProfileIntA("gplvr", "vrFovProbeKey", '5', path);
    GetPrivateProfileStringA("gplvr", "vrDebugYawDegrees", "0", text, sizeof(text), path);
    g_options.vrDebugYawDegrees = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrDebugPitchDegrees", "0", text, sizeof(text),
                             path);
    g_options.vrDebugPitchDegrees = static_cast<float>(atof(text));
    g_options.vrDebugRotate =
        GetPrivateProfileIntA("gplvr", "vrDebugRotate", 0, path) != 0;
    g_options.vrSubmitRenderedPose =
        GetPrivateProfileIntA("gplvr", "vrSubmitRenderedPose", 1, path) != 0;
    g_options.vrOverlayMarkers =
        GetPrivateProfileIntA("gplvr", "vrOverlayMarkers", 0, path) != 0;
    g_options.vrLevelSeat = GetPrivateProfileIntA("gplvr", "vrLevelSeat", 0, path) != 0;
    g_options.vrLevelRecentre =
        GetPrivateProfileIntA("gplvr", "vrLevelRecentre", 1, path) != 0;
    GetPrivateProfileStringA("gplvr", "vrWorldScale", "1.0", text, sizeof(text), path);
    g_options.vrWorldScale = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrDebugRotateRate", "45", text, sizeof(text), path);
    g_options.vrDebugRotateRate = static_cast<float>(atof(text));
    g_options.vrDebugYawLeftKey =
        GetPrivateProfileIntA("gplvr", "vrDebugYawLeftKey", 0x25, path);
    g_options.vrDebugYawRightKey =
        GetPrivateProfileIntA("gplvr", "vrDebugYawRightKey", 0x27, path);
    g_options.vrDebugPitchUpKey =
        GetPrivateProfileIntA("gplvr", "vrDebugPitchUpKey", 0x26, path);
    g_options.vrDebugPitchDownKey =
        GetPrivateProfileIntA("gplvr", "vrDebugPitchDownKey", 0x28, path);
    g_options.vrDebugRotateResetKey =
        GetPrivateProfileIntA("gplvr", "vrDebugRotateResetKey", 0x24, path);
    g_options.allowWindowTopmost =
        GetPrivateProfileIntA("gplvr", "allowWindowTopmost", 0, path) != 0;
    g_options.shrinkWindowByOnePixel =
        GetPrivateProfileIntA("gplvr", "shrinkWindowByOnePixel", 1, path) != 0;
    GetPrivateProfileStringA("gplvr", "stereoIpd", "0.064", text, sizeof(text), path);
    g_options.stereoIpd = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrMenuScale", "0.5", text, sizeof(text), path);
    g_options.vrMenuScale = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrFovScale", "1.0", text, sizeof(text), path);
    g_options.vrFovScale = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrFovScaleRight", "0", text, sizeof(text), path);
    g_options.vrFovScaleRight = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrEyeScale", "1.0", text, sizeof(text), path);
    g_options.vrEyeScale = static_cast<float>(atof(text));

    logf("options: stereo=%d ipd=%.4f m swapEyes=%d mirror=%d stereoKey=0x%02X",
         g_options.stereo, g_options.stereoIpd, g_options.stereoSwapEyes,
         g_options.stereoMirror, g_options.stereoKey);
    logf("options: vrMenuScale=%.2f vrFovScale=%.2f vrFovScaleRight=%.2f "
         "vrEyeScale=%.2f vrRecentreKey=0x%02X vrFovProbeKey=0x%02X",
         g_options.vrMenuScale, g_options.vrFovScale, g_options.vrFovScaleRight,
         g_options.vrEyeScale, g_options.vrRecentreKey, g_options.vrFovProbeKey);
    logf("options: installHooks=%d vr=%d glProbe=%d blockDisplayChange=%d blockClipCursor=%d "
         "allowWindowTopmost=%d shrinkWindowByOnePixel=%d (from %s)",
         g_options.installHooks, g_options.vr, g_options.glProbe, g_options.blockDisplayChange,
         g_options.blockClipCursor, g_options.allowWindowTopmost,
         g_options.shrinkWindowByOnePixel, path);
}

bool installFrameCounter(Binding& binding) {
    void** slot = binding.raster.screenDisplay;
    if (!slot || !*slot) return false;

    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_windowStart);

    g_originalScreenDisplay = reinterpret_cast<ScreenDisplayFn>(*slot);

    void* hook = reinterpret_cast<void*>(&screenDisplayHook);
    if (!patchBytes(slot, &hook, sizeof(hook))) {
        logf("installFrameCounter: could not write the screenDisplay global");
        return false;
    }
    logf("installFrameCounter: screenDisplay %p -> %p", g_originalScreenDisplay, hook);
    return true;
}

void requestSceneRenderHook(const Binding& binding) {
    stereoBind(binding);

    if (binding.sceneRenderCallSites.empty()) {
        logf("requestSceneRenderHook: nothing to patch");
        return;
    }
    g_pendingCallSites = binding.sceneRenderCallSites;
    g_originalSceneRender = reinterpret_cast<SceneRenderFn>(binding.sceneRender);

    // Without these the eyes render the game's field of view rather than the headset's,
    // so say plainly which of the two is about to happen.
    g_pendingBindCallSites = binding.cameraBindCallSitesInScene;
    g_originalCameraBind = reinterpret_cast<CameraBindFn>(binding.cameraBind);
    if (g_pendingBindCallSites.empty())
        logf("requestSceneRenderHook: no cameraBind call sites -- the eye frustum fix is "
             "OFF and the eyes will use the game's own fov");

    g_sceneHookPending = true;
    logf("requestSceneRenderHook: queued %zu sceneRender and %zu cameraBind call site(s); "
         "will apply on the next frame",
         g_pendingCallSites.size(), g_pendingBindCallSites.size());
}

void patchDisplaySettingsImports(HMODULE module) {
    // Patching our own imports would make every pass-through call re-enter its hook.
    if (module == g_selfModule) return;

    ModuleImage image;
    if (!getModuleImage(module, image)) return;

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return;

    auto descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            image.base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                         : descriptor->FirstThunk));
        auto slots =
            reinterpret_cast<IMAGE_THUNK_DATA32*>(
                const_cast<uint8_t*>(image.base) + descriptor->FirstThunk);

        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                image.base + names->u1.AddressOfData);
            for (const Replacement& entry : kApiReplacements) {
                if (std::strcmp(reinterpret_cast<const char*>(import->Name), entry.name) != 0)
                    continue;
                auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry.replacement));
                if (slots->u1.Function == value) break;  // already ours
                if (patchBytes(&slots->u1.Function, &value, sizeof(value))) {
                    char name[MAX_PATH] = {};
                    GetModuleFileNameA(module, name, MAX_PATH);
                    const char* leaf = strrchr(name, '\\');
                    logf("IAT: %s imports %s -> hooked", leaf ? leaf + 1 : name, entry.name);
                }
                break;
            }
        }
    }
}

void setSelfModule(HMODULE module) { g_selfModule = module; }

void patchDisplaySettingsImportsEverywhere() {
    HMODULE modules[512];
    DWORD   needed = 0;
    HANDLE  process = GetCurrentProcess();

    // EnumProcessModules lives in psapi on older SDKs; K32EnumProcessModules is in
    // kernel32 on everything this game will ever run on.
    using EnumFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    auto enumerate = reinterpret_cast<EnumFn>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules"));
    if (!enumerate) return;

    if (!enumerate(process, modules, sizeof(modules), &needed)) return;
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count && i < 512; ++i) patchDisplaySettingsImports(modules[i]);
}

}  // namespace gplvr
