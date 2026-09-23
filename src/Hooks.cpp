#include "Hooks.h"

#include "fast_text/FastText.h"
#include "DrawnMarks.h"
#include "FrameBudget.h"
#include "Hud.h"
#include "Interpolation.h"
#include "Letterbox.h"
#include "MenuScale.h"
#include "MirrorConfig.h"
#include "MirrorTarget.h"
#include "MouseLook.h"
#include "Log.h"
#include "PostProfile.h"
#include "Pribluda.h"
#include "RenderPacing.h"
#include "RenderProfile.h"
#include "SimRing.h"
#include "RenderTarget.h"
#include "Stereo.h"
#include "SwapControl.h"
#include "TaskRate.h"
#include "VrCamera.h"
#include "Window.h"
#include "presenter/Producer.h"

#include <cmath>
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
bool      g_vrHudLayerKeyLast = false;
bool      g_vrMirrorModeKeyLast = false;
bool      g_vrInterpolateKeyLast = false;
bool      g_dumpEyesKeyLast = false;
bool      g_dumpEyesRequested = false;
bool      g_dumpEyesLate = false;

// One eye's rect to a TGA, read from the framebuffer the headset's copy is taken from.
//
// The eye rects are in the coordinates of whatever the frame was rendered into. With the
// off-screen target that is its own framebuffer -- 6344x3172 for a 3172-per-eye headset --
// and by the time the dumps run it has already been unbound for the presenter, so a plain
// glReadPixels reads the WINDOW: the monitor's copy, where the right eye's rect points past
// the edge. A dump that only looks right when the target happens to fit the window and the
// window happens to show both eyes is not evidence of anything. GL_FRAMEBUFFER rather than
// the read target alone, because the EXT entry points do not know GL_READ_FRAMEBUFFER;
// both bindings are 0 again afterwards, which is what the present expects.
bool dumpEyeRect(GlFunctions& gl, const int rect[4], const char* path) {
    GLuint source = renderTargetFramebuffer();
    bool bound = source && glResolveFramebuffer(gl) && gl.bindFramebuffer;
    if (bound) gl.bindFramebuffer(kGlFramebuffer, source);
    bool ok = glDumpRectToTga(gl, rect[0], rect[1], rect[2], rect[3], path);
    if (bound) gl.bindFramebuffer(kGlFramebuffer, 0);
    return ok;
}
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

// Set the instant the headset's copy of the frame is taken, cleared when the next scene
// render begins. Anything the game draws while this is true is on the monitor's copy
// only: the monitor presents after the capture, the eyes do not.
bool      g_framePublished = false;

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

// gpl_sceneRender's MAIN world walk: `call gpl_worldRenderSegmentRange` at 0x487FAA, eight
// __cdecl arguments cleaned by the caller (`add esp, 20h`). The mirror walk at 0x487A4C calls
// the same function and is not touched. Its result is never read.
typedef void(__cdecl* WorldWalkFn)(int, int, int, int, int, int, int, int);
constexpr uintptr_t     kMainWalkSite = 0x00487FAA;
constexpr unsigned char kMainWalkStock[5] = {0xE8, 0xB1, 0xD9, 0xFF, 0xFF};
constexpr uintptr_t     kWorldWalk = 0x00485960;
bool g_mainWalkPending = false;
bool g_mainWalkRedirected = false;

// A mirror-only render (Stereo.cpp) fills one mirror pane and must leave the eye untouched,
// so the main walk is skipped. Everything else in the call runs: the per-pass resets after
// it (gpl_renderClearDrawnMarks, gpl_renderClearObjectRegistrations) keep the object lists
// consistent for the eye's own render that follows.
// THE MAIN WALK'S ARGUMENTS, for whoever comes back to this: a2 and a3 are the camera's dlong
// (the same value twice at cockpit field of view), a4 the heading as a binary angle whose
// magnitude picks the segment order at 0x40000000 and the distance clamps at 0x20000000 and
// 0x60000000, a5 an ahead bound read only past 135 degrees, a6 the behind-far bound, a7 the
// ahead-far bound, a8 the behind near and mid bounds. Repeatable comment at 0x487F8A, and the
// instruments that measured them are described in docs/audit-2026-09-18/85-instrument-archive.md.
void __cdecl mainWorldWalkThunk(int a1, int a2, int a3, int a4, int a5, int a6, int a7,
                                int a8) {
    if (stereoMirrorOnlyPass()) return;
    reinterpret_cast<WorldWalkFn>(kWorldWalk)(a1, a2, a3, a4, a5, a6, a7, a8);
}

unsigned g_sceneMirrorOnlyCalls = 0;  // mirror-only renders, counted apart from the eyes

void reportOncePerSecond() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const long long elapsed = now.QuadPart - g_windowStart.QuadPart;
    if (elapsed < g_frequency.QuadPart) return;
    int reporting = frameBudgetEnter(kSpanReport);

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

    // Mirror-only renders are not counted as renders, but their time is in the scene total
    // and so in "ms each": per eye render including the pane renders before it.
    char paneRenders[48] = {};
    if (g_sceneMirrorOnlyCalls)
        _snprintf_s(paneRenders, sizeof(paneRenders), _TRUNCATE, " + %.1f/s mirror-only",
                    g_sceneMirrorOnlyCalls / seconds);

    logf("fps %.1f | worldFrames %.1f/s, renders %.1f/s (x%.2f)%s, %.2f ms each, "
         "%.2f ms worst | transfer %.2f ms x%.1f/frame | %s%s%smode %s",
         g_frames / seconds, g_sceneHookCalls / seconds, g_sceneOrigCalls / seconds,
         g_sceneHookCalls ? static_cast<double>(g_sceneOrigCalls) / g_sceneHookCalls : 0.0,
         paneRenders, msPerRender, worstMs,
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
        char polys[64] = {};
        if (g_options.profilePolys)
            _snprintf_s(polys, sizeof(polys), _TRUNCATE, "  [%u polys/frame, %.0f ns each]",
                        g_profPolys / g_frames,
                        g_profPolys ? 1e6 * scene * g_frames / g_profPolys : 0.0);
        char extra[112] = {};
        if (g_options.profileGpuWait)
            _snprintf_s(extra, sizeof(extra), _TRUNCATE, "  [gpuWait %.2f of scene]%s",
                        g_profFinishTicks * toMs / g_frames, polys);
        else
            strcpy_s(extra, polys);

        // `other` used to be a subtraction, which made the largest term in the frame the
        // one nobody could account for. pre and post are measured, so the line now names
        // every part and the leftover is only clock error -- if `rest` is not near zero
        // the spans have stopped tiling and the numbers should not be trusted.
        // With profileStages the tiled frame replaces both lines below: its spans do not
        // overlap, and gpuWait is on its wait line.
        FrameSpans mean;
        FrameSpans worst;
        bool       tiled = frameBudgetReport(period, polys);
        if (!tiled && renderProfileTakeSpans(mean, worst)) {
            logf("frame %.2f ms = pre %.2f + scene %.2f + post %.2f + present %.2f + "
                 "gplvr %.2f (rest %.2f)%s | WORST %.2f = pre %.2f + scene %.2f + "
                 "post %.2f + present %.2f",
                 period, mean.pre, scene, mean.post, present, gplvr,
                 period - mean.pre - scene - mean.post - present - gplvr, extra,
                 worst.total, worst.pre, worst.scene, worst.post, worst.present);
        } else if (!tiled) {
            logf("frame %.2f ms (worst %.2f) = scene %.2f + present %.2f + gplvr %.2f + "
                 "other %.2f%s",
                 period, g_profPeriodWorst * toMs, scene, present, gplvr,
                 period - scene - present - gplvr, extra);
        }
        postProfileReport(period);
    }
    fastTextReport();

    // Both of these divide by FRAMES, so they read across against the frame budget above
    // rather than against each other.
    char geometry[288] = {};
    if (renderProfileFormatGeometry(geometry, sizeof(geometry), g_frames))
        logf("%s", geometry);

    // The simulation thread's cost per tick, from the window taskRateFormat closed above.
    char tickCost[200] = {};
    if (g_options.profileStages && taskRateFormatTickCost(tickCost, sizeof(tickCost)))
        logf("%s", tickCost);

    char present[512] = {};
    if (renderProfileFormatPresent(present, sizeof(present), seconds, g_frames))
        logf("%s", present);

    // `pre` split into spans that tile it, and above all into the part that is WORK and
    // the part that is the game asleep waiting for the next simulation tick. Takes no
    // window arguments: it averages over its own count of frames whose markers came out
    // in order, and says how many that was.
    char pre[512] = {};
    if (renderProfileFormatPre(pre, sizeof(pre))) logf("%s", pre);

    // The same frame as CPU WORK and CPU WAIT, and what a headset frame would have left.
    // After the frame budget's and the post split's own lines, because the GPU total on
    // the headroom line is the one whichever of them owns the timer has just drained.
    char workWait[640] = {};
    if (renderProfileFormatWorkWait(workWait, sizeof(workWait), seconds, g_frames))
        logf("%s", workWait);
    char headroom[512] = {};
    if (renderProfileFormatHeadroom(headroom, sizeof(headroom), seconds, g_frames))
        logf("%s", headroom);

    // Render pacing, once a second while tracked frames were rendered: with vrLockToHeadset = 0
    // it is the unpaced baseline for the same counters.
    char pacing[640] = {};
    if (renderPacingFormat(pacing, sizeof(pacing))) logf("%s", pacing);

    // World interpolation, once a second while world frames were latched.
    char interp[1280] = {};
    if (interpolationFormat(interp, sizeof(interp))) logf("%s", interp);

    // The drawn-mark list's peak and overflows, once a second while the world was drawn. Per
    // frame, so the clears read against the eye and mirror-only renders on the fps line.
    char marks[200] = {};
    if (drawnMarksFormat(marks, sizeof(marks), g_frames)) logf("%s", marks);

    char ring[512] = {};
    if (simRingFormat(ring, sizeof(ring), g_frames)) logf("%s", ring);

    // Which part of the track each eye pass selected to walk. A jump here is the world being
    // drawn from somewhere else while the view transform stays put.
    char walk[768] = {};
    if (stereoWalkFormat(walk, sizeof(walk))) logf("%s", walk);

    // After every line that reads them, and on every path -- a window that printed nothing
    // must still be drained, or its counts land in the next one.
    renderProfileEndWindow();
    frameBudgetEndWindow();

    g_windowStart = now;
    g_mirrorRefreshes = 0;
    g_frames = 0;
    g_sceneHookCalls = 0;
    g_sceneOrigCalls = 0;
    g_sceneMirrorOnlyCalls = 0;
    g_sceneTicks = 0;
    g_sceneTicksWorst = 0;
    g_profPresentTicks = 0;
    g_profGplvrTicks = 0;
    g_profPeriodTicks = 0;
    g_profPeriodWorst = 0;
    g_profFinishTicks = 0;
    g_profPolys = 0;
    frameBudgetLeave(reporting);
}

