#include "Hooks.h"

#include "MouseLook.h"
#include "Log.h"
#include "RenderTarget.h"
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
bool      g_vrMouseLookKeyLast = false;
bool      g_loggedMouseLook = false;

// Set by the scene hook when stereoRender actually split the frame into two eyes.
// Read and cleared by the frame hook.
//
// This is NOT the same as "the stereo option is on". Menus are drawn straight to the
// back buffer without a world render, so nothing splits them -- but telling the
// presenter side-by-side anyway made it cut the menu in half and blow one half up to
// fill the headset. What matters is whether THIS frame has two eyes in it.
bool      g_stereoRenderedFrame = false;
bool      g_warnedLateTick = false;

// The parameter block the last scene render used. Stable across frames -- it is
// simFrame+81 -- but taken from the hook rather than assumed, so an extra render can
// never run against a block the game has not handed us.
void*     g_lastSceneParams = nullptr;
LONGLONG  g_lastPresentTick = 0;
LONGLONG  g_frameInterval = 0;
bool      g_loggedExtraRenders = false;
bool      g_lastFrameWasStereo = false;
bool      g_warnedSlowTick = false;
LONGLONG  g_extraRenderCost = 0;

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

    double transferMs = 0.0;
    int    transfers = 0;
    const bool haveTransfer = presenterTakeTransferCost(transferMs, transfers);

    logf("fps %.1f | worldFrames %.1f/s, renders %.1f/s (x%.2f), %.2f ms each, "
         "%.2f ms worst | transfer %.2f ms x%.1f/frame | mode %s",
         g_frames / seconds, g_sceneHookCalls / seconds, g_sceneOrigCalls / seconds,
         g_sceneHookCalls ? static_cast<double>(g_sceneOrigCalls) / g_sceneHookCalls : 0.0,
         msPerRender, worstMs,
         haveTransfer && transfers ? transferMs / transfers : 0.0,
         haveTransfer && g_frames ? static_cast<double>(transfers) / g_frames : 0.0,
         g_options.stereo ? "stereo" : "single");

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

    // The mouse look toggle, and the mouse itself.
    //
    // Deliberately NOT gated on g_options.vr: the whole reason this exists is to drive
    // the camera with vr = 0, on a flat monitor, where a straight line is straight and
    // the headset cannot be blamed for anything.
    if (g_options.vrMouseLookKey) {
        const bool down = (GetAsyncKeyState(g_options.vrMouseLookKey) & 0x8000) != 0;
        if (down && !g_vrMouseLookKeyLast) {
            g_options.vrMouseLook = !g_options.vrMouseLook;
            logf("mouselook toggled %s%s", g_options.vrMouseLook ? "ON" : "off",
                 (g_options.vrMouseLook && g_options.vr)
                     ? " -- head tracking is now inhibited"
                     : "");
        }
        g_vrMouseLookKeyLast = down;
    }

    if (g_options.vrMouseLook && mouseLookEnsureStarted()) {
        int deltaX = 0;
        int deltaY = 0;
        if (mouseLookTake(deltaX, deltaY)) {
            // Right turns the view right, and yaw is positive to the LEFT, so that sign
            // flips. Mouse forward is negative Y and should look UP, and pitch is
            // positive up, so that one flips as well.
            const float pitchSign = g_options.vrMouseLookInvertPitch ? 1.0f : -1.0f;
            vrLookOverrideBy(
                -static_cast<float>(deltaX) * g_options.vrMouseLookSensitivity,
                pitchSign * static_cast<float>(deltaY) *
                    g_options.vrMouseLookSensitivity);

            if (!g_loggedMouseLook) {
                g_loggedMouseLook = true;
                float atYaw = 0.0f, atPitch = 0.0f;
                vrLookOverrideAngles(atYaw, atPitch);
                logf("mouselook: first movement -- the camera is being driven from the "
                     "mouse (now yaw %.1f deg, pitch %.1f deg). %s",
                     atYaw, atPitch,
                     g_options.vr
                         ? "Head tracking is inhibited."
                         : "vr is off, so this is GPL's own renderer on a flat monitor "
                           "with nothing else in the chain.");
            }
        }
    }

}

