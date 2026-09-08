#include "Hooks.h"

#include "MouseLook.h"
#include "Log.h"
#include "RenderShim.h"
#include "RenderTarget.h"
#include "Stereo.h"
#include "SwapControl.h"
#include "TaskRate.h"
#include "VrCamera.h"
#include "presenter/Producer.h"

#include <cstdio>   // _snprintf_s, for the mirror-refresh fragment
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

// FRAME PROFILE -- where the time goes that sceneRender does not account for.
//
// It exists because the two numbers stopped agreeing. Once vsync was switched off the
// frame period no longer matched the render: with the field in view, 8.05 ms a frame
// against 3.68 ms per render, 10.07 against 4.12. Four to six milliseconds a frame were
// being spent somewhere the instrument could not see, and they scaled with the car count
// exactly as the render did -- so the obvious suspect (the world pass) was already
// measured and was not it.
//
// The decomposition needs no new hooks, because the frame is already bracketed by the
// two we have. Between one present and the next lies exactly one frame, and inside it:
//
//   scene    time inside the engine's sceneRender, both passes -- already measured
//   present  time inside the engine's screenDisplay, i.e. SwapBuffers and the driver
//   gplvr    our own work in the present hook, everything except the original call
//   other    the remainder = period - scene - present - gplvr
//
// `other` is the answer being hunted: it is the game's own per-frame work outside the
// world pass -- gpl_simFrameSetup's per-car snapshot, and the HUD and 2D overlays GPL
// draws after sceneRender returns. It is a subtraction rather than a measurement, so it
// also absorbs anything running between the hooks, scheduler idle included; that is why
// the line reports the period too, and why a `other` that grows with the car count means
// something while a large one on an idle menu does not.
long long g_profPresentTicks = 0;   // inside g_originalScreenDisplay
long long g_profGplvrTicks = 0;     // our own work in the present hook
long long g_profPeriodTicks = 0;    // present entry to present entry
long long g_profPeriodWorst = 0;
LONGLONG  g_profLastEntry = 0;      // previous present-hook entry, for the period
LONGLONG  g_profFrameEntry = 0;     // this present-hook entry
long long g_profFinishTicks = 0;    // glFinish at the end of the scene, when asked for
unsigned  g_profPolys = 0;          // polyDraw calls, i.e. polygons handed to roglv2

// polyDraw's counter, as a naked thunk.
//
// The entry point is one of the 79 the game binds into writable globals, and its
// signature is not established -- so the thunk touches nothing but a memory increment and
// falls through to the original. `inc [mem]` writes only flags, and a call site cannot
// expect flags to survive a call, so this is safe whatever the convention turns out to
// be, with no need to know the argument count or who cleans the stack.
void* g_originalPolyDraw = nullptr;