// Mirror tweak mode: aiming the glasses from the headset. It changes g_mirrorAim, which aimPane
// reads every frame, so each step shows at once. Leaving saves the aim to the viewed car's
// section of gplvr_cars.ini (MirrorConfig); gplvr.ini is never written. It shares the arrows
// and Page Up/Down with HUD tweak mode, so entering either leaves the other.
constexpr unsigned kMirrorTweakKeys[] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT};
constexpr double   kMirrorTweakMaxStepSeconds = 0.1;  // the most one poll turns after a stall
constexpr double   kMirrorTweakLogSeconds = 0.5;      // between progress lines while adjusting

struct MirrorTweak {
    bool     active = false;
    bool     keyLast = false;
    bool     unlogged = false;  // the values changed since the last line
    LONGLONG frequency = 0;
    LONGLONG lastPoll = 0;
    LONGLONG loggedAt = 0;
};

MirrorTweak g_mirrorTweak;

bool mirrorTweakKeyHeld(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

float mirrorTweakClamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

void mirrorTweakLogProgress(LONGLONG now) {
    g_mirrorTweak.unlogged = false;
    g_mirrorTweak.loggedAt = now;
    logf("mirror: yaw %.2f pitch %.2f fovScale %.3f", g_mirrorAim.yaw, g_mirrorAim.pitch,
         g_mirrorAim.fovScale);
}

void mirrorTweakEnter() {
    pribludaTweakExit(" -- mirror tweak mode entered");
    g_mirrorTweak.active = true;
    g_mirrorTweak.unlogged = false;
    const char* key = mirrorConfigKey();
    logf("mirror: tweak mode on for %s (%s) -- yaw %.2f pitch %.2f fovScale %.3f",
         key ? key : "no identified car", mirrorConfigPerCar() ? "per-car" : "global",
         g_mirrorAim.yaw, g_mirrorAim.pitch, g_mirrorAim.fovScale);
    logf("mirror: Left/Right turn the glasses, Up/Down tilt them, Page Up/Down widen and "
         "narrow the field; key 0x%02X again leaves",
         g_options.vrMirrorTweakKey);
}

// Once a frame from the key poll, after HUD tweak mode's.
void mirrorTweakPoll() {
    if (!g_options.vr || !g_options.vrMirrorTweakKey) return;

    LARGE_INTEGER counter;
    if (!g_mirrorTweak.frequency) {
        QueryPerformanceFrequency(&counter);
        g_mirrorTweak.frequency = counter.QuadPart;
    }
    QueryPerformanceCounter(&counter);
    LONGLONG now = counter.QuadPart;
    double seconds = g_mirrorTweak.lastPoll
                         ? static_cast<double>(now - g_mirrorTweak.lastPoll) /
                               static_cast<double>(g_mirrorTweak.frequency)
                         : 0.0;
    g_mirrorTweak.lastPoll = now;
    if (seconds > kMirrorTweakMaxStepSeconds) seconds = kMirrorTweakMaxStepSeconds;

    bool toggle = mirrorTweakKeyHeld(g_options.vrMirrorTweakKey);
    bool togglePressed = toggle && !g_mirrorTweak.keyLast;
    g_mirrorTweak.keyLast = toggle;

    // Keys typed into another window are not for it.
    if (GetForegroundWindow() != gameWindow()) return;
    if (togglePressed) {
        if (g_mirrorTweak.active) {
            mirrorTweakExit("");
        } else {
            mirrorTweakEnter();
        }
        return;
    }
    if (!g_mirrorTweak.active) return;

    int turn = (mirrorTweakKeyHeld(VK_RIGHT) ? 1 : 0) - (mirrorTweakKeyHeld(VK_LEFT) ? 1 : 0);
    int tilt = (mirrorTweakKeyHeld(VK_UP) ? 1 : 0) - (mirrorTweakKeyHeld(VK_DOWN) ? 1 : 0);
    int widen = (mirrorTweakKeyHeld(VK_PRIOR) ? 1 : 0) - (mirrorTweakKeyHeld(VK_NEXT) ? 1 : 0);
    if (!turn && !tilt && !widen) {
        // The end of a hold, which the rate limit may have kept from the log.
        if (g_mirrorTweak.unlogged) mirrorTweakLogProgress(now);
        return;
    }

    float yaw = g_mirrorAim.yaw;
    float pitch = g_mirrorAim.pitch;
    float fovScale = g_mirrorAim.fovScale;
    float angleStep = static_cast<float>(g_options.vrMirrorTweakAngleSpeed * seconds);
    if (turn) {
        g_mirrorAim.yaw = mirrorTweakClamp(yaw + turn * angleStep, -45.0f, 45.0f);
    }
    if (tilt) {
        g_mirrorAim.pitch = mirrorTweakClamp(pitch + tilt * angleStep, -45.0f, 45.0f);
    }
    if (widen) {
        float factor = static_cast<float>(
            std::pow(2.0, widen * g_options.vrMirrorTweakFovSpeed * seconds));
        g_mirrorAim.fovScale = mirrorTweakClamp(fovScale * factor, 0.25f, 6.0f);
    }
    if (g_mirrorAim.yaw != yaw || g_mirrorAim.pitch != pitch ||
        g_mirrorAim.fovScale != fovScale) {
        g_mirrorTweak.unlogged = true;
        mirrorConfigMarkEdited();
    }

    if (g_mirrorTweak.unlogged &&
        now - g_mirrorTweak.loggedAt >=
            static_cast<LONGLONG>(g_mirrorTweak.frequency * kMirrorTweakLogSeconds))
        mirrorTweakLogProgress(now);
}

// True for a key press the window proc should keep from the game: the toggle key whenever it
// toggles, which is in VR with a key set, and the adjusting keys while the mode is on. Only
// presses; releases still reach the game.
bool mirrorTweakSwallowsKey(unsigned virtualKey) {
    if (!g_options.vr || !g_options.vrMirrorTweakKey) return false;
    if (virtualKey == static_cast<unsigned>(g_options.vrMirrorTweakKey)) return true;
    if (!g_mirrorTweak.active) return false;
    for (unsigned key : kMirrorTweakKeys) {
        if (key == virtualKey) return true;
    }
    return false;
}

}  // namespace

void mirrorTweakExit(const char* why) {
    if (!g_mirrorTweak.active) return;
    g_mirrorTweak.active = false;
    g_mirrorTweak.unlogged = false;
    logf("mirror: tweak mode off -- yaw %.2f pitch %.2f fovScale %.3f%s", g_mirrorAim.yaw,
         g_mirrorAim.pitch, g_mirrorAim.fovScale, why);
    mirrorConfigSave();
}

namespace {

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

    // Armed here, taken at publish time -- the key is polled on the game thread and the
    // read has to happen where the frame is complete and still bound.
    if (g_options.dumpEyesKey) {
        const bool down = (GetAsyncKeyState(g_options.dumpEyesKey) & 0x8000) != 0;
        if (down && !g_dumpEyesKeyLast) {
            g_dumpEyesRequested = true;
            g_dumpEyesLate = true;
        }
        g_dumpEyesKeyLast = down;
    }

    // Recentre: whatever pose the driver is in becomes the seat. Needed because
    // OpenXR's LOCAL space has its origin on the floor, not at the head -- measured,
    // see STATUS.md -- so without this the view starts a standing height too high.
    if (g_options.vr && g_options.vrRecentreKey) {
        const bool down = (GetAsyncKeyState(g_options.vrRecentreKey) & 0x8000) != 0;
        if (down && !g_vrRecentreKeyLast) vrRequestRecentre();
        g_vrRecentreKeyLast = down;
    }