void applyPendingSceneRenderHook();  // defined below
void enforceNotTopmost();            // defined below
void __cdecl callOriginalSceneRender(void* params);  // defined below

// Re-render the whole scene from a fresher head pose, spread across the idle tail of
// the 36 Hz tick.
//
// Calling sceneRender more than once per frame is not a new risk: stereo has been
// calling it twice, once per eye, for the whole project. This calls it twice more.
//
// The spacing is the point. Publishing three frames back to back would give the
// presenter three images at the same instant and nothing for the next 28 ms, which is
// no smoother than one. Each extra render therefore waits until its slice of the
// measured frame interval before drawing, so the poses it picks up from the presenter
// are genuinely newer.
void renderExtraFrames() {
    if (!g_options.vr || g_options.vrExtraRenders <= 0 || !g_options.stereo) return;
    if (!g_lastSceneParams || !stereoAvailable()) return;
    if (!g_lastFrameWasStereo || !vrFrustumInjected()) return;

    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);

    // Measure the tick from present to present rather than assuming 36 Hz -- the rate
    // changes with the replay/menu/sim state and a wrong guess would either overrun the
    // next frame or bunch the extra renders at the start of it.
    const LONGLONG previous = g_lastPresentTick;
    g_lastPresentTick = now.QuadPart;
    if (!previous) return;

    const LONGLONG measured = now.QuadPart - previous;
    if (measured <= 0) return;
    // Smooth it, and ignore an outlier from a load hitch.
    g_frameInterval = g_frameInterval ? (g_frameInterval * 3 + measured) / 4 : measured;
    const LONGLONG interval = g_frameInterval;
    if (interval <= 0 || measured > interval * 3) return;

    // Never stretch a frame that is already slow.
    //
    // The spacing waits a FRACTION of the measured interval, so during a load stall --
    // observed at 1.1 fps for seven seconds -- it waited a quarter of a second twice
    // per frame and made the stall several times worse. The smoothed interval adapts to
    // a sustained slow patch, so the spike guard above never fires for one. Anything
    // slower than 25 fps is not a tick with idle time to spend, so spend none.
    const LONGLONG slowest = frequency.QuadPart / 25;
    if (interval > slowest) {
        if (!g_warnedSlowTick) {
            g_warnedSlowTick = true;
            logf("vr: extra renders paused -- the game frame is %.1f ms, which leaves no "
                 "idle time to render into. They resume on their own once it recovers.",
                 1000.0 * static_cast<double>(interval) /
                     static_cast<double>(frequency.QuadPart));
        }
        return;
    }
    g_warnedSlowTick = false;

    // PACE AGAINST THE SIM'S OWN TICK, NOT THE RATE THE GAME HAPPENS TO BE RUNNING AT.
    //
    // Everything above measures the achieved frame interval, which is right for spacing
    // but wrong for deciding how much time there is to spend -- because the extra renders
    // are part of what determines that interval. At 30 Hz the measured interval is 33 ms,
    // comfortably inside the 25 fps guard, so the deadline is generous, every extra render
    // fits, and the game stays at 30. The loop is self-consistent at ANY rate, including
    // one it caused itself. Measured: three extra renders held a heavy map at exactly
    // 30.0 for minutes; dropping to two returned it to 36.0 with the same scene.
    //
    // The 36 is the sim TASK RATE, the same constant the mod is forbidden to change --
    // and a frame slower than that is not merely less smooth. The user reports tyre grip
    // behaving differently on maps that ran under 36, which is what you would expect if
    // the physics is stepped by this rate. NOT yet verified in simFrameRender, and worth
    // verifying before anything depends on it, but it turns "hold 36" from a comfort
    // preference into a correctness one.
    const LONGLONG budget = frequency.QuadPart / 36;

    // Already late: give the time back rather than spending it. Reported on the
    // transition only -- once per frame would be a flood at exactly the moment the log
    // is least readable.
    if (measured > budget + budget / 10) {
        // On CHANGE, and CAPPED. A latch alone is not enough when the condition itself
        // alternates: holding back on a late frame makes the next one early, so the
        // transition fires again, and the first version of this line appeared 2650 times
        // in one session. Reporting on change is necessary and not sufficient.
        static int lateReportsLeft = 12;
        if (!g_warnedLateTick && lateReportsLeft > 0) {
            --lateReportsLeft;
            g_warnedLateTick = true;
            logf("vr: extra renders held back -- the game frame is %.1f ms against a "
                 "27.8 ms tick, so the time belongs to the sim. They resume by "
                 "themselves.%s",
                 1000.0 * static_cast<double>(measured) /
                     static_cast<double>(frequency.QuadPart),
                 lateReportsLeft ? "" : " (further holdbacks will not be reported)");
        }
        return;
    }
    g_warnedLateTick = false;

    // The earlier of the two: never plan past the next 36 Hz tick, and never past the
    // next frame if the game is legitimately slower (a replay, a menu).
    const LONGLONG pace = interval < budget ? interval : budget;

    int extra = g_options.vrExtraRenders;
    if (extra > 3) extra = 3;

    if (!g_loggedExtraRenders) {
        g_loggedExtraRenders = true;
        // The wording said "spread over a tick" long after the spreading was replaced
        // by locking to the presenter's cadence. A stale log line is a lie told once a
        // session.
        logf("vr: extra renders ON -- up to %d per game frame, each waiting for the "
             "presenter to publish a new pose rather than for a slice of the %.1f ms "
             "tick. The world still animates at the sim rate; only head tracking gets "
             "faster. Watch that 'fps' below does not fall.",
             extra, 1000.0 * static_cast<double>(interval) /
                        static_cast<double>(frequency.QuadPart));
    }

    // Lock each extra render to the presenter's own cadence instead of slicing the
    // tick into equal parts.
    //
    // Slicing gave 3 renders per 27.8 ms tick -- 108 Hz into a 90 Hz display, which
    // does not divide. About every fifth frame is discarded and what is left beats at
    // 18 Hz, which is visible as precisely the stepping these renders exist to remove.
    // Waiting for the view counter to move means one rendered frame per display frame,
    // whatever the two rates happen to be.
    //
    // The deadline is the point where the game's own next frame is due. Stop before it
    // rather than counting renders: if the presenter is slow we do fewer, if it is fast
    // we do more, and neither case runs into the game.
    const LONGLONG deadline = now.QuadPart + pace - pace / 8;

    for (int done = 0; done < extra; ++done) {
        const int32_t before = presenterViewSequence();
        if (!before) return;

        // Wait for the presenter to publish a NEW view set. Without this the extra
        // render would draw from the same pose as the frame before it and cost time for
        // an identical image.
        for (;;) {
            LARGE_INTEGER current;
            QueryPerformanceCounter(&current);
            if (current.QuadPart >= deadline) return;
            if (presenterViewSequence() != before) break;
            SwitchToThread();
        }

        // Do not START a render that cannot finish before the game's next frame.
        //
        // The deadline was only checked while waiting, so a render begun just under it
        // ran straight into the game's own frame -- which is a hitch, and hitches are
        // what these renders exist to remove. Budget on what the last one actually
        // cost, doubled for headroom.
        LARGE_INTEGER before2;
        QueryPerformanceCounter(&before2);
        if (before2.QuadPart + g_extraRenderCost * 2 > deadline) return;

        if (stereoRender(static_cast<SceneParams*>(g_lastSceneParams),
                         &callOriginalSceneRender)) {
            presenterFrame(true, vrFrustumInjected());
        }

        LARGE_INTEGER after;
        QueryPerformanceCounter(&after);
        const LONGLONG cost = after.QuadPart - before2.QuadPart;
        g_extraRenderCost = g_extraRenderCost ? (g_extraRenderCost * 3 + cost) / 4 : cost;
    }
}

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

    // Same reason, same moment: the shared texture is filled from the back buffer
    // before the game presents it. presenterStart needs a current GL context, which is
    // why it starts here rather than at discovery time.
    // The off-screen frame has to stop being the draw target before the presenter
    // reads it and before the game presents: glCopyTexSubImage2D reads the READ
    // framebuffer, and SwapBuffers only ever shows the window's own.
    presenterEndFrame();

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
        // Remembered before it is cleared: an extra render must only ever repeat a
        // frame the game actually rendered a tracked stereo world into. On a menu the
        // saved parameter block is stale, and re-running it would paint the sim over
        // the menu.
        g_lastFrameWasStereo = g_stereoRenderedFrame;
        g_stereoRenderedFrame = false;
    }

    // The window is a monitor of the off-screen frame, not the frame itself. Between
    // the presenter's copy and the present, so what the player sees on the desktop is
    // the same image that went to the headset.
    presenterBlitToWindow(g_options.vrMonitorEye);

    const int presented = g_originalScreenDisplay ? g_originalScreenDisplay(surface) : 0;

    // Bind the off-screen target for the NEXT frame here, before the extra renders,
    // because an extra render is a whole frame too and must land in the same place the
    // game's own does. It also has to be before the game draws anything at all: GPL
    // lays out the HUD and the 2D overlays into the same surface after sceneRender
    // returns, so catching only the world would put the world in one buffer and the
    // instruments in another.
    presenterBeginFrame();

    // Extra renders go AFTER the present, in the idle tail of the tick.
    //
    // Before it, they would delay the game's own frame by however long they take and
    // cost exactly the frame rate they are meant to improve. After it, they spend time
    // the game thread was going to spend waiting for the next 36 Hz task anyway.
    renderExtraFrames();
    return presented;
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
    g_lastSceneParams = params;

    // The look override, on the flat path.
    //
    // With vr = 1 it is applied inside vrAcquireEyes, which writes it over the head
    // rotation; doing it here as well would apply it twice. With vr = 0 there is no
    // presenter and vrAcquireEyes never runs, so here is the only place it can go -- and
    // that is the case this exists for.
    //
    // params is the game's own block and it is reused every frame, so the original has
    // to go back before we return, exactly as stereoRender does with what it borrows.
    float savedOrientation[9];
    std::memcpy(savedOrientation, scene->orientation, sizeof(savedOrientation));
    const bool overridden = !g_options.vr && vrApplyLookOverride(scene->orientation);

    if (g_options.stereo && stereoRender(scene, &callOriginalSceneRender))
        g_stereoRenderedFrame = true;
    else
        callOriginalSceneRender(params);

    if (overridden)
        std::memcpy(scene->orientation, savedOrientation, sizeof(savedOrientation));
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