__declspec(naked) void polyDrawThunk() {
    __asm {
        inc dword ptr [g_profPolys]
        jmp dword ptr [g_originalPolyDraw]
    }
}

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
bool      g_loggedMirrorPath = false;
// How many scene renders this second actually REFRESHED the mirror texture.
//
// This is the measurement that settles why the mirror rate varies. gpl_simFrameRender
// allocates the mirror's bitmap instance, renders, copies the corner into mirror.mip,
// then FREES the instance and zeroes p[43] before returning. Our extra renders run after
// that, through a pointer into the same live params block -- so p[43] is already null and
// they skip the mirror entirely. Predicted consequence: mirror refreshes track the GAME
// frame rate while the headset sees 1 + vrExtraRenders frames, so the ratio is set by how
// much idle time the current scene leaves. If `mirror N/s` tracks worldFrames and not fps,
// that is confirmed; if it is far below worldFrames, something else is throttling it and
// the extra renders are not the story.
unsigned  g_mirrorRefreshes = 0;
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

    // On the SAME line as the frame rate, deliberately. A resync means the sim threw
    // away time, and the only question worth asking about it is what the frame rate was
    // doing at that moment -- which is work the reader should not have to do by
    // correlating timestamps across two lines.
    char tick[96] = {};
    const bool haveTick = taskRateFormat(tick, sizeof(tick), seconds);

    // Only when there is a mirror at all -- in the menus and the non-cockpit camera
    // modes gpl_camModeHasMirror says no, and a permanent "mirror 0.0/s" would be noise
    // rather than information.
    char mirror[40] = {};
    if (g_mirrorRefreshes)
        _snprintf_s(mirror, sizeof(mirror), _TRUNCATE, "mirror %.1f/s | ",
                    g_mirrorRefreshes / seconds);

    logf("fps %.1f | worldFrames %.1f/s, renders %.1f/s (x%.2f), %.2f ms each, "
         "%.2f ms worst | transfer %.2f ms x%.1f/frame | %s%s%smode %s",
         g_frames / seconds, g_sceneHookCalls / seconds, g_sceneOrigCalls / seconds,
         g_sceneHookCalls ? static_cast<double>(g_sceneOrigCalls) / g_sceneHookCalls : 0.0,
         msPerRender, worstMs,
         haveTransfer && transfers ? transferMs / transfers : 0.0,
         haveTransfer && g_frames ? static_cast<double>(transfers) / g_frames : 0.0,
         mirror, haveTick ? tick : "", haveTick ? " | " : "",
         g_options.stereo ? "stereo" : "single");

    // The frame budget, broken up. Per FRAME, not per render, so the four parts and the
    // remainder add up to the period on the same line and can be read across.
    if (g_options.profileFrame && g_frames) {
        const double toMs = 1000.0 / g_frequency.QuadPart;
        const double period = g_profPeriodTicks * toMs / g_frames;
        const double scene = g_sceneTicks * toMs / g_frames;
        const double present = g_profPresentTicks * toMs / g_frames;
        const double gplvr = g_profGplvrTicks * toMs / g_frames;
        // The finish is inside the scene span, so it is shown as a share of it rather
        // than as another term of the sum -- otherwise the parts would not add up.
        char extra[80] = {};
        int  used = 0;
        if (g_options.profileGpuWait)
            used = _snprintf_s(extra, sizeof(extra), _TRUNCATE, "  [gpuWait %.2f of scene]",
                               g_profFinishTicks * toMs / g_frames);
        if (g_options.profilePolys && used >= 0)
            _snprintf_s(extra + (used > 0 ? used : 0), sizeof(extra) - (used > 0 ? used : 0),
                        _TRUNCATE, "  [%u polys/frame, %.0f ns each]",
                        g_profPolys / g_frames,
                        g_profPolys ? 1e6 * scene * g_frames / g_profPolys : 0.0);

        logf("frame %.2f ms (worst %.2f) = scene %.2f + present %.2f + gplvr %.2f + "
             "other %.2f%s",
             period, g_profPeriodWorst * toMs, scene, present, gplvr,
             period - scene - present - gplvr, extra);
    }

    char shim[320] = {};
    if (shimFormat(shim, sizeof(shim), seconds)) logf("%s", shim);

    g_windowStart = now;
    g_mirrorRefreshes = 0;
    g_frames = 0;
    g_sceneHookCalls = 0;
    g_sceneOrigCalls = 0;
    g_sceneTicks = 0;
    g_sceneTicksWorst = 0;
    g_profPresentTicks = 0;
    g_profGplvrTicks = 0;
    g_profPeriodTicks = 0;
    g_profPeriodWorst = 0;
    g_profFinishTicks = 0;
    g_profPolys = 0;
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
    // The rate is the sim TASK RATE, the same constant the mod is forbidden to change,
    // and a frame slower than it is not merely less smooth. VERIFIED 2026-08-31: the
    // physics step does NOT stretch -- a timer thread free-runs against the CPU clock and
    // releases a semaphore, so a late frame yields more pending ticks, never a longer one.
    // But once the timer is more than EIGHT periods behind it re-bases on now and every
    // tick in between is abandoned. The sim skips time rather than running slow, which is
    // exactly what would upset tyre load and slip integration. So holding the rate is a
    // correctness requirement, and TaskRate.cpp counts the failures so it can be checked
    // rather than assumed.
    //
    // Read the rate rather than assuming 36: the 60 fps patches rewrite it, and on a
    // patched exe the sim genuinely steps at 60. Falling back to 36 only when the
    // scheduler could not be decoded at all.
    const int      simHz = taskRateSimHz();
    const LONGLONG budget = frequency.QuadPart / (simHz ? simHz : 36);

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
                 "%.1f ms tick, so the time belongs to the sim. They resume by "
                 "themselves.%s",
                 1000.0 * static_cast<double>(measured) /
                     static_cast<double>(frequency.QuadPart),
                 1000.0 * static_cast<double>(budget) /
                     static_cast<double>(frequency.QuadPart),
                 lateReportsLeft ? "" : " (further holdbacks will not be reported)");
        }
        return;
    }
    g_warnedLateTick = false;

    // The earlier of the two: never plan past the next sim tick, and never past the
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

        // NO MIRROR IN THE EXTRA RENDERS, on either path, deliberately.
        //
        // The two rendering paths disagree about what p[43] is worth by now. On the
        // COPY-OUT path gpl_simFrameRender has already freed the bitmap instance and
        // zeroed p[43], so this is already null and the mirror is skipped. On the DIRECT
        // path p[43] is a persistent render-target camera that survives the frame -- so
        // without this line every extra render would repeat the ENTIRE mirror pass, up to
        // three more full world renders from the mirror camera per game frame.
        //
        // That work buys nothing. Extra renders exist to move the HEAD; the world in them
        // is the same sim state, so a mirror redrawn inside one shows the same picture
        // from the same mirror camera. A mirror updating once per game frame is not stale
        // relative to anything.
        //
        // Forcing it null here makes the two paths behave and cost identically, which is
        // the whole point: the mirror should not depend on whether this machine's driver
        // still exposes pbuffer render targets.
        auto* const extraParams = static_cast<SceneParams*>(g_lastSceneParams);
        void* const extraMirror = extraParams->mirrorCamera;
        extraParams->mirrorCamera = nullptr;
        const bool rendered = stereoRender(extraParams, &callOriginalSceneRender);
        // Put it back: on the direct path the game does NOT rewrite p[43] from scratch on
        // entry to gpl_simFrameRender -- it branches on finding it already set.
        extraParams->mirrorCamera = extraMirror;
        if (rendered) {
            // An extra render redraws the world over the frame the cursor was already
            // put into, so it has to go back in before this one is published. Without
            // this the pointer would appear only on the game's own frames and blink at
            // whatever rate the extra renders happen to be running at.
            presenterDrawCursor(true);
            presenterFrame(true, vrFrustumInjected());
            // Same pairing as the main frame, and for the same reason: a save must be
            // put back in the block that made it, immediately after the transfer that
            // needed it. Leaving it for the next frame's restore would mean two saves
            // outstanding at once and one of them lost.
            presenterRestoreUnderCursor();
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

    LARGE_INTEGER frameEntry;
    QueryPerformanceCounter(&frameEntry);
    if (g_profLastEntry) {
        const long long period = frameEntry.QuadPart - g_profLastEntry;
        g_profPeriodTicks += period;
        if (period > g_profPeriodWorst) g_profPeriodWorst = period;
    }
    g_profLastEntry = frameEntry.QuadPart;
    g_profFrameEntry = frameEntry.QuadPart;

    ++g_frames;
    shimEndFrame();
    taskRateReportThread("present");
    pollToggle();
    enforceNotTopmost();
    // Here rather than at discovery time for the same reason presenterStart is: it
    // needs a current GL context. It re-applies itself whenever roglv2 builds a new
    // one, which is every menu-to-sim transition.
    swapControlApply();
    reportOncePerSecond();

    // Before the original, not after: the back buffer still holds the finished frame
    // at this point, which is the whole reason the transport probe lives here.

    // Same reason, same moment: the shared texture is filled from the back buffer
    // before the game presents it. presenterStart needs a current GL context, which is
    // why it starts here rather than at discovery time.
    // The mouse pointer goes in LAST, into whatever surface this frame was rendered
    // into, and BEFORE the off-screen target is unbound and before the transfer copies
    // the frame -- otherwise it would land in the flat window and nowhere else.
    //
    // g_stereoRenderedFrame is still set at this point; the vr block below is what
    // clears it. It is the right question to ask, too: it is "does this frame hold two
    // eyes", not "is the stereo option on", and a menu answers no while the option is
    // on.
    presenterDrawCursor(g_stereoRenderedFrame);

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

    // Put the pixels under the pointer back, now that the frame has gone to the headset
    // and before the game presents.
    //
    // GPL's menu does not redraw the whole back buffer and roglv2 preserves it across the
    // swap, so a cursor drawn into it stays there for good -- observed as the pointer
    // painting a trail like a brush rather than moving. The saved pixels are microseconds
    // old and came from this same frame, which is why the restore belongs HERE and not at
    // the top of the next one: there is no window in which they can go stale.
    //
    // The flat window therefore stops showing the overlay. THAT IS DELIBERATE AND
    // CORRECT, not a regression: Windows draws the real pointer over the window already,
    // and the only surface that cannot see it is the headset -- whose copy was taken a
    // few lines above. Do not "fix" this by moving the restore after the present; that
    // puts the trail straight back.
    presenterRestoreUnderCursor();

    LARGE_INTEGER beforePresent, afterPresent;
    QueryPerformanceCounter(&beforePresent);
    const int presented = g_originalScreenDisplay ? g_originalScreenDisplay(surface) : 0;
    QueryPerformanceCounter(&afterPresent);
    g_profPresentTicks += afterPresent.QuadPart - beforePresent.QuadPart;
    // Our own cost is the whole hook minus the engine's present. Taken here rather than
    // at the bottom so the extra renders below -- which are whole frames of their own and
    // already reported separately -- do not land in it.
    g_profGplvrTicks += (afterPresent.QuadPart - g_profFrameEntry) -
                        (afterPresent.QuadPart - beforePresent.QuadPart);

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

    // Drain the pipeline here, and time the drain, to find out what `present` is made of.
    //
    // present costs 4.2-4.8 ms a frame -- more than the scene render -- and there are two
    // candidates. Either roglv2 flushes its polygon batch inside screenDisplay, in which
    // case present is CPU submission cost; or the GPU is behind and SwapBuffers is where
    // it catches up. A glFinish here answers it: if the finish is expensive and present
    // then collapses, the GPU was the queue; if the finish is cheap and present stays
    // where it was, the work is roglv2 running on the CPU.
    //
    // It changes the thing it measures -- a pipeline stall per frame costs frame rate --
    // so it is off unless asked for, and the frame rate under it is not comparable with
    // the frame rate without it.
    if (g_options.profileGpuWait) {
        static GlFinishFn finish = nullptr;
        static bool       resolved = false;
        if (!resolved) {
            resolved = true;
            HMODULE gl = GetModuleHandleA("opengl32.dll");
            if (gl)
                finish = reinterpret_cast<GlFinishFn>(GetProcAddress(gl, "glFinish"));
            if (!finish) logf("profile: glFinish did not resolve; no GPU-wait figure");
        }
        if (finish) {
            LARGE_INTEGER f0, f1;
            QueryPerformanceCounter(&f0);
            finish();
            QueryPerformanceCounter(&f1);
            g_profFinishTicks += f1.QuadPart - f0.QuadPart;
        }
    }
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

    // Which task are we on? Asked from inside the running game via the exe's own
    // inBackground/inForeground/inServer exports, because the call chain to the renderer
    // goes through a vtable slot and reading it statically left the answer ambiguous.
    taskRateReportThread("sceneRender");

    auto scene = static_cast<SceneParams*>(params);

    // A live p[43] IS the mirror pass -- gpl_sceneRender does the whole thing inside
    // `if (p[43])`. Deliberately NOT also testing p[85].b0: that bit means "copy the
    // rendered corner back into mirror.mip", and gpl_simFrameRender sets it only on the
    // fallback path. On a machine where DirectMirrorRendering engages, the mirror camera
    // targets the texture directly, nothing is copied, the bit is never set -- and a
    // counter that tested it would read zero while the mirror worked perfectly.
    if (scene->mirrorCamera) {
        ++g_mirrorRefreshes;
        if (!g_loggedMirrorPath) {
            g_loggedMirrorPath = true;
            logf("mirror: rendering by the %s. %s",
                 (scene->mirrorFlags & 1u) ? "COPY-OUT path" : "DIRECT render-target path",
                 (scene->mirrorFlags & 1u)
                     ? "gpl_simFrameRender builds a bitmap instance over a corner of the "
                       "frame, renders into it, copies it into mirror.mip, then frees the "
                       "instance and zeroes p[43]."
                     : "mirror.mip is itself a render target and p[43] is persistent, so "
                       "nothing is rebuilt per frame.");
        }
    }

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

}  // namespace