    // Pribluda's panel between the head and the seat. Tweak mode is left as it is, so an
    // element being placed can be looked at both ways. A seat panel with no seat published
    // would be invisible, so one is published on the spot.
    if (g_options.vr && g_options.vrHudLayerKey) {
        bool down = (GetAsyncKeyState(g_options.vrHudLayerKey) & 0x8000) != 0;
        if (down && !g_vrHudLayerKeyLast) {
            g_options.vrHudLayer = g_options.vrHudLayer == 1 ? 2 : 1;
            const char* seat = "";
            if (g_options.vrHudLayer == 2 && !presenterSeatPublished()) {
                seat = vrPublishSeat()
                           ? " -- no seat had been published, so the head pose now is the seat"
                           : " -- no seat yet and no head pose to take one from; it appears "
                             "after the first recentre";
            }
            logf("vr: HUD panel switched to %s%s",
                 g_options.vrHudLayer == 1 ? "follow the head" : "the seat", seat);
        }
        g_vrHudLayerKeyLast = down;
    }

    pribludaPollTweak();
    mirrorTweakPoll();

    // Per-eye mirrors on and off. The pass loop reads stereoMirror every frame and the main
    // world walk redirect is installed whenever it started non-zero, so 1 and 2 swap freely.
    if (g_options.vr && g_options.vrMirrorModeKey && g_options.stereoMirror != 0) {
        bool down = (GetAsyncKeyState(g_options.vrMirrorModeKey) & 0x8000) != 0;
        if (down && !g_vrMirrorModeKeyLast) {
            g_options.stereoMirror = g_options.stereoMirror == 2 ? 1 : 2;
            logf("mirror: stereoMirror now %d -- %s", g_options.stereoMirror,
                 g_options.stereoMirror == 2 ? "each eye renders both glasses (4 mirror renders)"
                                             : "both glasses once from the head centre (2 "
                                               "mirror renders)");
        }
        g_vrMirrorModeKeyLast = down;
    }

    // World interpolation on and off, for an A/B in one race. The records keep being copied
    // while it is off, so the blend resumes on the next frame.
    if (g_options.vr && g_options.vrInterpolateKey) {
        bool down = (GetAsyncKeyState(g_options.vrInterpolateKey) & 0x8000) != 0;
        if (down && !g_vrInterpolateKeyLast) {
            g_options.vrInterpolate = !g_options.vrInterpolate;
            logf("interp: vrInterpolate now %d -- %s", g_options.vrInterpolate ? 1 : 0,
                 g_options.vrInterpolate ? "the world is drawn blended to the display time"
                                         : "the world is drawn at the newest latched tick");
        }
        g_vrInterpolateKeyLast = down;
    }


    fastTextPollFrame();
    postProfilePollFrame();

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
void pollFocusEdge();                // defined below
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
    // An extra render publishes a frame between two paced ones.
    if (renderPacingEngaged()) {
        static bool loggedPaced = false;
        if (!loggedPaced) {
            loggedPaced = true;
            logf("vr: extra renders held at 0 while render pacing is engaged (vrExtraRenders = "
                 "%d applies to unpaced frames only)",
                 g_options.vrExtraRenders);
        }
        return;
    }
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
        // One wait span around the whole spin, so a yield costs no timestamps.
        int  waiting = frameBudgetEnter(kSpanWaitExtra);
        bool advanced = false;
        for (;;) {
            LARGE_INTEGER current;
            QueryPerformanceCounter(&current);
            if (current.QuadPart >= deadline) break;
            if (presenterViewSequence() != before) {
                advanced = true;
                break;
            }
            SwitchToThread();
        }
        frameBudgetLeave(waiting);
        if (!advanced) return;

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
            // Same reasoning as the cursor below, and the HUD needs it more: the extra
            // render has just painted the world over a HUD that was drawn once, in
            // gpl_simFrameRender, before this loop was reached. Without the replay the
            // HUD survives only on frames that had no extra render.
            hudReplay();

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
    frameBudgetSwitch(kSpanHousekeeping);

    // Every stampDraw of this frame has been issued by now, so its strings are complete.
    if (g_options.vr) hudEndFrame();

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

    // The scaled front end goes into the window's back buffer FIRST, before anything
    // reads that buffer. The VR transfer below is one of those readers, which is what
    // lets the headset show a scaled menu as well as the monitor.
    menuScaleEndFrameAndBlit();

    ++g_frames;
    taskRateReportThread("present");
    pollToggle();
    windowFrame();
    // Every frame, before the rate-limited housekeeping below: a focus change has to be
    // acted on when it happens, not up to a quarter of a second later.
    pollFocusEdge();
    letterboxFrame();
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
    // The in-race HUD a driver actually reads -- lap times, the boards, tyres, oil, gear
    // -- belongs to pribluda, which draws raw OpenGL inside roglv2's present for the
    // monitor. A second copy goes into the panel texture the headset shows as a quad
    // layer; it needs the game's GL context, which is current here.
    frameBudgetSwitch(kSpanPanel);
    pribludaDrawHud();

    frameBudgetSwitch(kSpanCursor);
    presenterDrawCursor(g_stereoRenderedFrame);

    // The off-screen frame has to stop being the draw target before the presenter
    // reads it and before the game presents: glCopyTexSubImage2D reads the READ
    // framebuffer, and SwapBuffers only ever shows the window's own.
    frameBudgetSwitch(kSpanUnbind);
    presenterEndFrame();

    frameBudgetSwitch(kSpanPublish);
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

        // FLUSH BEFORE READING THE FRAMEBUFFER, for the same reason the menu blit does.
        //
        // The user's eye dumps settled this: both eye rects together cover the whole
        // 2560x1440 buffer, neither contained the HUD, and the monitor -- which presents
        // from that same buffer a moment later -- showed it as always. So the HUD reaches
        // the buffer between our copy and SwapBuffers, and the only thing in between is
        // roglv2's screenDisplay, whose last-resort flush is exactly where anything still
        // queued gets issued. The glDrawArrays counter reads zero through a race because
        // it counts one entry point and a 2D flush does not use it.
        //
        // Everything gplvr takes off the framebuffer -- the dump below and the headset's
        // copy after it -- has to come after this.
        frameBudgetSwitch(kSpanFlushPending);
        rasterFlushPending();
        frameBudgetSwitch(kSpanPublish);

        // BEFORE the publish, so what lands on disk is exactly what is handed over.
        if (g_dumpEyesRequested) {
            g_dumpEyesRequested = false;
            GlFunctions gl;
            int rect[2][4];
            if (glResolve(gl) && stereoEyeRect(0, rect[0]) && stereoEyeRect(1, rect[1])) {
                // NEXT TO THE EXE, not wherever the process happens to be standing.
                // gplc67 runs with its working directory set to the `font` subfolder, so
                // the first version of this wrote two files nobody could find and
                // reported success doing it.
                char base[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, base, MAX_PATH);
                if (char* slash = strrchr(base, '\\')) slash[1] = '\0';

                char path[MAX_PATH];
                for (int eye = 0; eye < 2; ++eye) {
                    sprintf_s(path, "%sgplvr-eye%d.tga", base, eye);
                    bool ok = dumpEyeRect(gl, rect[eye], path);
                    logf("dump: eye %d rect %d,%d %dx%d of framebuffer %u -> %s%s", eye,
                         rect[eye][0], rect[eye][1], rect[eye][2], rect[eye][3],
                         renderTargetFramebuffer(), path, ok ? "" : "  <-- FAILED");
                }
            } else {
                logf("dump: nothing written -- GL did not resolve, or there are no eye "
                     "rects this frame (stereo is not running)");
            }
        }