// roglv2's WindowManager::SetSize is the one that asks:
//     SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, cx, cy, 0)
// on every mode change. Demote TOPMOST to NOTOPMOST on the way through. Note the flags
// are 0, so that call also moves the window to (0,0) and resizes it -- and it then polls
// GetWindowRect up to 500 times at 10 ms until the rect is exactly (0,0,cx,cy). Do not
// change the window rect while a resize is in flight or the game stalls five seconds.
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

// Re-assert from the frame hook -- once a second is plenty and costs nothing.
//
// The guess this once carried, that gplc67 created the window WS_EX_TOPMOST, is WRONG:
// WinMain calls CreateWindowExA with dwExStyle 0 and style WS_POPUP, and the IAT patch
// covers roglv2 as well as the exe. The demote branch below should therefore never fire;
// if it does, something outside these two modules is setting it, and that is worth
// knowing. The branch that DOES earn its place is the one-pixel shrink: WS_POPUP exactly
// covering the desktop is what Windows hands fullscreen exclusivity to.
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
    g_options.stereoMirror = GetPrivateProfileIntA("gplvr", "stereoMirror", 1, path);
    g_options.stereoKey = GetPrivateProfileIntA("gplvr", "stereoKey", '1', path);
    g_options.blockDisplayChange =
        GetPrivateProfileIntA("gplvr", "blockDisplayChange", 0, path) != 0;
    g_options.blockClipCursor =
        GetPrivateProfileIntA("gplvr", "blockClipCursor", 0, path) != 0;
    g_options.vr = GetPrivateProfileIntA("gplvr", "vr", 0, path) != 0;
    g_options.vrRecentreKey = GetPrivateProfileIntA("gplvr", "vrRecentreKey", '3', path);
    g_options.vrMouseLook =
        GetPrivateProfileIntA("gplvr", "vrMouseLook", 0, path) != 0;
    GetPrivateProfileStringA("gplvr", "vrMouseLookSensitivity", "0.08", text,
                             sizeof(text), path);
    g_options.vrMouseLookSensitivity = static_cast<float>(atof(text));
    g_options.vrMouseLookInvertPitch =
        GetPrivateProfileIntA("gplvr", "vrMouseLookInvertPitch", 0, path) != 0;
    g_options.vrMouseLookKey =
        GetPrivateProfileIntA("gplvr", "vrMouseLookKey", '6', path);
    GetPrivateProfileStringA("gplvr", "vrHorizonLock", "0.0", text, sizeof(text), path);
    g_options.vrHorizonLock = static_cast<float>(atof(text));
    g_options.vrExtraRenders = GetPrivateProfileIntA("gplvr", "vrExtraRenders", 0, path);
    GetPrivateProfileStringA("gplvr", "vrWorldScale", "1.0", text, sizeof(text), path);
    g_options.vrWorldScale = static_cast<float>(atof(text));
    g_options.allowWindowTopmost =
        GetPrivateProfileIntA("gplvr", "allowWindowTopmost", 0, path) != 0;
    g_options.shrinkWindowByOnePixel =
        GetPrivateProfileIntA("gplvr", "shrinkWindowByOnePixel", 1, path) != 0;
    GetPrivateProfileStringA("gplvr", "vrMenuScale", "0.5", text, sizeof(text), path);
    g_options.vrMenuScale = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrRenderScale", "1.0", text, sizeof(text), path);
    g_options.vrRenderScale = static_cast<float>(atof(text));

    logf("options: stereo=%d mirror=%d stereoKey=0x%02X",
         g_options.stereo, g_options.stereoMirror, g_options.stereoKey);
    g_options.vrOffscreen = GetPrivateProfileIntA("gplvr", "vrOffscreen", 0, path) != 0;
    g_options.vrMonitorEye = GetPrivateProfileIntA("gplvr", "vrMonitorEye", 0, path);
    if (g_options.vrMonitorEye < 0 || g_options.vrMonitorEye > 2)
        g_options.vrMonitorEye = 0;

    logf("options: vrMenuScale=%.2f vrRenderScale=%.2f vrRecentreKey=0x%02X "
         "vrOffscreen=%d vrMonitorEye=%d",
         g_options.vrMenuScale, g_options.vrRenderScale, g_options.vrRecentreKey,
         g_options.vrOffscreen, g_options.vrMonitorEye);
    logf("options: vrExtraRenders=%d vrHorizonLock=%.2f vrWorldScale=%.2f",
         g_options.vrExtraRenders, g_options.vrHorizonLock, g_options.vrWorldScale);
    logf("options: vrMouseLook=%d sensitivity=%.3f invertPitch=%d key=0x%02X",
         g_options.vrMouseLook ? 1 : 0, g_options.vrMouseLookSensitivity,
         g_options.vrMouseLookInvertPitch ? 1 : 0, g_options.vrMouseLookKey);
    logf("options: vr=%d blockDisplayChange=%d blockClipCursor=%d "
         "allowWindowTopmost=%d shrinkWindowByOnePixel=%d (from %s)",
         g_options.vr, g_options.blockDisplayChange,
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

    // Hand over the ORIGINAL, before anyone can read the global and find our hook there.
    renderTargetSetScreenDisplay(reinterpret_cast<const void*>(g_originalScreenDisplay));
    return true;
}

void requestSceneRenderHook(const Binding& binding) {
    stereoBind(binding);
    renderTargetBind(binding);

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