// Declared in Hooks.h rather than kept private: the cursor overlay has to turn a SCREEN
// position into a position in the client area of exactly this window, and no other HWND
// in the process answers that question.
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

namespace {

// Installed ahead of every other subclass, so it sees WM_ACTIVATEAPP first.
//
// The chain below us, innermost last: roglv2's rogl_windowProc, then gplc67's platform
// proc (0x4F4210), then its keyboard proc, then the class proc. Both draw gates live
// below, so intercepting here covers both with one hook.
WNDPROC g_originalWndProc = nullptr;

LRESULT CALLBACK activationWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Only the DEACTIVATION half, and only this message. Activations pass through
    // untouched so the gates still get set the first time the game is focused, and so
    // the game's own one-shot display restore keeps working exactly as it does now.
    if (message == WM_ACTIVATEAPP && wParam == FALSE && g_options.renderWhenUnfocused) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            logf("focus: WM_ACTIVATEAPP(FALSE) swallowed -- the game will keep rendering "
                 "and presenting while it is not the foreground app. Both draw gates "
                 "(roglv2 WindowManager+0x19 and gplc67 gpl_isForeground) keep the value "
                 "the last activation gave them.");
        }
        return 0;
    }
    return CallWindowProcA(g_originalWndProc, window, message, wParam, lParam);
}