        presenterFrame(g_stereoRenderedFrame, vrFrustumInjected());
        // WHAT THE HEADSET HAS ALREADY BEEN GIVEN. Anything drawn after this point
        // reaches the monitor, which presents later, and never reaches the eyes.
        g_framePublished = true;
        // Remembered before it is cleared: an extra render must only ever repeat a
        // frame the game actually rendered a tracked stereo world into. On a menu the
        // saved parameter block is stale, and re-running it would paint the sim over
        // the menu.
        g_lastFrameWasStereo = g_stereoRenderedFrame;
        // The same answer decides whether the next frame's latch is paced.
        renderPacingEndFrame(g_lastFrameWasStereo && vrFrustumInjected());
        g_stereoRenderedFrame = false;
    }

    // The window is a monitor of the off-screen frame, not the frame itself. Between
    // the presenter's copy and the present, so what the player sees on the desktop is
    // the same image that went to the headset.
    frameBudgetSwitch(kSpanWindowBlit);
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
    frameBudgetSwitch(kSpanCursor);
    presenterRestoreUnderCursor();
    frameBudgetSwitch(kSpanPublish);

    // A WINDOW IN THE BACKGROUND DOES NOT PRESENT.
    //
    // Clearing roglv2's draw gate on focus loss stops the rasterisation, but the swap
    // would still go through and put the same finished frame back on the glass 144 times
    // a second -- which on a window that exactly covers the output is the whole of "the
    // window I alt-tabbed to never appears". The engine's own rule is the same one
    // (gpl_presentIfForeground); it is simply never reached, because the WM_ACTIVATEAPP
    // that would set the flag is swallowed by DirectInput's subclass.
    //
    bool background = !keepDrawingWhileUnfocused() && !windowHasFocus();

    // THE LATE DUMP: the same two eye rects, taken as late as gplvr can reach, with only
    // roglv2's screenDisplay left between here and the swap.
    //
    // The early pair above is what the headset receives. If the HUD is in this pair and
    // not in that one, something draws it between the copy and the present -- pribluda is
    // the obvious candidate, since gplc67 loads it from the rasterizer's own startup and
    // it paints its overlay through a hook of its own. If it is in NEITHER pair while the
    // monitor shows it, it is drawn inside screenDisplay or after it, and the question
    // becomes which.
    if (g_dumpEyesLate) {
        g_dumpEyesLate = false;
        GlFunctions gl;
        int rect[2][4];
        if (glResolve(gl) && stereoEyeRect(0, rect[0]) && stereoEyeRect(1, rect[1])) {
            char base[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, base, MAX_PATH);
            if (char* slash = strrchr(base, '\\')) slash[1] = '\0';

            char path[MAX_PATH];
            for (int eye = 0; eye < 2; ++eye) {
                sprintf_s(path, "%sgplvr-eye%d-late.tga", base, eye);
                dumpEyeRect(gl, rect[eye], path);
            }
            logf("dump: the LATE pair is written too -- compare gplvr-eye0.tga (what the "
                 "headset got) against gplvr-eye0-late.tga (the last thing gplvr can see "
                 "before roglv2 presents). A difference between them is drawn by someone "
                 "else after our copy.");
        }
    }

    // Every stampDraw of the frame has been issued, so anything the fast text path still has
    // batched is drawn now -- before roglv2 presents, and after everything GPL drew over it.
    fastTextEndFrame();

    frameBudgetSwitch(kSpanPresent);
    postProfilePresentBegin();
    LARGE_INTEGER beforePresent, afterPresent;
    QueryPerformanceCounter(&beforePresent);
    renderProfileSetInPresent(true);
    const int presented =
        (!background && g_originalScreenDisplay) ? g_originalScreenDisplay(surface) : 0;
    renderProfileSetInPresent(false);
    QueryPerformanceCounter(&afterPresent);
    postProfilePresentEnd();
    renderProfileNoteFrame(beforePresent.QuadPart, afterPresent.QuadPart);
    frameBudgetEndFrame(afterPresent.QuadPart);
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

    // After the sim target, and it declines whenever that one took the frame: the two
    // are mutually exclusive by mode -- the off-screen target owns a world mode, this
    // owns the 640x480 front end -- and the invariant is checked rather than assumed.
    frameBudgetSwitch(kSpanRebindMenu);
    menuScaleBeginFrame();

    // Extra renders go AFTER the present, in the idle tail of the tick.
    //
    // Before it, they would delay the game's own frame by however long they take and
    // cost exactly the frame rate they are meant to improve. After it, they spend time
    // the game thread was going to spend waiting for the next 36 Hz task anyway.
    frameBudgetSwitch(kSpanExtra);
    frameBudgetSuppress(true);
    renderExtraFrames();
    frameBudgetSuppress(false);
    frameBudgetSwitch(kSpanTailGame);
    return presented;
}

// One timed invocation of the engine's sceneRender.
void __cdecl callOriginalSceneRender(void* params) {
    // A mirror-only render fills a pane for the eye render that follows; the frame budget
    // charges it to that eye's mirror spans and the report counts it apart.
    bool mirrorOnly = stereoMirrorOnlyPass();
    frameBudgetMirrorOnly(mirrorOnly);
    frameBudgetEyeBegin();
    postProfileSceneBegin();
    // Any text the fast path still has batched belongs under the world this call is about to
    // draw, so it goes to the framebuffer first -- and again afterwards, so a stamp issued
    // inside the scene cannot end up over polygons that followed it.
    fastTextFlushNow();
    LARGE_INTEGER begin, end;
    QueryPerformanceCounter(&begin);
    g_originalSceneRender(params);
    QueryPerformanceCounter(&end);
    fastTextFlushNow();
    postProfileSceneEnd();
    frameBudgetEyeEnd();
    frameBudgetMirrorOnly(false);

    const long long ticks = end.QuadPart - begin.QuadPart;
    if (mirrorOnly)
        ++g_sceneMirrorOnlyCalls;
    else
        ++g_sceneOrigCalls;
    g_sceneTicks += ticks;
    if (ticks > g_sceneTicksWorst) g_sceneTicksWorst = ticks;

    // GPL zeroes its counters at the top of sceneRender and reads them at the bottom, so
    // this is the one moment they describe the render that just finished. Outside the
    // timed span above, so the six loads do not land in the scene cost.
    renderProfileAfterRender();
    renderProfileNoteScene(begin.QuadPart, end.QuadPart);

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
            int waiting = frameBudgetEnter(kSpanWaitGpu);
            QueryPerformanceCounter(&f0);
            finish();
            QueryPerformanceCounter(&f1);
            frameBudgetLeave(waiting);
            g_profFinishTicks += f1.QuadPart - f0.QuadPart;
        }
    }
}

bool g_loggedOrientation = false;
bool g_loggedMirror = false;

// sceneRender overwrites the camera's fov with the game's own, 27 bytes before it calls
// cameraBind. This hook sits on those two calls, so it runs after that write and before
// cameraBind rebuilds the projection -- the only window where an eye frustum survives.
//
// The original bind is where roglv2 flushes the previous pass's queued polygons (cameraBind
// -> polySetDest), so the frame budget charges it to that pass's flush span.
void __cdecl cameraBindHook(void* camera) {
    auto* scene = static_cast<SceneParams*>(g_lastSceneParams);
    frameBudgetBindBegin();
    stereoOnCameraBind(camera);
    if (g_originalCameraBind) g_originalCameraBind(camera);
    mirrorTargetAfterBind(camera, scene);
    frameBudgetBindEnd(camera && scene && camera == scene->mirrorCamera);
}

void __cdecl sceneRenderHook(void* params) {
    frameBudgetSceneBegin();
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

    // The HUD's twenty drawDebugText calls come AFTER this returns, inline in
    // gpl_simFrameRender, so this is where last frame's recording is dropped -- and
    // where the thunk is put back if a mode change re-resolved it away.
    g_framePublished = false;
    if (g_options.vr) {
        hudEnsureThunk();
        hudBeginFrame();
    }

    // The lobby's TV feed is a world render issued while the engine is still in the
    // front end, and it is the one draw in the menu that does not come from the menu's
    // own code. Asking where it lands separates "the framebuffer flickers" from "this
    // particular draw misses the framebuffer", which the four scaling gates could not.
    if (windowInFrontEnd()) menuScaleReportBinding("sceneRender in the front end");

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

    // The widescreen field of view, on the flat path only. In VR the runtime's per-eye
    // fov wins and Stereo builds its own cameras, so there is nothing here to correct;
    // sceneRender stamps p[40] onto whatever camera sits in p[2] on the line before
    // cameraBind, which is why writing it here reaches the projection at all.
    float savedFov = scene->mainFov;
    bool  fovCorrected = !g_options.vr && !g_options.stereo &&
                        windowCorrectMainFov(savedFov, scene->mainFov);

    if (g_options.stereo && stereoRender(scene, &callOriginalSceneRender))
        g_stereoRenderedFrame = true;
    else
        callOriginalSceneRender(params);
    mirrorTargetEndScene();

    if (fovCorrected) scene->mainFov = savedFov;
    if (overridden)
        std::memcpy(scene->orientation, savedOrientation, sizeof(savedOrientation));
    frameBudgetSceneEnd();
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

    {
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

    {
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

    {
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

    {
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

    if (g_mainWalkPending) {
        g_mainWalkPending = false;
        auto site = reinterpret_cast<unsigned char*>(kMainWalkSite);
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&mainWorldWalkThunk) - (kMainWalkSite + 5));
        if (std::memcmp(site, kMainWalkStock, sizeof(kMainWalkStock)) != 0) {
            logf("mirror: %08X does not hold the stock `call gpl_worldRenderSegmentRange` "
                 "(%02X %02X %02X %02X %02X) -- per-pane mirrors are off",
                 static_cast<unsigned>(kMainWalkSite), site[0], site[1], site[2], site[3],
                 site[4]);
        } else if (!patchBytes(site + 1, &rel, sizeof(rel))) {
            logf("mirror: could not write the main world walk call at %08X -- per-pane "
                 "mirrors are off",
                 static_cast<unsigned>(kMainWalkSite));
        } else {
            g_mainWalkRedirected = true;
            logf("mirror: main world walk call at %08X redirected to %08X, which skips it "
                 "during a mirror-only render",
                 static_cast<unsigned>(kMainWalkSite),
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(&mainWorldWalkThunk)));
        }
    }
}

BOOL WINAPI clipCursorHook(const RECT* rect) {
    ApiGuard guard;
    if (guard.reentered) {
        logf("re-entry into clipCursorHook -- passing through inert");
        return TRUE;
    }


    {
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

    if (insertAfter == HWND_TOPMOST) {
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

// ALT-TAB LEAVES GPL HOLDING THE ALT KEY, AND THAT IS WHY THE BUTTONS STOP.
//
// gplc67's keyboard proc sets bit 0x100000 of its modifier word on
// WM_SYSKEYDOWN(VK_MENU). The matching WM_SYSKEYUP is delivered to whatever window TOOK
// focus, so it never arrives, and nothing in any focus path clears the bit. The binding
// evaluator then gates joystick BUTTONS and KEYBOARD actions behind an EQUALITY test of
// the binding's recorded modifier against the live one -- so every binding recorded with
// no modifier stops matching, permanently. AXES do not go through that test, which is
// exactly why they keep working and the buttons do not.
//
// The fix sends the key release the game never received, through the game's own handler.
// Deliberately not writing the modifier word directly: GPL's keyboard state lives in two
// places -- the modifier word and a per-key table -- and its own WM_*KEYUP arm is the one
// piece of code that is guaranteed to agree with itself about both.
void releaseStuckModifiers(HWND window, bool activating) {
    if (!g_originalWndProc) return;

    struct Release {
        UINT   message;
        WPARAM key;
        UINT   scan;
    };
    static const Release kReleases[] = {
        {WM_SYSKEYUP, VK_MENU, 0x38},
        {WM_KEYUP, VK_CONTROL, 0x1D},
        {WM_KEYUP, VK_SHIFT, 0x2A},
    };

    for (const Release& release : kReleases) {
        // Repeat count 1, the key's scan code, previous state down, transition up --
        // the lParam a real release carries.
        LPARAM lParam = static_cast<LPARAM>((release.scan << 16) | 1u | (1u << 30) |
                                            (1u << 31));
        CallWindowProcA(g_originalWndProc, window, release.message, release.key, lParam);
    }

    // Logged for the first few, with the activation state, because "the fix did not
    // work" and "the fix did not run" look identical from outside and were argued about
    // once already. If these lines are absent after an alt-tab, the handler is not being
    // reached and the keyboard is not the thing to look at.
    static int announced = 0;
    if (announced < 4) {
        ++announced;
        logf("focus: released Alt, Ctrl and Shift through the game's own keyboard proc "
             "(WM_SYSKEYUP -> sub_4C3060 -> sub_4C3620 -> sub_4C2350). "
             "Alt-Tab sends GPL the Alt PRESS and sends the release to the window that "
             "took focus, and GPL gates every unmodified button and key binding on the "
             "modifier word matching exactly -- so without this, coming back from "
             "alt-tab leaves the buttons dead while the axes still work. "
             "WM_ACTIVATEAPP(%s).",
             activating ? "TRUE" : "FALSE");
    }
}

// DOES OUR WINDOW PROC RUN AT ALL, AND DOES WM_ACTIVATEAPP REACH IT?
//
// Asked directly, because every indirect answer so far has been wrong. Identity tests on
// the installed proc cannot answer it -- GetWindowLongPtrA returns an opaque HANDLE, not
// an address, whenever the proc is not ANSI-callable, which made a comparison against our
// own function fail and cost a re-install that called itself until the stack ran out.
// A counter cannot be misread that way.
unsigned g_wndProcMessages = 0;
unsigned g_wndProcActivateApp = 0;

LRESULT CALLBACK activationWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ++g_wndProcMessages;
    if (message == WM_ACTIVATEAPP) ++g_wndProcActivateApp;

    // THE CHAIN OUTLIVES THE MODULE IT POINTS INTO, AND THAT IS A CRASH.
    //
    // g_originalWndProc is roglv2's proc (or DirectInput's, which forwards to it).
    // roglv2 is unloaded at shutdown while the window still exists, so every message
    // after that moment was being forwarded into freed memory. Windows reports it as
    // "roglv2.dll_unloaded" at fault offset 0x18470 -- rogl_windowProc exactly -- with
    // 0xC0000005 followed by 0xC000041D, the code for a fault inside a user-mode
    // callback. Four entries in the user's event log across two sessions.
    //
    // WM_NCDESTROY is the last message any window receives, so unchaining here restores
    // whatever was in front of us for the teardown itself and leaves nothing of ours
    // behind. `unchained` is the belt to that brace: once the window is gone we answer
    // for ourselves rather than calling a pointer we can no longer vouch for.
    static bool unchained = false;
    if (unchained) return DefWindowProcA(window, message, wParam, lParam);

    if (message == WM_NCDESTROY) {
        WNDPROC previous = g_originalWndProc;
        unchained = true;
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        g_originalWndProc = nullptr;
        logf("window: proc unchained at WM_NCDESTROY -- roglv2 is unloaded before the "
             "window dies, and forwarding into it afterwards is the shutdown crash at "
             "rogl_windowProc.");
        return previous ? CallWindowProcA(previous, window, message, wParam, lParam)
                        : DefWindowProcA(window, message, wParam, lParam);
    }

    // Both halves, and before anything else can swallow the message. On the way out
    // because the press must not outlive the focus; on the way back in because that is
    // when it has to be clean, and because the swallow below means the chain may never
    // have seen the deactivation at all.
    if (message == WM_ACTIVATEAPP) releaseStuckModifiers(window, wParam != FALSE);

    // The window and menu-scaling messages first. It may answer the message outright
    // (WM_GETMINMAXINFO, WM_CLOSE) or rewrite lParam and let it carry on down the chain,
    // which is what the mouse remap does -- the platform wndproc below takes the click
    // position straight out of lParam.
    LRESULT handled = 0;
    if (windowHandleMessage(window, message, wParam, lParam, handled)) return handled;

    // HUD tweak mode steers with the arrows and Page Up/Down. GPL takes its keyboard only
    // from these messages, in the keyboard proc below, so the presses stop here while the
    // mode is on. Only the presses: a release still reaches the game, which keeps its key
    // table right for a key held down across entering or leaving the mode.
    if (message == WM_KEYDOWN && pribludaTweakSwallowsKey(static_cast<unsigned>(wParam)))
        return 0;

    // Mirror tweak mode steers with the same keys, and its toggle key stops here as well.
    if (message == WM_KEYDOWN && mirrorTweakSwallowsKey(static_cast<unsigned>(wParam)))
        return 0;
    if (message == WM_KEYDOWN && g_options.vr && g_options.vrMirrorModeKey &&
        static_cast<unsigned>(wParam) == static_cast<unsigned>(g_options.vrMirrorModeKey))
        return 0;

    // Only the DEACTIVATION half, and only this message. Activations pass through
    // untouched so the gates still get set the first time the game is focused, and so
    // the game's own one-shot display restore keeps working exactly as it does now.
    if (message == WM_ACTIVATEAPP && wParam == FALSE && keepDrawingWhileUnfocused()) {
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
    return g_originalWndProc
               ? CallWindowProcA(g_originalWndProc, window, message, wParam, lParam)
               : DefWindowProcA(window, message, wParam, lParam);
}

// Lazily, because the window does not exist when the DLL loads and roglv2 subclasses it
// later still. Called once a second from enforceNotTopmost until it takes.
void installActivationHook() {
    // ARE WE STILL THE WINDOW PROC?
    //
    // This used to return the moment it had installed once, on the assumption that a
    // subclass stays where it was put. Nothing checked, and when the Alt release and the
    // cursor re-clip both turned out to be silent -- same handler, no log line, in a
    // build that provably contained them -- there was no way to tell "the fix is wrong"
    // from "the fix never ran". Now the invariant is tested every time round and a
    // displacement is loud, because a chain we are no longer in is the one failure that
    // makes every window feature look individually broken.
    if (g_originalWndProc) {
        HWND current = gameWindow();
        if (!current) return;
        auto installed = reinterpret_cast<WNDPROC>(
            GetWindowLongPtrA(current, GWLP_WNDPROC));
        if (installed == &activationWndProc) return;

        // NOT NECESSARILY DISPLACED, AND NEVER RE-CHAIN ON THIS EVIDENCE.
        //
        // GetWindowLongPtrA returns an opaque HANDLE rather than an address when the
        // window proc is not ANSI-callable -- 0xFFFF04CB on a window of 0x001204CC, the
        // low word one apart, is Windows' own encoding and not somebody's code. The
        // first version of this read that as a displacement, re-installed on top of
        // ITSELF, and set our predecessor to our own function: the proc called itself
        // until the stack ran out, which is the crash that came back as 0xc00000fd.
        //
        // So this reports and does nothing. The message counters above are the real
        // instrument; identity here is only worth logging.
        // NAME THE INTERLOPER, AND NEVER RE-CHAIN.
        //
        // GetWindowLongPtrA returns a 0xFFFFxxxx COOKIE, not an address, when the
        // current procedure is Unicode and the caller is ANSI -- so the A variant cannot
        // answer this and a comparison against it once reported a displacement that did
        // not exist, re-chained gplvr onto itself, and blew the stack.
        //
        // The W variant returns the real address, and that is worth having: gpl_winMain
        // registers exactly ONE class, with RegisterClassA, and the image contains no
        // SetWindowLongW at all -- so a window that reports IsWindowUnicode TRUE can only
        // have got that way from somebody writing GWLP_WNDPROC with the W variant AFTER
        // us. VirtualQuery plus GetModuleFileName turns that address into a filename.
        static bool complained = false;
        if (!complained) {
            complained = true;

            auto unicodeProc = reinterpret_cast<void*>(
                GetWindowLongPtrW(current, GWLP_WNDPROC));
            char owner[MAX_PATH] = "unknown";
            MEMORY_BASIC_INFORMATION info = {};
            if (unicodeProc && VirtualQuery(unicodeProc, &info, sizeof(info))) {
                GetModuleFileNameA(static_cast<HMODULE>(info.AllocationBase), owner,
                                   MAX_PATH);
            }
            const char* leaf = strrchr(owner, '\\');

            logf("window: the current proc is %p (ANSI view: %p, a cookie), ours is %p, "
                 "unicode window: %d, and it lives in %s. gplc67 registers one class "
                 "with RegisterClassA and never calls SetWindowLongW, so a Unicode "
                 "window means somebody subclassed AFTER us. NOT re-chaining -- the "
                 "displacer keeps ours as its predecessor and reinstalling makes a ring.",
                 unicodeProc, reinterpret_cast<void*>(installed),
                 reinterpret_cast<void*>(&activationWndProc),
                 IsWindowUnicode(current) ? 1 : 0, leaf ? leaf + 1 : owner);
        }
        return;
    }
    // Unconditional now. It began as one tenant that had to be asked for -- swallowing
    // WM_ACTIVATEAPP(FALSE) -- and gained three more: WM_GETMINMAXINFO and WM_CLOSE for
    // the borderless window, the click position for the scaled front end, and the stuck
    // Alt release. That last one is a fix for a stock GPL defect that costs every player
    // their joystick buttons after an alt-tab, and gating a bug fix behind an unrelated
    // option would be a way of not shipping it.
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
    logf("window: proc chained ahead of roglv2's and the game's");
}

// FOCUS IS POLLED, NOT DELIVERED.
//
// Every focus-dependent thing in gplvr hung off WM_ACTIVATEAPP, on the authority of
// three reports and roglv2's own dispatcher, which handles it. On this machine THE
// MESSAGE IS NEVER SENT: the counters recorded 1373 messages reaching our proc across
// four focus changes and not one of them was WM_ACTIVATEAPP. The corroboration is that
// GPL kept rendering the world while unfocused -- roglv2's draw gate is cleared by that
// same message, so it did not get it either.
//
// GetForegroundWindow answers correctly and costs nothing, so the edge is taken from
// there. Choosing the signal that works over the signal that is documented is not a
// workaround; the documented one was measured and it is not there.
void pollFocusEdge() {
    HWND window = gameWindow();
    if (!window) return;

    int focused = window == GetForegroundWindow() ? 1 : 0;
    static int last = -1;
    if (focused == last) return;
    last = focused;

    releaseStuckModifiers(window, focused != 0);
    windowOnFocusChanged(focused != 0);
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
    // Foreground is in the change test, not just the printout. It is the independent
    // witness for whether WM_ACTIVATEAPP is reaching our proc at all: this line comes
    // from the frame hook, the focus line comes from the window proc, and a focus change
    // that produces one without the other says exactly which of the two is broken.
    static LONG lastStyle = 0, lastExStyle = 0;
    static RECT lastRect = {};
    static int lastForeground = -1;
    int foreground = window == GetForegroundWindow() ? 1 : 0;
    if (style != lastStyle || exStyle != lastExStyle || foreground != lastForeground ||
        memcmp(&rect, &lastRect, sizeof(rect)) != 0) {
        lastStyle = style;
        lastExStyle = exStyle;
        lastRect = rect;
        lastForeground = foreground;

        char className[64] = {};
        GetClassNameA(window, className, sizeof(className));
        logf("window %p '%s' style=%08lX exStyle=%08lX rect=%ld,%ld..%ld,%ld "
             "topmost=%d foreground=%d desktop=%dx%d | our proc has seen %u messages, "
             "%u of them WM_ACTIVATEAPP",
             window, className, style, exStyle, rect.left, rect.top, rect.right,
             rect.bottom, (exStyle & WS_EX_TOPMOST) != 0, foreground,
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
             g_wndProcMessages, g_wndProcActivateApp);
    }

    // The geometry itself belongs to the window module, which owns the policy and the
    // one number -- the target rect -- that the mode override and the menu blit are both
    // derived from. Before the topmost early-out below, because the two are independent
    // and a player who wants the window on top still wants it the right size.
    windowEnforceGeometry();

    if ((exStyle & WS_EX_TOPMOST) != 0) {
        logf("demoting WS_EX_TOPMOST on %p", window);
        SetWindowLongA(window, GWL_EXSTYLE, exStyle & ~WS_EX_TOPMOST);
        realUser32<decltype(&SetWindowPos)>("SetWindowPos")(
            window, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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

void windowProcUnchain() {
    if (!g_originalWndProc) return;

    HWND window = gameWindow();
    WNDPROC previous = g_originalWndProc;
    g_originalWndProc = nullptr;

    // Only if it is still OURS. Somebody who subclassed after us -- DirectInput does --
    // would otherwise be torn out of the chain along with us, and their proc would then
    // be the one pointing at freed memory.
    if (window && IsWindow(window)) {
        auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(window, GWLP_WNDPROC));
        if (current == &activationWndProc)
            SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
    }
}


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
    g_options.stereoMirror = GetPrivateProfileIntA("gplvr", "stereoMirror", 2, path);
    if (g_options.stereoMirror < 0 || g_options.stereoMirror > 2) g_options.stereoMirror = 2;
    GetPrivateProfileStringA("gplvr", "vrMirrorFovScale", "1.0", text, sizeof(text), path);
    g_options.vrMirrorFovScale = static_cast<float>(atof(text));
    if (!(g_options.vrMirrorFovScale >= 0.25f)) g_options.vrMirrorFovScale = 0.25f;
    if (g_options.vrMirrorFovScale > 6.0f) g_options.vrMirrorFovScale = 6.0f;
    auto readMirrorAngle = [&](const char* key) {
        GetPrivateProfileStringA("gplvr", key, "0.0", text, sizeof(text), path);
        float degrees = static_cast<float>(atof(text));
        if (!(degrees >= -45.0f)) degrees = -45.0f;
        if (degrees > 45.0f) degrees = 45.0f;
        return degrees;
    };
    g_options.vrMirrorYaw = readMirrorAngle("vrMirrorYaw");
    g_options.vrMirrorPitch = readMirrorAngle("vrMirrorPitch");
    g_options.vrMirrorOffscreen =
        GetPrivateProfileIntA("gplvr", "vrMirrorOffscreen", 1, path) != 0;
    g_options.vrMirrorTweakKey = GetPrivateProfileIntA("gplvr", "vrMirrorTweakKey", 'M', path);
    g_options.vrMirrorModeKey = GetPrivateProfileIntA("gplvr", "vrMirrorModeKey", 'N', path);
    GetPrivateProfileStringA("gplvr", "vrMirrorTweakAngleSpeed", "10.0", text, sizeof(text),
                             path);
    g_options.vrMirrorTweakAngleSpeed = static_cast<float>(atof(text));
    if (!(g_options.vrMirrorTweakAngleSpeed >= 0.5f)) g_options.vrMirrorTweakAngleSpeed = 0.5f;
    if (g_options.vrMirrorTweakAngleSpeed > 90.0f) g_options.vrMirrorTweakAngleSpeed = 90.0f;
    GetPrivateProfileStringA("gplvr", "vrMirrorTweakFovSpeed", "0.5", text, sizeof(text), path);
    g_options.vrMirrorTweakFovSpeed = static_cast<float>(atof(text));
    if (!(g_options.vrMirrorTweakFovSpeed >= 0.05f)) g_options.vrMirrorTweakFovSpeed = 0.05f;
    if (g_options.vrMirrorTweakFovSpeed > 4.0f) g_options.vrMirrorTweakFovSpeed = 4.0f;
    g_options.stereoKey = GetPrivateProfileIntA("gplvr", "stereoKey", '1', path);
    g_options.profileFrame =
        GetPrivateProfileIntA("gplvr", "profileFrame", 1, path) != 0;
    g_options.profileGpuWait =
        GetPrivateProfileIntA("gplvr", "profileGpuWait", 0, path) != 0;
    g_options.profilePolys =
        GetPrivateProfileIntA("gplvr", "profilePolys", 0, path) != 0;
    g_options.profileStages =
        GetPrivateProfileIntA("gplvr", "profileStages", 0, path) != 0;
    g_options.profilePost = GetPrivateProfileIntA("gplvr", "profilePost", 0, path) != 0;
    g_options.profileWorkWait =
        GetPrivateProfileIntA("gplvr", "profileWorkWait", 0, path) != 0 ||
        g_options.profileStages;
    g_options.profileStallUs = GetPrivateProfileIntA("gplvr", "profileStallUs", 50, path);
    if (g_options.profileStallUs < 0) g_options.profileStallUs = 0;
    if (g_options.profileStallUs > 10000) g_options.profileStallUs = 10000;
    g_options.profileTargetHz = GetPrivateProfileIntA("gplvr", "profileTargetHz", 90, path);
    if (g_options.profileTargetHz < 30) g_options.profileTargetHz = 30;
    if (g_options.profileTargetHz > 240) g_options.profileTargetHz = 240;
    g_options.fastText = GetPrivateProfileIntA("gplvr", "fastText", 1, path) != 0;
    g_options.fastTextKey = GetPrivateProfileIntA("gplvr", "fastTextKey", '9', path);
    g_options.dumpEyesKey = GetPrivateProfileIntA("gplvr", "dumpEyesKey", '0', path);
    g_options.swapInterval = GetPrivateProfileIntA("gplvr", "swapInterval", 0, path);
    if (g_options.swapInterval < -1) g_options.swapInterval = -1;
    if (g_options.swapInterval > 4) g_options.swapInterval = 4;
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
    g_options.windowWidth = GetPrivateProfileIntA("gplvr", "windowWidth", 0, path);
    g_options.windowHeight = GetPrivateProfileIntA("gplvr", "windowHeight", 0, path);
    if (g_options.windowWidth < 0) g_options.windowWidth = 0;
    if (g_options.windowHeight < 0) g_options.windowHeight = 0;
    // One of the pair on its own would give a window whose other dimension is the whole
    // monitor, which is never what anybody means by it.
    if ((g_options.windowWidth == 0) != (g_options.windowHeight == 0)) {
        logf("options: windowWidth and windowHeight must be given together -- ignoring "
             "both and covering the monitor exactly");
        g_options.windowWidth = 0;
        g_options.windowHeight = 0;
    }
    g_options.windowX = GetPrivateProfileIntA("gplvr", "windowX", -1, path);
    g_options.windowY = GetPrivateProfileIntA("gplvr", "windowY", -1, path);
    g_options.renderWhenUnfocused =
        GetPrivateProfileIntA("gplvr", "renderWhenUnfocused", 0, path) != 0;
    g_options.menuFilter = GetPrivateProfileIntA("gplvr", "menuFilter", 2, path);
    if (g_options.menuFilter < 0 || g_options.menuFilter > 2) g_options.menuFilter = 2;
    g_options.enableLetterbox =
        GetPrivateProfileIntA("gplvr", "enableLetterbox", 0, path) != 0;
    g_options.widescreenFix =
        GetPrivateProfileIntA("gplvr", "widescreenFix", 0, path) != 0;
    GetPrivateProfileStringA("gplvr", "vrMenuScale", "0.5", text, sizeof(text), path);
    g_options.vrMenuScale = static_cast<float>(atof(text));
    GetPrivateProfileStringA("gplvr", "vrGamma", "1.0", text, sizeof(text), path);
    g_options.vrGamma = static_cast<float>(atof(text));
    if (g_options.vrGamma < 0.5f) g_options.vrGamma = 0.5f;
    if (g_options.vrGamma > 3.0f) g_options.vrGamma = 3.0f;
    GetPrivateProfileStringA("gplvr", "vrHudDepth", "0.0", text, sizeof(text), path);
    g_options.vrHudDepth = static_cast<float>(atof(text));
    g_options.vrHudLayer = GetPrivateProfileIntA("gplvr", "vrHudLayer", 2, path);
    if (g_options.vrHudLayer < 0 || g_options.vrHudLayer > 2) g_options.vrHudLayer = 2;
    // A key that is absent keeps the default the Options struct was built with.
    auto readFloat = [&](const char* key, float& value) {
        GetPrivateProfileStringA("gplvr", key, "", text, sizeof(text), path);
        if (text[0]) value = static_cast<float>(atof(text));
    };
    auto clampPlacement = [](HudPlacement& placement) {
        if (placement.ahead < 0.10f) placement.ahead = 0.10f;
        if (placement.width < 0.05f) placement.width = 0.05f;
        if (placement.width > 5.0f) placement.width = 5.0f;
    };
    readFloat("vrHudHeadAhead", g_options.vrHudHead.ahead);
    readFloat("vrHudHeadDown", g_options.vrHudHead.down);
    readFloat("vrHudHeadRight", g_options.vrHudHead.right);
    readFloat("vrHudHeadWidth", g_options.vrHudHead.width);
    clampPlacement(g_options.vrHudHead);
    readFloat("vrHudSeatAhead", g_options.vrHudSeat.ahead);
    readFloat("vrHudSeatDown", g_options.vrHudSeat.down);
    readFloat("vrHudSeatRight", g_options.vrHudSeat.right);
    readFloat("vrHudSeatWidth", g_options.vrHudSeat.width);
    clampPlacement(g_options.vrHudSeat);
    readFloat("vrHudBackdropOpacity", g_options.vrHudBackdropOpacity);
    if (g_options.vrHudBackdropOpacity < 0.0f) g_options.vrHudBackdropOpacity = 0.0f;
    if (g_options.vrHudBackdropOpacity > 1.0f) g_options.vrHudBackdropOpacity = 1.0f;
    GetPrivateProfileStringA("gplvr", "vrHudTextScale", "1.0", text, sizeof(text), path);
    g_options.vrHudTextScale = static_cast<float>(atof(text));
    if (g_options.vrHudTextScale < 0.25f) g_options.vrHudTextScale = 0.25f;
    if (g_options.vrHudTextScale > 4.0f) g_options.vrHudTextScale = 4.0f;
    g_options.vrHudLayerKey = GetPrivateProfileIntA("gplvr", "vrHudLayerKey", VK_HOME, path);
    g_options.vrHudTweakKey = GetPrivateProfileIntA("gplvr", "vrHudTweakKey", VK_END, path);
    GetPrivateProfileStringA("gplvr", "vrHudTweakSpeed", "0.10", text, sizeof(text), path);
    g_options.vrHudTweakSpeed = static_cast<float>(atof(text));
    if (g_options.vrHudTweakSpeed < 0.005f) g_options.vrHudTweakSpeed = 0.005f;
    if (g_options.vrHudTweakSpeed > 2.0f) g_options.vrHudTweakSpeed = 2.0f;
    g_options.vrHudEye = GetPrivateProfileIntA("gplvr", "vrHudEye", 0, path);
    if (g_options.vrHudEye < 0 || g_options.vrHudEye > 2) g_options.vrHudEye = 0;
    g_options.vrPredictPosition =
        GetPrivateProfileIntA("gplvr", "vrPredictPosition", 1, path) != 0;
    g_options.vrLockToHeadset = GetPrivateProfileIntA("gplvr", "vrLockToHeadset", 1, path) != 0;
    GetPrivateProfileStringA("gplvr", "vrPaceMarginMs", "2.0", text, sizeof(text), path);
    g_options.vrPaceMarginMs = static_cast<float>(atof(text));
    if (!(g_options.vrPaceMarginMs >= 0.0f)) g_options.vrPaceMarginMs = 0.0f;
    if (g_options.vrPaceMarginMs > 10.0f) g_options.vrPaceMarginMs = 10.0f;
    g_options.vrInterpolate = GetPrivateProfileIntA("gplvr", "vrInterpolate", 1, path) != 0;
    if (g_options.vrLockToHeadset && !g_options.vrInterpolate) {
        g_options.vrInterpolate = true;
        logf("options: vrLockToHeadset = 1 turns vrInterpolate on -- the lock declines every "
             "frame without it whenever the display period is not a whole number of sim ticks "
             "(144 Hz into 90 Hz is 1.6), and the game would free-run instead. It costs one sim "
             "tick of latency. Set vrLockToHeadset = 0 to race unlocked without it.");
    }
    g_options.vrInterpolateKey = GetPrivateProfileIntA("gplvr", "vrInterpolateKey", '7', path);
    g_options.vrFixEyeSegment = GetPrivateProfileIntA("gplvr", "vrFixEyeSegment", 1, path) != 0;
    g_options.simRing = GetPrivateProfileIntA("gplvr", "simRing", 0, path);
    if (g_options.simRing < 0 || g_options.simRing > 2) g_options.simRing = 0;
    GetPrivateProfileStringA("gplvr", "vrRenderScale", "1.0", text, sizeof(text), path);
    g_options.vrRenderScale = static_cast<float>(atof(text));

    // Flat mode is the vanilla game in a borderless window: the window itself, the
    // letterbox and widescreen fixes, front-end scaling, roglv2's glFinish drop and swap
    // interval, and mouselook. Stereo and the tools built to develop and measure the
    // headset path -- the eye dumps, the stage and polygon profilers -- are for vr only. Switching them off here keeps every installer
    // and key that reads them inert, without a second check at each one.
    if (!g_options.vr) {
        g_options.stereo = false;
        g_options.stereoKey = 0;
        g_options.dumpEyesKey = 0;
        g_options.vrMirrorTweakKey = 0;
        g_options.vrMirrorModeKey = 0;
        g_options.profilePolys = false;
        // profileWorkWait survives: the work/wait split is the one measurement that has to
        // read the same frame in both modes, and it owns no GPU marks, so it does not
        // collide with profilePost.
        g_options.profileStages = false;
        g_options.vrLockToHeadset = false;
        g_options.vrInterpolate = false;
        g_options.vrInterpolateKey = 0;
    }

    logf("options: stereo=%d mirror=%d vrMirrorFovScale=%.2f vrMirrorYaw=%.1f "
         "vrMirrorPitch=%.1f vrMirrorOffscreen=%d stereoKey=0x%02X",
         g_options.stereo, g_options.stereoMirror, g_options.vrMirrorFovScale,
         g_options.vrMirrorYaw, g_options.vrMirrorPitch, g_options.vrMirrorOffscreen ? 1 : 0,
         g_options.stereoKey);
    logf("options: vrMirrorTweakKey=0x%02X vrMirrorTweakAngleSpeed=%.2f "
         "vrMirrorTweakFovSpeed=%.2f vrMirrorModeKey=0x%02X",
         g_options.vrMirrorTweakKey, g_options.vrMirrorTweakAngleSpeed,
         g_options.vrMirrorTweakFovSpeed, g_options.vrMirrorModeKey);
    // The vrMirror* values above are the defaults for any car without a gplvr_cars.ini section.
    mirrorConfigReset();
    g_options.vrOffscreen =GetPrivateProfileIntA("gplvr", "vrOffscreen", 0, path) != 0;
    g_options.vrMonitorEye = GetPrivateProfileIntA("gplvr", "vrMonitorEye", 1, path);
    if (g_options.vrMonitorEye < 0 || g_options.vrMonitorEye > 3)
        g_options.vrMonitorEye = 1;
    g_options.vrTransferFinish =
        GetPrivateProfileIntA("gplvr", "vrTransferFinish", 1, path) != 0;
    g_options.vrHudPanelRate = GetPrivateProfileIntA("gplvr", "vrHudPanelRate", 72, path);
    if (g_options.vrHudPanelRate < 0) g_options.vrHudPanelRate = 0;
    if (g_options.vrHudPanelRate > 0 && g_options.vrHudPanelRate < 10)
        g_options.vrHudPanelRate = 10;
    logf("options: vrTransferFinish=%d vrHudPanelRate=%d%s", g_options.vrTransferFinish ? 1 : 0,
         g_options.vrHudPanelRate, g_options.vrHudPanelRate == 0 ? " (every frame)" : "/s");
    g_options.vrSkipStampText =
        g_options.vr && GetPrivateProfileIntA("gplvr", "vrSkipStampText", 0, path) != 0;
    logf("options: vrSkipStampText=%d (%s)", g_options.vrSkipStampText ? 1 : 0,
         g_options.vrSkipStampText
             ? "GPL's stamped text on screen-space surfaces the headset cannot see is "
               "dropped"
             : "every stamp is drawn, and the stamped strings are logged");

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

    logf("options: vrMenuScale=%.2f vrGamma=%.2f vrHudDepth=%.2f vrRenderScale=%.2f "
         "vrRecentreKey=0x%02X vrOffscreen=%d vrMonitorEye=%d",
         g_options.vrMenuScale, g_options.vrGamma, g_options.vrHudDepth,
         g_options.vrRenderScale,
         g_options.vrRecentreKey,
         g_options.vrOffscreen, g_options.vrMonitorEye);
    logf("options: vrExtraRenders=%d vrHorizonLock=%.2f vrWorldScale=%.2f",
         g_options.vrExtraRenders, g_options.vrHorizonLock, g_options.vrWorldScale);
    logf("options: vrHudLayer=%d vrHudTextScale=%.2f vrHudBackdropOpacity=%.2f vrHudEye=%d "
         "vrPredictPosition=%d",
         g_options.vrHudLayer, g_options.vrHudTextScale, g_options.vrHudBackdropOpacity,
         g_options.vrHudEye, g_options.vrPredictPosition ? 1 : 0);
    logf("options: vrLockToHeadset=%d vrPaceMarginMs=%.1f (%s)", g_options.vrLockToHeadset ? 1 : 0,
         g_options.vrPaceMarginMs,
         g_options.vrLockToHeadset
             ? "in a tracked race each latch waits to publish this many ms before the "
               "presenter's latch, and the views are located for the display the frame reaches"
             : "frames start as soon as a sim tick arrives, as stock");
    logf("options: vrFixEyeSegment=%d (%s)", g_options.vrFixEyeSegment ? 1 : 0,
         g_options.vrFixEyeSegment
             ? "the world walk's lookup asks about the VIEWED CAR's position instead of the "
               "eye's, which is the point flat GPL asks about, and its answer is checked against "
               "GPL's own containment test before the walk is centred on it"
             : "the world walk's lookup asks about the eye's own position and its answer is "
               "taken as the game gives it");
    logf("options: vrInterpolate=%d vrInterpolateKey=0x%02X (%s)", g_options.vrInterpolate ? 1 : 0,
         g_options.vrInterpolateKey,
         g_options.vrInterpolate
             ? "every car and the view are drawn blended between the two newest latched sim "
               "ticks at the frame's display time, which adds one sim tick of latency (6.94 ms "
               "at 144 Hz); render pacing also engages when the display period is not a whole "
               "number of ticks"
             : "the world is drawn at the newest latched sim tick, as stock");
    logf("options: simRing=%d (%s)", g_options.simRing,
         g_options.simRing >= 2
             ? "the sim thread publishes each tick and the car render records are written from "
               "the ring"
         : g_options.simRing == 1
             ? "the sim thread publishes each tick and the ring is verified against GPL's own "
               "latch; nothing drawn changes"
             : "off: the sim and the render thread exchange state through GPL's latch alone");
    logf("options: profilePost=%d (%s)", g_options.profilePost ? 1 : 0,
         g_options.profilePost ? "post is split into roglv2's flushes and the rest, CPU and GPU"
                               : "post is one number");
    logf("options: profileWorkWait=%d profileStallUs=%d profileTargetHz=%d (%s)",
         g_options.profileWorkWait ? 1 : 0, g_options.profileStallUs, g_options.profileTargetHz,
         g_options.profileWorkWait
             ? "every millisecond of the frame is reported as CPU WORK or CPU WAIT"
             : "no work/wait split -- pre and present stay single numbers");
    logf("options: fastText=%d fastTextKey=0x%02X (%s)", g_options.fastText ? 1 : 0,
         g_options.fastTextKey,
         g_options.fastText
             ? "GPL's stamped glyphs come from our GPU atlas, a run of them in one draw, "
               "instead of one glDrawPixels a glyph"
             : "every glyph is roglv2's glDrawPixels, ~103 us each");
    logf("options: HUD head ahead=%.2f down=%.2f right=%.2f width=%.2f | seat ahead=%.2f "
         "down=%.2f right=%.2f width=%.2f",
         g_options.vrHudHead.ahead, g_options.vrHudHead.down, g_options.vrHudHead.right,
         g_options.vrHudHead.width, g_options.vrHudSeat.ahead, g_options.vrHudSeat.down,
         g_options.vrHudSeat.right, g_options.vrHudSeat.width);
    logf("options: vrHudLayerKey=0x%02X vrHudTweakKey=0x%02X vrHudTweakSpeed=%.3f",
         g_options.vrHudLayerKey, g_options.vrHudTweakKey, g_options.vrHudTweakSpeed);
    logf("options: vrMouseLook=%d sensitivity=%.3f invertPitch=%d key=0x%02X",
         g_options.vrMouseLook ? 1 : 0, g_options.vrMouseLookSensitivity,
         g_options.vrMouseLookInvertPitch ? 1 : 0, g_options.vrMouseLookKey);
    logf("options: vr=%d (from %s)", g_options.vr, path);
    logf("options: window=%dx%d at %d,%d menuFilter=%d enableLetterbox=%d "
         "widescreenFix=%d renderWhenUnfocused=%d%s",
         g_options.windowWidth, g_options.windowHeight, g_options.windowX,
         g_options.windowY, g_options.menuFilter, g_options.enableLetterbox ? 1 : 0,
         g_options.widescreenFix ? 1 : 0, g_options.renderWhenUnfocused ? 1 : 0,
         (!g_options.renderWhenUnfocused && g_options.vr) ? " (forced on by vr)" : "");

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
    // The sceneRender call sites are patched in both modes: that hook is where mouselook
    // and the widescreen field of view reach the camera on the flat path. Stereo, the
    // off-screen target, the task-rate probe and the eye-frustum hook on cameraBind are
    // the headset's.
    if (g_options.vr) {
        stereoBind(binding);
        renderTargetBind(binding);
        taskRateBind(&binding.image);
    }

    if (binding.sceneRenderCallSites.empty()) {
        logf("requestSceneRenderHook: nothing to patch");
        return;
    }
    g_pendingCallSites = binding.sceneRenderCallSites;
    g_originalSceneRender = reinterpret_cast<SceneRenderFn>(binding.sceneRender);

    // Without these the eyes render the game's field of view rather than the headset's,
    // so say plainly which of the two is about to happen.
    g_originalCameraBind = reinterpret_cast<CameraBindFn>(binding.cameraBind);
    if (g_options.vr) {
        g_pendingBindCallSites = binding.cameraBindCallSitesInScene;
        if (g_pendingBindCallSites.empty())
            logf("requestSceneRenderHook: no cameraBind call sites -- the eye frustum fix "
                 "is OFF and the eyes will use the game's own fov");
    }

    // Per-pane mirrors (VR) need this call redirected so a mirror-only render can skip it. The
    // sceneRender found by discovery must be the one the fixed call site lives in.
    g_mainWalkPending =
        g_options.vr && g_options.stereoMirror > 0 && binding.sceneRender == 0x00487620;

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

void setCursorClip(const RECT& rect) {
    realUser32<decltype(&ClipCursor)>("ClipCursor")(&rect);
}

bool frameAlreadyPublished() { return g_framePublished; }

bool hooksMainWalkRedirected() { return g_mainWalkRedirected; }

void* hooksLastSceneParams() { return g_lastSceneParams; }

bool keepDrawingWhileUnfocused() {
    return g_options.renderWhenUnfocused || g_options.vr;
}

void releaseCursorClip() { realUser32<decltype(&ClipCursor)>("ClipCursor")(nullptr); }

bool patchModuleImport(HMODULE module, const char* name, void* replacement,
                       void** original) {
    if (module == g_selfModule) return false;

    ModuleImage image;
    if (!getModuleImage(module, image)) return false;

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;

    auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        image.base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            image.base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                         : descriptor->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            const_cast<uint8_t*>(image.base) + descriptor->FirstThunk);

        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                image.base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), name) != 0)
                continue;

            auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement));
            if (slots->u1.Function == value) return true;  // already ours

            *original = reinterpret_cast<void*>(
                static_cast<uintptr_t>(slots->u1.Function));
            if (!patchBytes(&slots->u1.Function, &value, sizeof(value))) return false;

            char file[MAX_PATH] = {};
            GetModuleFileNameA(module, file, MAX_PATH);
            const char* leaf = strrchr(file, '\\');
            logf("IAT: %s imports %s -> hooked", leaf ? leaf + 1 : file, name);
            return true;
        }
    }
    return false;
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