// Lazily, because the window does not exist when the DLL loads and roglv2 subclasses it
// later still. Called once a second from enforceNotTopmost until it takes.
void installActivationHook() {
    if (g_originalWndProc || !g_options.renderWhenUnfocused) return;

    HWND window = gameWindow();
    if (!window) return;

    // SetWindowLongPtr puts us OUTERMOST, which is what we want: we see the message
    // before roglv2 and before the game, and swallowing it stops both.
    auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&activationWndProc)));
    if (!previous) {
        logf("focus: could not subclass the game window (error %lu) -- the game will "
             "still stop drawing when it loses focus",
             GetLastError());
        return;
    }
    g_originalWndProc = previous;
    logf("focus: window proc chained ahead of roglv2's and the game's, to swallow "
         "WM_ACTIVATEAPP(FALSE). renderWhenUnfocused = 1.");
}

void enforceNotTopmost() {
    installActivationHook();
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
    GetPrivateProfileStringA("gplvr", "vrMirrorYaw", "0.0", text, sizeof(text), path);
    g_options.vrMirrorYaw = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrMirrorPitch", "0.0", text, sizeof(text), path);
    g_options.vrMirrorPitch = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrMirrorFov", "0.0", text, sizeof(text), path);
    g_options.vrMirrorFov = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrMirrorBack", "0.0", text, sizeof(text), path);
    g_options.vrMirrorBack = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrMirrorUp", "0.0", text, sizeof(text), path);
    g_options.vrMirrorUp = static_cast<float>(atof(text));
    // Clamped, not trusted. A wild fov here would build a degenerate frustum, and the
    // symptom would look like a mirror bug rather than a typo in an ini.
    if (g_options.vrMirrorFov < 0.0f || g_options.vrMirrorFov > 170.0f)
        g_options.vrMirrorFov = 0.0f;
    if (g_options.vrMirrorYaw != 0.0f || g_options.vrMirrorPitch != 0.0f ||
        g_options.vrMirrorFov != 0.0f || g_options.vrMirrorBack != 0.0f ||
        g_options.vrMirrorUp != 0.0f)
        logf("options: mirror aim yaw=%+.1f pitch=%+.1f back=%+.2f up=%+.2f fov=%.1f "
             "(0 = the stock 40 degrees)",
             g_options.vrMirrorYaw, g_options.vrMirrorPitch, g_options.vrMirrorBack,
             g_options.vrMirrorUp, g_options.vrMirrorFov);
    g_options.stereoKey = GetPrivateProfileIntA("gplvr", "stereoKey", '1', path);
    g_options.profileFrame =
        GetPrivateProfileIntA("gplvr", "profileFrame", 1, path) != 0;
    g_options.shimMeasure = GetPrivateProfileIntA("gplvr", "shimMeasure", 0, path) != 0;
    g_options.billboardFaceEye =
        GetPrivateProfileIntA("gplvr", "billboardFaceEye", 1, path) != 0;
    g_options.profileGpuWait =
        GetPrivateProfileIntA("gplvr", "profileGpuWait", 0, path) != 0;
    g_options.profilePolys =
        GetPrivateProfileIntA("gplvr", "profilePolys", 0, path) != 0;
    g_options.swapInterval = GetPrivateProfileIntA("gplvr", "swapInterval", 0, path);
    if (g_options.swapInterval < -1) g_options.swapInterval = -1;
    if (g_options.swapInterval > 4) g_options.swapInterval = 4;
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
    g_options.renderWhenUnfocused =
        GetPrivateProfileIntA("gplvr", "renderWhenUnfocused", 0, path);
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

    // 0 by default, so an ini written before this existed does not silently gain a
    // cursor. The ini shipped with the mod turns it on, which is where the feature is
    // meant to be exercised from.
    g_options.vrCursor = GetPrivateProfileIntA("gplvr", "vrCursor", 0, path);
    if (g_options.vrCursor < 0 || g_options.vrCursor > 2) g_options.vrCursor = 0;
    logf("options: vrCursor=%d (%s)", g_options.vrCursor,
         g_options.vrCursor == 0
             ? "no pointer is drawn into the VR image"
             : (g_options.vrCursor == 1 ? "drawn while Windows says the pointer is "
                                          "showing"
                                        : "drawn whatever Windows says"));

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

    if (g_options.profilePolys) {
        void** poly = binding.raster.polyDraw;
        if (poly && *poly) {
            g_originalPolyDraw = *poly;
            void* thunk = reinterpret_cast<void*>(&polyDrawThunk);
            if (patchBytes(poly, &thunk, sizeof(thunk)))
                logf("profile: counting polygons -- polyDraw %p -> %p", g_originalPolyDraw,
                     thunk);
            else
                logf("profile: could not write the polyDraw global; no polygon count");
        } else {
            logf("profile: polyDraw was not discovered; no polygon count");
        }
    }
    return true;
}

void requestSceneRenderHook(const Binding& binding) {
    stereoBind(binding);
    renderTargetBind(binding);
    taskRateBind(&binding.image);

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
