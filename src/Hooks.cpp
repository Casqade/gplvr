#include "Hooks.h"

#include "fast_text/FastText.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/TrackBake.h"
#include "DataRebase.h"
#include "DrawnMarks.h"
#include "FlatFrames.h"
#include "FlatTarget.h"
#include "FrameBudget.h"
#include "FrameTrace.h"
#include "GlTrace.h"
#include "Hud.h"
#include "Interpolation.h"
#include "Letterbox.h"
#include "MenuScale.h"
#include "MirrorCache.h"
#include "MirrorConfig.h"
#include "MirrorTarget.h"
#include "MirrorUv.h"
#include "MouseLook.h"
#include "MirrorDriverArms.h"
#include "NoSignal.h"
#include "Log.h"
#include "Paths.h"
#include "PostProfile.h"
#include "Pribluda.h"
#include "QuickMenu.h"
#include "ReplayQuickMenu.h"
#include "RenderPacing.h"
#include "RenderProfile.h"
#include "ReplayPanel.h"
#include "RenderTarget.h"
#include "Stereo.h"
#include "SwapControl.h"
#include "TaskRate.h"
#include "TweakApi.h"
#include "VrCamera.h"
#include "VrInput.h"
#include "VrMenu.h"
#include "Window.h"
#include "XrRuntimeProbe.h"
#include "GameExit.h"
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
bool      g_vrStarted = false;
bool      g_vrRecentreKeyLast = false;
bool      g_vrHudLayerKeyLast = false;
bool      g_vrMirrorModeKeyLast = false;
bool      g_vrMouseLookKeyLast = false;
bool      g_loggedMouseLook = false;

const char* mirrorModeText(int mode) {
    switch (mode) {
        case kMirrorShared:
            return "one pane every glass shares, rendered from each eye: depth, 2 mirror renders a "
                   "frame";
        case kMirrorSharedHeadCentre:
            return "one pane every glass shares, rendered once from the head centre: no depth, 1 "
                   "mirror render a frame";
        default:
            return "a pane per glass, rendered from each eye: depth, 2 mirror renders a frame per "
                   "glass";
    }
}

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
// Frames published as a stereo pair while nothing was rendered into them. Bounded, because
// the condition can hold for every frame of a long stall and the point is to name it once.
int       g_heldPairReports = 4;

// GPL's "no signal" static (NoSignal.h): one episode is the run of frames it fills, from the
// first to the next frame it does not.
struct NoSignalEpisode {
    ULONGLONG since = 0;     // GetTickCount64 at the episode's first frame, 0 between episodes
    bool      replay = false;  // the replay screen was up when it began
    int       held = 0;      // frames not published, so the headset kept its last one
    int       flat = 0;      // frames published as one flat image for both eyes
    int       unplaced = 0;  // frames whose static was on a surface no rect is known for: held
    int       withWorld = 0;  // frames that also rendered the stereo world, counted in the above
    int       leftOver = 0;  // frames GPL drew nothing on after its static; in the above
    void*     surface = nullptr;      // the surface the last of them was drawn on
    int       size[2] = {};           // its own size
    const char* where = nullptr;      // which part of the frame that is, null if unknown
    int       rect[4] = {};  // that part, all zero for the whole frame
};
NoSignalEpisode g_noSignal;
int             g_noSignalReports = 10;
// ONE HOLD A SPELL, NOT ONE AN EPISODE. Upside down, GPL draws its static in bursts of 140-420 ms
// with 300-450 ms of world between them (2026-10-18). Each burst is an episode of its own, so a
// hold per episode kept the headset frozen for most of the spell and showed the noise only for
// the tail of the rare burst that outlasted it. A race episode that begins within
// kNoSignalRejoinMs of the last static frame continues the spell, and the hold is measured from
// the spell's start.
constexpr ULONGLONG kNoSignalRejoinMs = 500;
ULONGLONG       g_noSignalSpellSince = 0;
ULONGLONG       g_noSignalLastDrawnMs = 0;
// One-frame holds in a race, summed rather than logged one by one: FlatFrames' early release
// for a BG request causes them ten a second at a session's start, and they spent the whole
// report budget in one run. Summarised at the end of an episode once kNoSignalBlipMs has passed.
constexpr ULONGLONG kNoSignalBlipMs = 10000;
int             g_noSignalBlips = 0;
ULONGLONG       g_noSignalBlipsSince = 0;
// Whether the presenter has been handed any frame yet, so there is one for a hold to keep.
bool            g_publishedAny = false;

// Where this frame's static is, into g_noSignal: the surface of the camera it was drawn through.
// The 3D view's surface is its rect at the frame's top left; an eye's surface is that eye's rect;
// the surface the eyes were carved out of is the whole frame; any other surface roglv2 draws
// through the framebuffer is its own rect. Anything else is a place the flat image cannot be taken
// from, and `where` stays null.
void placeNoSignalStatic() {
    void*       surface = noSignalSurface();
    int         rect[4] = {};
    const char* where = nullptr;
    if (windowViewSurface(surface, rect)) where = "the 3D view's rect";
    for (int eye = 0; eye < 2 && !where; ++eye)
        if (surface && surface == stereoEyeSurface(eye) && stereoEyeRect(eye, rect))
            where = eye ? "the right eye's rect" : "the left eye's rect";
    if (!where && surface && surface == stereoSourceSurface()) where = "the whole frame";
    // A framebuffer surface's pixels sit at its origin in the framebuffer bound for the frame,
    // which is the one the presenter copies. The replay screen and the race both draw the static
    // through a camera of their own, on a window-sized surface at 0,0 (handles 6D and C5,
    // 2026-10-16/17), which the three tests above never match.
    int  origin[2] = {};
    int  extent[2] = {};
    bool framebuffer = false;
    if (!where && surface &&
        fastTextSurfaceInfo(static_cast<int>(reinterpret_cast<intptr_t>(surface)), origin[0],
                            origin[1], extent[0], extent[1], framebuffer) &&
        framebuffer && extent[0] > 0 && extent[1] > 0) {
        rect[0] = origin[0];
        rect[1] = origin[1];
        rect[2] = extent[0];
        rect[3] = extent[1];
        where = "a framebuffer surface, at";
    }
    int size[2] = {};
    if (surface) stereoSurfaceSize(surface, size[0], size[1]);
    g_noSignal.surface = surface;
    g_noSignal.size[0] = size[0];
    g_noSignal.size[1] = size[1];
    g_noSignal.where = where;
    for (int i = 0; i < 4; ++i) g_noSignal.rect[i] = rect[i];
    frameTraceStatic(noSignalCamera(), surface, size[0], size[1],
                     where ? where : "not the 3D view, an eye, the frame or a framebuffer surface");
}

bool      g_warnedSlowTick = false;
bool      g_loggedMirrorPath = false;
// How many scene renders this second actually REFRESHED the mirror texture.
//
// This is the measurement that settles why the mirror rate varies. gpl_simFrameRender
// allocates the mirror's bitmap instance, renders, copies the corner into mirror.mip,
// then FREES the instance and zeroes p[43] before returning. Our extra renders run after
// that, through a pointer into the same live params block -- so p[43] is already null and
// they skip the mirror entirely. Predicted consequence: mirror refreshes track the GAME
// frame rate while the headset sees the extra frames too, so the ratio is set by how
// much idle time the current scene leaves. If `mirror N/s` tracks worldFrames and not fps,
// that is confirmed; if it is far below worldFrames, something else is throttling it and
// the extra renders are not the story.
unsigned  g_mirrorRefreshes = 0;
LONGLONG  g_extraRenderCost = 0;

// Why the extra frames of frameRate = 2 did not happen, counted for one report window.
//
// Every gate in renderExtraFrames is a silent `return`, and four of them can fire for reasons
// that look identical from outside: the frame rate simply stays at the sim's. Two rounds were
// spent guessing which, so each now has a counter and the `extra:` line names it.
struct ExtraFrameCounts {
    unsigned ticks = 0;        // frames that reached the budget
    unsigned rendered = 0;     // extra frames actually drawn and published
    unsigned notStereo = 0;    // no tracked stereo world frame to repeat
    unsigned slow = 0;         // the game is slower than 25 fps
    unsigned late = 0;         // this frame already overran the sim tick
    unsigned noDeadline = 0;   // the tick clock had no reading, so the old estimate was used
    unsigned noPose = 0;       // the presenter published no newer view set before the deadline
    unsigned noRoom = 0;       // a render would not have finished before the next tick
    LONGLONG roomShort = 0;    // how much it was short by, summed, for the worst case below
    LONGLONG roomWorst = 0;
};
ExtraFrameCounts g_extraFrames;

std::vector<uintptr_t> g_pendingCallSites;
volatile bool          g_sceneHookPending = false;

typedef void(__cdecl* CameraBindFn)(void*);
CameraBindFn           g_originalCameraBind = nullptr;
std::vector<uintptr_t> g_pendingBindCallSites;

// gpl_sceneRender's MAIN world walk: `call gpl_worldRenderSegmentRange` at 0x487FAA, eight
// __cdecl arguments cleaned by the caller (`add esp, 20h`). The mirror walk at 0x487A4C calls
// the same function; it is redirected only with the baked renderer, by baked_gpl/BakedMirror.cpp.
// Its result is never read.
typedef void(__cdecl* WorldWalkFn)(int, int, int, int, int, int, int, int);
constexpr uintptr_t     kMainWalkSite = 0x00487FAA;
constexpr unsigned char kMainWalkStock[5] = {0xE8, 0xB1, 0xD9, 0xFF, 0xFF};
constexpr uintptr_t     kWorldWalk = 0x00485960;
bool g_mainWalkPending = false;
bool g_mainWalkRedirected = false;

// gpl_mirrorDistanceScale. gpl_sceneRender scales the two draw-distance globals by this around
// the mirror walk and restores them after (0x487A12 and 0x487A62), so it sets how far back the
// mirror sees. At the stock 0.25 the mirror reaches about 78 m ahead and 29 m behind, where the
// eye reaches 312 and 117.
//
// A modded track builds its horizon as ordinary finite scenery instead of the background
// primitive vanilla uses, and that mesh lies outside the mirror's quartered reach -- which is
// the white sky above about 13 degrees of glass pitch. 1.0 gives the mirror the eye's own reach
// and was confirmed to fix it on spa67.
//
// Back at GPL's own 0.25 for now, because the cost has never been measured: the mirror passes
// were only 0.25 ms each BECAUSE the window was quartered, and the session that proved the fix
// ran with profileStages off. This is where an ini key belongs -- read it into
// kMirrorDistanceScaleWanted and the rest of this works unchanged.
// A reference-link address, placed on the running image through DataRebase before it is written.
constexpr uintptr_t kMirrorDistanceScale = 0x0076E768;
constexpr float     kMirrorDistanceScaleWanted = 0.25f;
bool                g_mirrorDistanceScaleSaid = false;

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
// THE EIGHT ARGUMENTS ARE FORWARDED UNCHANGED, and the decode is worth keeping even though
// nothing here uses it any more. From the push order at 0x487F7D..0x487FAA: a1 is the segment
// table, a2 and a3 the camera dlong twice (a3 from a second camera below the fov threshold at
// 0x487E71), a4 the heading as a binary angle whose magnitude picks the segment order at
// 0x40000000 and the distance columns at 0x20000000 and 0x60000000, a5 and a6 two table
// columns multiplied by flt_7743E8, a7 and a8 two columns used raw. a5 is read only past 135
// degrees. Repeatable comment at 0x487F8A.
//
// TWO KNOBS THAT SCALED THE DISTANCES AND PINNED THE HEADING LIVED HERE and were removed on
// 2026-10-05, once `fixEyeSegment` turned out to be the whole of the artifact. Scaling the
// distances did move the strip's far edge, which proved the 1-D window is real, but the fault
// was never the range -- see STATUS.md. Both are two lines if either is ever wanted again.
void __cdecl mainWorldWalkThunk(int a1, int a2, int a3, int a4, int a5, int a6, int a7,
                                int a8) {
    if (stereoMirrorOnlyPass()) return;
    trackBakeMainWalk(g_lastSceneParams);
    bakedPassBegin(g_lastSceneParams);
    reinterpret_cast<WorldWalkFn>(kWorldWalk)(a1, a2, a3, a4, a5, a6, a7, a8);
    // Always: it also ends the capture probe's watch of a pass GPL drew alone.
    bakedPassEnd(g_lastSceneParams);
    // Nothing of the pass is submitted after the walk (0x487FAF-0x487FEE: pops, resets, the stats
    // overlay), so the cockpit goes over the eye here (report 105 section 4.49).
    bakedPassMainWalkDone();
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
    //
    // `+N restored` is the frames that got their reflection from the per-eye copy instead of
    // rendering it. It is the pair of counters that says whether mirrorUpdateInterval is doing
    // anything: a restore count of zero with the option above 1 means the copies never built,
    // and the `mirror:` line above will say why.
    // `copy-outs C rebound R`: the copy-out rebind's copies (kMirrorCopyRebind), and those that left mirror.mip bound
    // where roglv2's cache did not know it.
    char     mirror[128] = {};
    unsigned restored = mirrorCacheTakeRestores();
    unsigned copies = 0;
    unsigned rebound = 0;
    mirrorCacheTakeCopies(copies, rebound);
    if (g_mirrorRefreshes && restored)
        _snprintf_s(mirror, sizeof(mirror), _TRUNCATE,
                    "mirror %.1f/s +%.1f restored, copy-outs %u rebound %u | ",
                    g_mirrorRefreshes / seconds, restored / seconds, copies, rebound);
    else if (g_mirrorRefreshes)
        _snprintf_s(mirror, sizeof(mirror), _TRUNCATE, "mirror %.1f/s, copy-outs %u rebound %u | ",
                    g_mirrorRefreshes / seconds, copies, rebound);
    // Panes rendered and panes whose glass no eye could see (report 105 section 4.45).
    unsigned panesRendered = 0;
    unsigned panesOutOfView = 0;
    stereoTakePaneCounts(panesRendered, panesOutOfView);
    char panes[96] = {};
    if (panesRendered || panesOutOfView)
        _snprintf_s(panes, sizeof(panes), _TRUNCATE,
                    "mirror panes %u, out of view %u (skip out of view %d) | ", panesRendered,
                    panesOutOfView, kMirrorSkipOutOfView ? 1 : 0);

    // Mirror-only renders are not counted as renders, but their time is in the scene total
    // and so in "ms each": per eye render including the pane renders before it.
    char paneRenders[48] = {};
    if (g_sceneMirrorOnlyCalls)
        _snprintf_s(paneRenders, sizeof(paneRenders), _TRUNCATE, " + %.1f/s mirror-only",
                    g_sceneMirrorOnlyCalls / seconds);

    // What the HEADSET is given, next to what the game runs at, because they are not the same
    // number and "fps" alone has been read as the headset's more than once. `fps` counts game
    // frames -- one per sim tick, whatever else happens -- while every published frame,
    // including the extra ones, takes a transfer.
    char headsetRate[48] = {};
    if (g_options.vr && haveTransfer)
        _snprintf_s(headsetRate, sizeof(headsetRate), _TRUNCATE, " (headset %.1f/s)",
                    transfers / seconds);

    logf("fps %.1f%s | worldFrames %.1f/s, renders %.1f/s (x%.2f)%s, %.2f ms each, "
         "%.2f ms worst | transfer %.2f ms x%.1f/frame | %s%s%s%smode %s",
         g_frames / seconds, headsetRate, g_sceneHookCalls / seconds, g_sceneOrigCalls / seconds,
         g_sceneHookCalls ? static_cast<double>(g_sceneOrigCalls) / g_sceneHookCalls : 0.0,
         paneRenders, msPerRender, worstMs,
         haveTransfer && transfers ? transferMs / transfers : 0.0,
         haveTransfer && g_frames ? static_cast<double>(transfers) / g_frames : 0.0,
         mirror, panes, haveTick ? tick : "", haveTick ? " | " : "",
         g_options.vr ? "stereo" : "single");

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
    replayPanelReport();

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

    // Render pacing, once a second while tracked frames were rendered: at any frameRate other
    // than 1 it is the unpaced baseline for the same counters.
    char pacing[640] = {};
    if (renderPacingFormat(pacing, sizeof(pacing))) logf("%s", pacing);

    // The fallback path's extra frames, and -- when there are none -- which gate stopped them.
    // Every one of those gates is a silent return, and they all look the same from outside: the
    // rate stays at the sim's. Suppressed when the loop's own wait is ours, where the `frames:`
    // line below is the account and this one would print a row of zeroes that reads as failure.
    if (g_options.vr && g_options.frameRate > 0 && !flatFramesActive()) {
        ExtraFrameCounts& e = g_extraFrames;
        LARGE_INTEGER     frequency;
        QueryPerformanceFrequency(&frequency);
        double toMsShort = e.noRoom ? 1000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
        logf("extra: %u drawn from %u game frames%s | refused: no room %u (short by %.1f ms "
             "mean, %.1f worst), no newer pose %u, frame already late %u, game under 25 fps "
             "%u, no tracked stereo frame %u, no tick clock %u | one costs %.2f ms",
             e.rendered, e.ticks, e.ticks ? "" : " -- NONE REACHED THE BUDGET", e.noRoom,
             e.noRoom ? toMsShort * static_cast<double>(e.roomShort) / e.noRoom : 0.0,
             toMsShort * static_cast<double>(e.roomWorst), e.noPose, e.late, e.slow,
             e.notStereo, e.noDeadline,
             1000.0 * static_cast<double>(g_extraRenderCost) /
                 static_cast<double>(frequency.QuadPart));
        g_extraFrames = ExtraFrameCounts{};
    }

    // World interpolation, once a second while world frames were latched.
    char interp[1280] = {};
    if (interpolationFormat(interp, sizeof(interp))) logf("%s", interp);

    // Frames drawn between the simulation's ticks, in either mode. After the interp line, which
    // says what the blend did with them.
    char flat[384] = {};
    if (flatFramesFormat(flat, sizeof(flat), seconds)) logf("%s", flat);

    // The drawn-mark list's peak and overflows, once a second while the world was drawn. Per
    // frame, so the clears read against the eye and mirror-only renders on the fps line.
    char marks[200] = {};
    if (drawnMarksFormat(marks, sizeof(marks), g_frames)) logf("%s", marks);

    // The controllers and the menu. Both print only when something happened, so a session
    // with no headset adds nothing to the log -- and both print their REFUSALS, because a
    // controller that is not reaching the game and a controller nobody is touching look
    // exactly alike from the inside.
    char input[320] = {};
    if (vrInputFormatStatus(input, sizeof(input))) logf("%s", input);
    char menu[320] = {};
    if (vrMenuFormatStatus(menu, sizeof(menu))) logf("%s", menu);
    char quick[640] = {};
    if (quickMenuFormatStatus(quick, sizeof(quick))) logf("%s", quick);
    char replay[1000] = {};
    if (replayQuickMenuFormatStatus(replay, sizeof(replay))) logf("%s", replay);

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

// Mirror tweak mode: aiming the glasses from the headset. It changes MirrorConfig's per-glass
// aims, which aimPane reads every frame, so each step shows at once. Leaving saves them to the
// viewed car's section of gplvr_mirrors.ini; gplvr.ini is never written. Home steps through the
// glasses -- all of them together, then each in turn. It shares the arrows and Page Up/Down with
// HUD tweak mode, so entering either leaves the other.
constexpr unsigned kMirrorTweakKeys[] = {VK_LEFT, VK_RIGHT, VK_UP,  VK_DOWN,
                                         VK_PRIOR, VK_NEXT, VK_HOME};
constexpr double   kMirrorTweakMaxStepSeconds = 0.1;  // the most one poll turns after a stall
constexpr double   kMirrorTweakLogSeconds = 0.5;      // between progress lines while adjusting

struct MirrorTweak {
    bool     active = false;
    bool     keyLast = false;
    bool     selectLast = false;  // Home, which steps once per press rather than per poll
    bool     unlogged = false;  // the values changed since the last line
    LONGLONG frequency = 0;
    LONGLONG lastPoll = 0;
    LONGLONG loggedAt = 0;
};

MirrorTweak g_mirrorTweak;

bool mirrorTweakKeyHeld(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

void mirrorTweakLogProgress(LONGLONG now) {
    g_mirrorTweak.unlogged = false;
    g_mirrorTweak.loggedAt = now;
    char aims[512];
    mirrorConfigFormatAims(aims, sizeof(aims));
    logf("mirror: %s", aims);
}

void mirrorTweakEnter() {
    // The headset's own mirror mode edits the same three numbers through the same API.
    // Two owners of one aim is how a value ends up somewhere neither of them put it.
    if (vrMenuActive()) {
        logf("mirror: tweak mode not entered -- the VR menu has the mirror; close it first");
        return;
    }
    pribludaTweakExit(" -- mirror tweak mode entered");
    g_mirrorTweak.active = true;
    g_mirrorTweak.unlogged = false;
    const char* key = mirrorConfigKey();
    char        aims[512];
    mirrorConfigFormatAims(aims, sizeof(aims));
    logf("mirror: tweak mode on for %s (%s), aiming %s -- %s", key ? key : "no identified car",
         mirrorConfigPerCar() ? "per-car" : "no section", mirrorConfigSelectedName(), aims);
    logf("mirror: Left/Right turn the glasses, Up/Down tilt them, Page Up/Down widen and "
         "narrow the field, Home picks a glass (all, then each in turn); key 0x%02X again leaves",
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
            // The key IS the save. Nothing else writes.
            mirrorTweakExit("", true);
        } else {
            mirrorTweakEnter();
        }
        return;
    }
    if (!g_mirrorTweak.active) return;

    bool select = mirrorTweakKeyHeld(VK_HOME);
    bool selectPressed = select && !g_mirrorTweak.selectLast;
    g_mirrorTweak.selectLast = select;
    if (selectPressed) {
        TweakCommand step;
        step.target = TweakTarget::Mirror;
        step.action = TweakAction::SelectNext;
        if (tweakApply(step)) mirrorTweakLogProgress(now);
    }

    int turn = (mirrorTweakKeyHeld(VK_RIGHT) ? 1 : 0) - (mirrorTweakKeyHeld(VK_LEFT) ? 1 : 0);
    int tilt = (mirrorTweakKeyHeld(VK_UP) ? 1 : 0) - (mirrorTweakKeyHeld(VK_DOWN) ? 1 : 0);
    int widen = (mirrorTweakKeyHeld(VK_PRIOR) ? 1 : 0) - (mirrorTweakKeyHeld(VK_NEXT) ? 1 : 0);
    if (!turn && !tilt && !widen) {
        // The end of a hold, which the rate limit may have kept from the log.
        if (g_mirrorTweak.unlogged) mirrorTweakLogProgress(now);
        return;
    }

    // Through the SAME semantic commands the controller produces. An arrow key held down
    // is a rate axis at full deflection, which is exactly what the keys always were --
    // the clamps, the step sizes and the multiplicative field are TweakApi's now, so
    // there is one copy of them and the two input paths cannot drift apart.
    TweakCommand command;
    command.target = TweakTarget::Mirror;
    command.mode = TweakMode::Rate;
    command.seconds = static_cast<float>(seconds);

    bool changed = false;
    if (turn) {
        command.axis = TweakAxis::Yaw;
        command.value = static_cast<float>(turn);
        changed = tweakApply(command) || changed;
    }
    if (tilt) {
        command.axis = TweakAxis::Pitch;
        command.value = static_cast<float>(tilt);
        changed = tweakApply(command) || changed;
    }
    if (widen) {
        command.axis = TweakAxis::Fov;
        command.value = static_cast<float>(widen);
        changed = tweakApply(command) || changed;
    }
    if (changed) g_mirrorTweak.unlogged = true;

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

bool mirrorTweakActive() { return g_mirrorTweak.active; }

bool hotkeyOwnedByMode(unsigned virtualKey) {
    if (pribludaTweakSwallowsKey(virtualKey)) return true;
    if (!g_mirrorTweak.active) return false;
    for (unsigned key : kMirrorTweakKeys) {
        if (key == virtualKey) return true;
    }
    return false;
}

void mirrorTweakExit(const char* why, bool save) {
    if (!g_mirrorTweak.active) return;
    g_mirrorTweak.active = false;
    g_mirrorTweak.unlogged = false;
    char aims[512];
    mirrorConfigFormatAims(aims, sizeof(aims));
    logf("mirror: tweak mode off -- %s%s", aims, why);
    // Leaving without saving writes nothing and reverts nothing: the aim stays where it was put
    // until it is saved or the game exits, which is how the pribluda layout and the VR menu's
    // settings already behave. Changing the viewed car or the car set takes an unsaved aim away
    // (MirrorConfig), because nothing may ever write a car that is not in view.
    if (save)
        mirrorConfigSave();
    else
        logf("mirror: NOT saved -- the aim stays as you left it; press the key in tweak mode to "
             "write it to gplvr_mirrors.ini");
}

namespace {

void pollToggle() {
    // Recentre: whatever pose the driver is in becomes the seat. Needed because
    // OpenXR's LOCAL space has its origin on the floor, not at the head -- measured,
    // see STATUS.md -- so without this the view starts a standing height too high.
    if (g_options.vr && g_options.vrRecentreKey) {
        const bool down = (GetAsyncKeyState(g_options.vrRecentreKey) & 0x8000) != 0;
        if (down && !g_vrRecentreKeyLast && !hotkeyOwnedByMode(g_options.vrRecentreKey))
            vrRequestRecentre();
        g_vrRecentreKeyLast = down;
    }

    // Pribluda's panel between the head and the seat. Tweak mode is left as it is, so an
    // element being placed can be looked at both ways. A seat panel with no seat published
    // would be invisible, so one is published on the spot.
    if (g_options.vr && g_options.vrHudLayerKey) {
        bool down = (GetAsyncKeyState(g_options.vrHudLayerKey) & 0x8000) != 0;
        if (down && !g_vrHudLayerKeyLast && !hotkeyOwnedByMode(g_options.vrHudLayerKey)) {
            g_options.vrHudMode = hudLayer() == 1 ? 1 : 2;
            const char* seat = "";
            if (hudLayer() == 2 && !presenterSeatPublished()) {
                seat = vrPublishSeat()
                           ? " -- no seat had been published, so the head pose now is the seat"
                           : " -- no seat yet and no head pose to take one from; it appears "
                             "after the first recentre";
            }
            logf("vr: HUD panel switched to %s%s",
                 hudLayer() == 1 ? "follow the head" : "the seat", seat);
        }
        g_vrHudLayerKeyLast = down;
    }

    // The controllers, before anything that reacts to them. One read of the shared block
    // a frame, and everything below sees the same sample -- a second reader would get a
    // newer one halfway through the poll and the two would disagree about which buttons
    // were pressed this frame.
    vrInputPoll();

    pribludaPollTweak();
    mirrorTweakPoll();

    // After the keyboard modes, so a mode entered by key this frame already holds its
    // target and the menu's own entry refuses rather than taking it away.
    vrMenuPoll();
    // After the VR menu, so a grip tap that opened it this poll is already seen and the radial
    // stays shut; and on this, GPL's fg thread, which is where its in-car posts belong.
    quickMenuPoll();
    // The replay's stick and radial: after the VR menu for the same reason, and after the
    // replay pointer, which has already clicked if this press was on GPL's panel. Its posts
    // belong on the fg thread too.
    replayQuickMenuPoll();
    // A close the window proc took (Alt+F4, the window's Close), carried out here on the fg thread
    // in GPL's order: out of the car, then the main menu's Exit. Either mode.
    gameExitPoll();

    // The next mirror mode. The pass loop reads mirrorMode every frame and the main world walk
    // redirect is installed whichever way it started, so the modes swap freely.
    if (g_options.vr && g_options.vrMirrorModeKey) {
        bool down = (GetAsyncKeyState(g_options.vrMirrorModeKey) & 0x8000) != 0;
        if (down && !g_vrMirrorModeKeyLast && !hotkeyOwnedByMode(g_options.vrMirrorModeKey)) {
            g_options.mirrorMode = (g_options.mirrorMode + 1) % 3;
            logf("mirror: mode %d -- %s", g_options.mirrorMode,
                 mirrorModeText(g_options.mirrorMode));
        }
        g_vrMirrorModeKeyLast = down;
    }


    // THE TOP OF A FRAME, BEFORE ANYTHING ASKS FOR A SURFACE. The session that owned gplvr's
    // cached rasterizer handles has ended, and a handle outliving its session is what killed
    // the process on 2026-10-09 -- `roglv2!bitmapWidth+0x27`, handle 0x6F, resolved through a
    // table slot the teardown had zeroed. Dropped here rather than in the present hook because
    // the crashing path was INSIDE the present hook: MenuScale's rasterFlushPending synthesises
    // a 2D call every front-end frame, and by then it is too late to find out the handles are
    // dead.
    if (windowTakeFrontEndEntered()) stereoForgetSurfaces("the session is over");

    fastTextPollFrame();
    postProfilePollFrame();

    // The mouse look toggle, and the mouse itself.
    //
    // Deliberately NOT gated on g_options.vr: the whole reason this exists is to drive
    // the camera with vr = 0, on a flat monitor, where a straight line is straight and
    // the headset cannot be blamed for anything.
    if (g_options.vrMouseLookKey) {
        const bool down = (GetAsyncKeyState(g_options.vrMouseLookKey) & 0x8000) != 0;
        if (down && !g_vrMouseLookKeyLast && !hotkeyOwnedByMode(g_options.vrMouseLookKey)) {
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
        // Drained on every screen, so the mouse moved in the menus does not turn the view on
        // the way into the car.
        if (mouseLookTake(deltaX, deltaY) && vrLookOverrideActive()) {
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
    if (!g_options.vr || g_options.frameRate <= 0) return;
    // THE FALLBACK ONLY. When the frame loop's wait is ours every frame is a whole frame of
    // GPL's own, drawn through its loop with the mirror pass and the HUD, and there is nothing
    // for a partial repeat after the present to add. This path exists for the case where those
    // call sites could not be taken.
    if (flatFramesActive()) return;
    if (!g_lastSceneParams || !stereoAvailable()) return;
    if (!g_lastFrameWasStereo || !vrFrustumInjected()) {
        ++g_extraFrames.notStereo;
        return;
    }

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
        ++g_extraFrames.slow;
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
        ++g_extraFrames.late;
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

    // A ceiling, not a setting: the deadline is what actually stops this, and no sim tick has
    // room for more whole frames than this even in principle.
    int extra = 7;

    if (!g_loggedExtraRenders) {
        g_loggedExtraRenders = true;
        // The wording said "spread over a tick" long after the spreading was replaced
        // by locking to the presenter's cadence. A stale log line is a lie told once a
        // session.
        logf("vr: frameRate = 2 -- up to %d extra frames per game frame, each waiting for "
             "the presenter to publish a new pose rather than for a slice of the %.1f ms "
             "tick. The world is re-blended to each one's own display time, so the head and "
             "the world both move at the headset's rate. Watch that 'fps' below does not "
             "fall.",
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
    // The deadline is the point where the game's own next frame is due -- WHEN THE NEXT SIM
    // TICK ARRIVES, not a tick from now.
    //
    // Those are not the same instant, and the difference is most of the budget. This runs
    // after the present, by which point the latch, the two eye renders, the HUD and the
    // transfer have already spent 14 of the tick's 27.8 ms. Measuring from `now` claimed 24 ms
    // of room where 13 existed, so a render that could not possibly finish was started anyway,
    // the game's frame went out 37 ms after the last one, and the late-tick guard below then
    // held the NEXT frame back -- which is why the log showed the holdback several times a
    // second and the interp: line counted four extra frames in a second instead of thirty.
    //
    // The sim tick clock knows when the next one is due. Without it, fall back to the old
    // estimate rather than to no extra frames at all.
    double         nextTick = 0.0;
    bool           haveDue = renderPacingNextTickDue(nextTick);
    const LONGLONG deadline =
        haveDue ? static_cast<LONGLONG>(nextTick) : now.QuadPart + pace - pace / 8;
    if (!haveDue) ++g_extraFrames.noDeadline;
    ++g_extraFrames.ticks;
    if (deadline <= now.QuadPart) {
        ++g_extraFrames.noRoom;
        return;
    }

    for (int done = 0; done < extra; ++done) {
        // What the LAST RENDER used, not what is current now. The game's own frame took its
        // pose at its latch, and the presenter has published one or two sets in the time since
        // -- so the first extra frame of a tick has a newer pose already waiting and should
        // not spend a display period discovering that. After it renders, this moves on with
        // it, and the ones after it wait properly.
        const int32_t before = presenterAcquiredViewSequence();
        if (!presenterViewSequence()) return;

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
        if (!advanced) {
            ++g_extraFrames.noPose;
            return;
        }

        // Do not START a render that cannot finish before the game's next frame.
        //
        // The deadline was only checked while waiting, so a render begun just under it
        // ran straight into the game's own frame -- which is a hitch, and hitches are
        // what these renders exist to remove. Budget on what the last one actually cost,
        // plus a quarter for headroom.
        //
        // It used to double it, which was survivable only because the deadline was an
        // overestimate by about the same factor; against the real one it refuses every
        // frame whose render takes more than half the idle tail, and at 10 ms of render in
        // 13 ms of tail that is all of them.
        LARGE_INTEGER before2;
        QueryPerformanceCounter(&before2);
        LONGLONG needed = g_extraRenderCost + g_extraRenderCost / 4;
        if (before2.QuadPart + needed > deadline) {
            ++g_extraFrames.noRoom;
            LONGLONG shortBy = before2.QuadPart + needed - deadline;
            g_extraFrames.roomShort += shortBy;
            if (shortBy > g_extraFrames.roomWorst) g_extraFrames.roomWorst = shortBy;
            return;
        }

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
        // Move the WORLD to this frame's display time, not just the head.
        //
        // Without it an extra render redraws the same sim instant from a newer pose: head
        // tracking gets smooth and everything outside the cockpit still steps at 36 Hz, which
        // is the whole picture for anyone not turning their head. The records are rewritten
        // from the same two latched ticks the frame's own blend used, at a target one display
        // period further on, exactly as the loop-level path does between the ticks.
        interpolationExtraFrame();

        auto* const extraParams = static_cast<SceneParams*>(g_lastSceneParams);
        void* const extraMirror = extraParams->mirrorCamera;
        extraParams->mirrorCamera = nullptr;
        const bool rendered = stereoRender(extraParams, &callOriginalSceneRender);
        // Put it back: on the direct path the game does NOT rewrite p[43] from scratch on
        // entry to gpl_simFrameRender -- it branches on finding it already set.
        extraParams->mirrorCamera = extraMirror;
        if (rendered) {
            ++g_extraFrames.rendered;
            // Same reasoning as the cursor below, and the HUD needs it more: the extra
            // render has just painted the world over a HUD that was drawn once, in
            // gpl_simFrameRender, before this loop was reached. Without the replay the
            // HUD survives only on frames that had no extra render.
            hudReplay();

            // An extra render redraws the world over the frame the cursor was already
            // put into, so it has to go back in before this one is published. Without
            // this the pointer would appear only on the game's own frames and blink at
            // whatever rate the extra renders happen to be running at. The VR menu needs
            // no such repeat: it is a composition layer, not part of this picture, and
            // the runtime draws it on every display frame regardless.
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
    glTraceRoglWindow(false);
    glTraceStepIfStarted("present hook: entered -- everything GPL drew this frame");
    frameBudgetSwitch(kSpanHousekeeping);

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
    glTraceStepIfStarted("present hook: the front end's scaled blit");

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
    glTraceStepIfStarted("present hook: window, letterbox, swap interval");

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
    //
    // vrHudMode 0 draws no panel (pribludaDrawHud returns first), so the presenter drops
    // the headset's quad. pribluda's own monitor HUD is pribluda's, in every mode.
    frameBudgetSwitch(kSpanPanel);
    pribludaDrawHud();
    glTraceStepIfStarted("present hook: pribluda's HUD panel");

    // The VR menu's layer is built here, with the GL context current and before the frame
    // transfer: it publishes under a lock of its own, and its texture is what the
    // monitor's copy of the panel is drawn from later in this same present.
    //
    // It is NOT drawn into the frame, so it needs no repeat after an extra render the way
    // the cursor and the HUD do -- the runtime composites the layer on every display
    // frame whatever the game's own frames are doing.
    frameBudgetSwitch(kSpanCursor);
    presenterUpdateVrMenu();
    presenterDrawCursor(g_stereoRenderedFrame);
    glTraceStepIfStarted("present hook: the VR menu and the cursor");

    // GPL'S OWN STAMPED TEXT HAS TO BE DRAWN WHILE THE OFF-SCREEN TARGET IS STILL BOUND.
    //
    // In VR the fast text path batches GPL's glyphs as quads and draws them in one
    // go, and its own end-of-frame flush sits far below -- after the headset's copy and after
    // the window blit, which is where it belongs in flat mode and is a frame too late here.
    // Flushed from this side so the pixels are in the image the headset is handed; the call
    // below then has nothing left to draw and keeps doing its bookkeeping.
    fastTextFlushNow();

    // GPL'S STATIC IS ONE QUAD, AND roglv2 HAS NOT DRAWN IT YET. gpl_drivingViewWidgetDraw
    // clears the surface black and submits the noise to roglv2's bins, and only a destination
    // change or a 2D call issues them. With no HUD text after it, the first of those was the
    // flush below the unbind: the quad went to the window, through the target's 6344x3172
    // viewport and off the top of a 2560x1440 buffer, and the eye target kept only the black
    // clear (static probes of 2026-10-18: noise in the target on the one frame with text after
    // it, black on the two without). That black is what a static past vrNoSignalDelay published
    // to both eyes.
    if (noSignalDrawnThisFrame()) rasterFlushPending();

    // rendererType() = 2's mark, while the frame is still bound.
    bakedMarkFrameEnd();
    glTraceStepIfStarted("present hook: fast text flush, static, renderer mark");

    // The off-screen frame has to stop being the draw target before the presenter
    // reads it and before the game presents: glCopyTexSubImage2D reads the READ
    // framebuffer, and SwapBuffers only ever shows the window's own.
    frameBudgetSwitch(kSpanUnbind);
    presenterEndFrame();
    glTraceStepIfStarted("present hook: the off-screen frame unbound");

    frameBudgetSwitch(kSpanPublish);
    // Set by a race frame that had GPL's static in the target; acted on once the target is
    // bound for the next frame, below.
    bool clearStaticFromTarget = false;
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
        // Everything gplvr takes off the framebuffer -- the headset's copy after it -- has to
        // come after this.
        frameBudgetSwitch(kSpanFlushPending);
        rasterFlushPending();
        frameBudgetSwitch(kSpanPublish);

        // WHETHER THE BUFFER HOLDS A STEREO PAIR IS NOT THE SAME QUESTION AS WHETHER THIS
        // FRAME DREW ONE.
        //
        // presenterFrame's first argument decides `sideBySide`, and the consumer reads that
        // to choose between sampling each eye's own rect and treating the whole texture as
        // one mono image. Passing g_stereoRenderedFrame conflated the two: on a frame where
        // GPL never called sceneRender the publish went out mono, so each eye was handed the
        // ENTIRE side-by-side texture and the player saw both eye images side by side in
        // each eye.
        //
        // The buffer is still a side-by-side pair in that situation when nothing has drawn
        // over it. GPL's "no signal" screen (0x423148) is the one case known to: it fills the
        // whole frame with static, and is handled separately below. Otherwise the label follows
        // the buffer, and the pose follows the frame:
        // vrClearRendered() below still reports no head pose, which is the honest answer and
        // is what stops the runtime reprojecting a stale one.
        //
        // g_lastFrameWasStereo is what separates this from a menu, where the buffer really
        // does hold one mono image and must be presented as one. It costs the first frame
        // after leaving a race, which is published as a pair while holding a menu -- the same
        // one-frame cost the extra-render gate accepts for the same reason.
        //
        // THIS IS PLUMBING, NOT A FIX. On its own it holds the last good pair, and a held
        // image reads as a hang rather than as a missing picture. What it buys is that
        // anything drawn per eye on such a frame now ARRIVES per eye -- without it, noise
        // drawn into both eye rects would still show up as two rectangles in each eye.
        //
        // GPL's "no signal" static is the exception. It fills the surface
        // of the camera bound last. On the replay screen that is the 3D view: the window's size,
        // at the top left of the headset-sized frame (the whole-frame dump of 2026-10-15 showed
        // it), over the left eye's previous picture. As a pair that is noise in one eye, which
        // reads as a rendering fault. In a race nothing is published and the headset keeps its
        // last frame, with that frame's own poses, exactly as for a dropped frame; the camera
        // switches that cause it last a frame or three. Once a spell of static has lasted
        // vrNoSignalDelay, the part of the frame the static is on goes out as ONE flat image both
        // eyes share, stretched over the whole eye. On the replay screen it goes out at once, the
        // same way: the camera switch it marks lasts a few frames and is not worth holding.
        //
        // A frame that also rendered the stereo world is no exception: the static is in the
        // buffer all the same, and publishing it as a pair is the fault itself.
        //
        // NOR IS A FRAME THAT DREW NOTHING. Between static frames, and on the frame after a
        // refused car reset, GPL often renders neither the world nor its static, and the buffer
        // still holds the static over the last pair. Published as an ordinary frame that went
        // out as the whole buffer in one image: a frozen two-eye picture with noise in its top
        // left (presenter log, 2026-10-17). Such a frame continues the episode. On the replay
        // screen it is shown as the static's flat rect like the frame that drew it; in a race it
        // is held, because the target was cleared after the static's own frame. Only a rendered
        // world, or leaving the race, ends it.
        // NOT IN THE FRONT END. There the static is the lobby TV's, on its own 373x280 surface,
        // and it belongs to the menu picture like the rest of the lobby. Treated as an episode it
        // was held a frame at a time, which nobody saw -- until spells joined one-frame bursts and
        // the TV's picture went out stretched over both eyes at random frames (2026-10-18).
        bool noSignal = noSignalDrawnThisFrame() && !windowInFrontEnd();
        bool staticLeftOver = !noSignal && g_noSignal.since &&
                              !g_stereoRenderedFrame && !windowInFrontEnd();
        if (noSignal || staticLeftOver) {
            ULONGLONG nowMs = GetTickCount64();
            if (noSignal) {
                if (!g_noSignal.since) {
                    g_noSignal = NoSignalEpisode{};
                    g_noSignal.since = nowMs;
                    g_noSignal.replay = gplReplayScreenShown();
                    bool rejoins = !g_noSignal.replay && g_noSignalLastDrawnMs &&
                                   nowMs - g_noSignalLastDrawnMs < kNoSignalRejoinMs;
                    if (!rejoins) g_noSignalSpellSince = nowMs;
                }
                g_noSignalLastDrawnMs = nowMs;
                if (g_stereoRenderedFrame) ++g_noSignal.withWorld;
                placeNoSignalStatic();
            } else {
                ++g_noSignal.leftOver;
            }

            // In a race the static is cleared out of the target once its frame has gone (see
            // clearStaticFromTarget), so a frame that drew nothing has no static left to publish
            // and holds the last one instead.
            if (!g_noSignal.replay) clearStaticFromTarget = true;
            const int* rect = g_noSignal.rect;
            bool hold = g_publishedAny && !gplReplayScreenShown() &&
                        (nowMs - g_noSignalSpellSince <
                             static_cast<ULONGLONG>(g_options.vrNoSignalDelay) ||
                         (!noSignal && !g_noSignal.replay));
            if (hold) {
                ++g_noSignal.held;
                frameTracePublish(noSignal ? "HELD for the static"
                                           : "HELD, GPL drew nothing after its static",
                                  false, false);
            } else if (g_noSignal.where) {
                ++g_noSignal.flat;
                // The noise fills the eye, stretched: it has no text to keep legible and no shape
                // to keep, and at the menu's size it read as a screen floating in the dark. The
                // replay screen's controls are on their own layer and stay over it.
                presenterFrame(false, true, rect[2] ? rect : nullptr);
                g_publishedAny = true;
                frameTracePublish(noSignal ? "the static published as one flat image"
                                           : "the static left in the buffer published as one "
                                             "flat image",
                                  false, false);
            } else {
                ++g_noSignal.unplaced;
                frameTracePublish("HELD, the static is on a surface gplvr cannot place", false,
                                  false);
            }
        } else {
            if (g_noSignal.since) {
                ULONGLONG nowMs = GetTickCount64();
                bool blip = !g_noSignal.replay && g_noSignal.held == 1 && g_noSignal.flat == 0 &&
                            g_noSignal.unplaced == 0;
                if (blip) {
                    if (!g_noSignalBlips) g_noSignalBlipsSince = nowMs;
                    ++g_noSignalBlips;
                    if (nowMs - g_noSignalBlipsSince >= kNoSignalBlipMs) {
                        logf("vr: no signal -- %d one-frame holds in a race over the last %.1f s, "
                             "each kept the headset on its last frame. FlatFrames' `released "
                             "early for a BG request` count is the known source.",
                             g_noSignalBlips, (nowMs - g_noSignalBlipsSince) / 1000.0);
                        g_noSignalBlips = 0;
                    }
                } else if (g_noSignalReports > 0) {
                    --g_noSignalReports;
                    char shown[192] = "";
                    sprintf_s(shown, ". The last was drawn on surface %p, %dx%d, %s %d,%d %dx%d",
                              g_noSignal.surface, g_noSignal.size[0], g_noSignal.size[1],
                              g_noSignal.where ? g_noSignal.where
                                               : "not the 3D view's, an eye's, the frame's or a "
                                                 "framebuffer surface --",
                              g_noSignal.rect[0], g_noSignal.rect[1], g_noSignal.rect[2],
                              g_noSignal.rect[3]);
                    int frames = g_noSignal.held + g_noSignal.flat + g_noSignal.unplaced;
                    logf("vr: no signal -- GPL's static was in the buffer for %d frame%s (%llu "
                         "ms)%s: %d drew it, %d drew nothing at all and kept it; %d with the "
                         "stereo world rendered too. %d held, so the headset kept its last frame; "
                         "%d published as one flat image; %d held because the surface was not "
                         "one gplvr can place%s%s",
                         frames, frames == 1 ? "" : "s", nowMs - g_noSignal.since,
                         g_noSignal.replay ? " on the replay screen" : " in a race",
                         frames - g_noSignal.leftOver, g_noSignal.leftOver,
                         g_noSignal.withWorld, g_noSignal.held, g_noSignal.flat,
                         g_noSignal.unplaced, shown,
                         g_noSignalReports ? "" : " (further episodes will not be reported)");
                }
                g_noSignal = NoSignalEpisode{};
            }

            int eyeRect[2][4];
            bool pairStillInBuffer = g_lastFrameWasStereo && stereoEyeRect(0, eyeRect[0]) &&
                                     stereoEyeRect(1, eyeRect[1]);
            bool publishAsPair = g_stereoRenderedFrame || pairStillInBuffer;
            if (!g_stereoRenderedFrame && pairStillInBuffer && g_heldPairReports > 0) {
                --g_heldPairReports;
                logf("vr: no scene render this frame, but the eye targets are intact and the "
                     "last frame was a tracked pair -- published as side-by-side so each eye "
                     "samples its own rect. The head pose is cleared, so this frame holds "
                     "still. If this repeats for more than a moment the picture is frozen and "
                     "the cause is upstream: GPL drew no world.");
            }
            presenterFrame(publishAsPair, vrFrustumInjected());
            g_publishedAny = true;
            frameTracePublish(!publishAsPair         ? "published as one image"
                              : g_stereoRenderedFrame ? "published as a pair"
                                                      : "published as a HELD PAIR: no world was "
                                                        "rendered, the buffer's last pair went out",
                              publishAsPair, vrFrustumInjected());
        }
        noSignalEndFrame();
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
    glTraceStepIfStarted("present hook: the window blit and the pixels under the cursor");
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

    // Every stampDraw of the frame has been issued, so anything the fast text path still has
    // batched is drawn now -- before roglv2 presents, and after everything GPL drew over it.
    fastTextEndFrame();
    glTraceStepIfStarted("present hook: fast text's end of frame");

    frameBudgetSwitch(kSpanPresent);
    postProfilePresentBegin();
    LARGE_INTEGER beforePresent, afterPresent;
    QueryPerformanceCounter(&beforePresent);
    renderProfileSetInPresent(true);
    glTracePresent(false);
    const int presented =
        (!background && g_originalScreenDisplay) ? g_originalScreenDisplay(surface) : 0;
    glTracePresent(true);
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

    // GPL'S STATIC DOES NOT GET PAINTED OVER. It fills the window-sized 3D view at the top left
    // of the target, and the world frame that ends an episode does not cover every one of those
    // pixels: noise stayed visible along the top edge of the view after the static had gone
    // (user, 2026-10-18). Its frame has been published or held by now, so it is cleared before
    // GPL draws the next one, which either draws the static again or draws the world over black.
    if (clearStaticFromTarget) {
        GlFunctions gl;
        if (glResolve(gl)) renderTargetClear(gl);
    }

    // After the sim target, and it declines whenever that one took the frame: the two
    // are mutually exclusive by mode -- the off-screen target owns a world mode, this
    // owns the 640x480 front end -- and the invariant is checked rather than assumed.
    frameBudgetSwitch(kSpanRebindMenu);
    menuScaleBeginFrame();
    // Flat world modes, the complement of the two above; it copies the frame into the window
    // at the swap, inside the next present.
    flatTargetBeginFrame();

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
        mirrorDriverArmsEnsure();
        // What is actually rendered, not what GPL asked for: with mirrorUpdateInterval above 1 the
        // request comes every frame and `stereoRender` drops most of them, and a counter that
        // read the request would have reported the mirror at the full frame rate while it was
        // being drawn half as often.
        if (stereoMirrorDueThisFrame()) ++g_mirrorRefreshes;
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
    // off. Say so the first time one appears, because otherwise mirrorMode looks broken
    // when in fact there is simply no mirror pass to place.
    if (scene->mirrorCamera && !g_loggedMirror) {
        g_loggedMirror = true;
        logf("mirror camera appeared: %p -- the mirror pass now has something to place",
             scene->mirrorCamera);
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

    // The TV feed is drawn as flat GPL draws it, and GPL renders one rear view over the whole of
    // mirror.mip, so the glasses go back to the UVs their model was authored with. The headset's
    // regions are applied again on the first frame in the car.
    if (windowInFrontEnd()) mirrorUvRelease();

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
    bool  fovCorrected = !g_options.vr && windowCorrectMainFov(savedFov, scene->mainFov);

    if (g_options.vr && stereoRender(scene, &callOriginalSceneRender))
        g_stereoRenderedFrame = true;
    else
        callOriginalSceneRender(params);
    mirrorTargetEndScene();
    // GPL's 3D view has been repainted, which in flat mode paints over the replay bands' area
    // too; the replay panel's next capture starts empty to match.
    if (g_options.vr) replayPanelNoteScene();

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
        const char* carries = "per-pane mirrors are off";
        if (std::memcmp(site, kMainWalkStock, sizeof(kMainWalkStock)) != 0) {
            logf("walk: %08X does not hold the stock `call gpl_worldRenderSegmentRange` "
                 "(%02X %02X %02X %02X %02X) -- %s",
                 static_cast<unsigned>(kMainWalkSite), site[0], site[1], site[2], site[3],
                 site[4], carries);
        } else if (!patchBytes(site + 1, &rel, sizeof(rel))) {
            logf("walk: could not write the main world walk call at %08X -- %s",
                 static_cast<unsigned>(kMainWalkSite), carries);
        } else {
            g_mainWalkRedirected = true;
            logf("walk: main world walk call at %08X redirected to %08X -- it skips the walk "
                 "during a mirror-only render",
                 static_cast<unsigned>(kMainWalkSite),
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(&mainWorldWalkThunk)));
        }
    }

    // Written on every rebind, not once: GPL takes this from the graphics options and a mode
    // change puts the stock value back.
    //
    // Through rebase because this is a bare 4-byte store to a fixed address, with nothing about
    // the value that would identify the wrong one: on the plain link the global is 8 lower and
    // 0.25 went into a gpl_flushSegmentDrawQueue variable instead, which the log reported as a
    // successful write.
    auto* mirrorScale = reinterpret_cast<float*>(rebase(kMirrorDistanceScale));
    if (!mirrorScale) {
        if (!g_mirrorDistanceScaleSaid) {
            g_mirrorDistanceScaleSaid = true;
            logf("mirror: gpl_mirrorDistanceScale %08X cannot be placed in this image (deltas "
                 "%s), so it is left alone and the mirror keeps whatever reach GPL gave it",
                 static_cast<unsigned>(kMirrorDistanceScale), rebaseSummary());
        }
    } else if (*mirrorScale != kMirrorDistanceScaleWanted) {
        float stock = *mirrorScale;
        bool  wrote = patchBytes(mirrorScale, &kMirrorDistanceScaleWanted, sizeof(float));
        if (!g_mirrorDistanceScaleSaid) {
            g_mirrorDistanceScaleSaid = true;
            if (wrote)
                logf("mirror: gpl_mirrorDistanceScale at %08X set %.2f -> %.2f, which is the "
                     "fraction of the eye's draw distance the mirror walks. At 1.00 a modded "
                     "track's horizon is in reach and the sky is right; below that it is not.",
                     static_cast<unsigned>(reinterpret_cast<uintptr_t>(mirrorScale)), stock,
                     kMirrorDistanceScaleWanted);
            else
                logf("mirror: could not write gpl_mirrorDistanceScale at %08X -- it stays at "
                     "%.2f and the mirror keeps its shorter reach",
                     static_cast<unsigned>(reinterpret_cast<uintptr_t>(mirrorScale)), stock);
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


// EVERY HOTKEY, CHECKED AGAINST EVERY OTHER ONE. Two removed hotkeys once shipped on one key,
// both handlers polled GetAsyncKeyState independently, and one press did both things with
// nothing refusing.
//
// This is the whole table. A key added without a line here is a key this cannot see.
void hotkeyCollisionReport() {
    struct Entry {
        const char* name;
        int         code;
        bool        modeToggle;  // enters or leaves a keyboard tweak mode
    };
    const Entry keys[] = {
        {"vrMirrorTweakKey", g_options.vrMirrorTweakKey, true},
        {"vrMirrorModeKey", g_options.vrMirrorModeKey, false},
        {"vrRecentreKey", g_options.vrRecentreKey, false},
        {"vrMouseLookKey", g_options.vrMouseLookKey, false},
        {"vrHudLayerKey", g_options.vrHudLayerKey, false},
        {"vrHudTweakKey", g_options.vrHudTweakKey, true},
    };
    int count = static_cast<int>(sizeof(keys) / sizeof(keys[0]));
    int clashes = 0;
    int enabled = 0;
    for (int i = 0; i < count; ++i) {
        if (!keys[i].code) continue;  // 0 is off, and any number of features may be off
        ++enabled;
        for (int j = i + 1; j < count; ++j) {
            if (keys[j].code != keys[i].code) continue;
            ++clashes;
            logf("options: HOTKEY CLASH -- %s and %s are both 0x%02X ('%c'). Both handlers poll "
                 "the key and neither refuses, so one press does both things and any evidence "
                 "collected by it describes two different states. Give one of them another key "
                 "in gplvr.ini.",
                 keys[i].name, keys[j].name, keys[i].code,
                 (keys[i].code >= 32 && keys[i].code < 127) ? keys[i].code : '?');
        }
    }
    // The keyboard tweak modes steer with fixed keys, and the mirror's set contains the HUD's.
    // A hotkey on one of them is not read while a mode that owns it is on (hotkeyOwnedByMode);
    // a mode's own toggle there would also steer the other mode, so that is a clash.
    for (int i = 0; i < count; ++i) {
        if (!keys[i].code) continue;
        for (unsigned key : kMirrorTweakKeys) {
            if (static_cast<unsigned>(keys[i].code) != key) continue;
            if (keys[i].modeToggle) ++clashes;
            logf("options: %s is 0x%02X, a key the keyboard tweak modes steer with -- %s",
                 keys[i].name, keys[i].code,
                 keys[i].modeToggle ? "HOTKEY CLASH: inside the other mode it would also steer. Give it "
                          "another key in gplvr.ini."
                        : "it does nothing while a mode that uses the key is on");
        }
    }
    if (!clashes)
        logf("options: %d hotkeys bound of %d, no two on the same key", enabled, count);
}

void loadOptions(const char* fileName) {
    // Beside gplvr. An absolute path either way: GetPrivateProfile* resolves a bare name against
    // the working directory, which gplc67 moves to `font`.
    //
    // A path that will not fit is said out loud, because GetPrivateProfile* answers an unreadable
    // file with the default for every key -- which is a whole session running on defaults and
    // looking exactly like a session that was configured that way.
    char path[MAX_PATH] = {};
    if (!pathsOwnFile(path, sizeof(path), fileName))
        logf("options: %s%s does not fit in a path, so EVERY option below is its default",
             pathsOwnDirectory(), fileName);

    // Floats have to come back as text; GetPrivateProfileInt cannot represent them.
    // Declared up here because the float options are no longer all in one block at
    // the end -- the debug rotation ones sit with the keys they belong to.
    char text[64] = {};

    g_options.mirrorMode = GetPrivateProfileIntA("gplvr", "mirrorMode", kMirrorPerGlass, path);
    if (g_options.mirrorMode < kMirrorSharedHeadCentre || g_options.mirrorMode > kMirrorPerGlass)
        g_options.mirrorMode = kMirrorPerGlass;
    g_options.mirrorUpdateInterval = GetPrivateProfileIntA("gplvr", "mirrorUpdateInterval", 1, path);
    if (g_options.mirrorUpdateInterval < 0) g_options.mirrorUpdateInterval = 0;
    if (g_options.mirrorUpdateInterval > 16) g_options.mirrorUpdateInterval = 16;
    if (g_options.mirrorUpdateInterval > 1 && mirrorFromEachEye()) {
        logf("options: mirrorUpdateInterval = %d with the mirror from each eye needs a copy of "
             "mirror.mip per eye, because the two eyes render their panes into the same texture "
             "and a skipped frame would leave the left eye looking at the right eye's reflection. "
             "The copies are built on the first frame that renders the mirror; watch for the "
             "`mirror: each eye keeps its own copy` line, and for a `no per-eye copy` line "
             "naming what refused. Until they exist every frame renders, as before.",
             g_options.mirrorUpdateInterval);
    }
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
    g_options.profileFrame =
        GetPrivateProfileIntA("gplvr", "profileFrame", 0, path) != 0;
    g_options.profileGpuWait =
        GetPrivateProfileIntA("gplvr", "profileGpuWait", 0, path) != 0;
    g_options.profilePolys =
        GetPrivateProfileIntA("gplvr", "profilePolys", 0, path) != 0;
    g_options.profileStages =
        GetPrivateProfileIntA("gplvr", "profileStages", 0, path) != 0;
    g_options.profilePost = GetPrivateProfileIntA("gplvr", "profilePost", 0, path) != 0;
    g_options.glTrace = GetPrivateProfileIntA("gplvr", "glTrace", 0, path);
    if (g_options.glTrace < 0) g_options.glTrace = 0;
    if (g_options.glTrace > 2) g_options.glTrace = 2;
    g_options.profileWorkWait =
        GetPrivateProfileIntA("gplvr", "profileWorkWait", 0, path) != 0 ||
        g_options.profileStages;
    g_options.profileStallUs = GetPrivateProfileIntA("gplvr", "profileStallUs", 50, path);
    if (g_options.profileStallUs < 0) g_options.profileStallUs = 0;
    if (g_options.profileStallUs > 10000) g_options.profileStallUs = 10000;
    g_options.vr = GetPrivateProfileIntA("gplvr", "vr", 1, path) != 0;
    // Before anything reads vr: every VR-only install gate then sees the flat answer, exactly as
    // with vr = 0 in the ini.
    if (g_options.vr) {
        char runtime[1200];
        if (xrRuntimeFind(runtime, sizeof(runtime))) {
            logf("vr: OpenXR runtime -- %s", runtime);
        } else {
            g_options.vr = false;
            logf("vr: NO OpenXR runtime -- %s. Playing flat, as with vr = 0; install or select a "
                 "runtime (SteamVR, Oculus, Pico, Virtual Desktop...) to play in the headset",
                 runtime);
        }
    }
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
    GetPrivateProfileStringA("gplvr", "vrReplayHorizonLock", "1.0", text, sizeof(text), path);
    g_options.vrReplayHorizonLock = static_cast<float>(atof(text));
    g_options.vrHeadAudio = GetPrivateProfileIntA("gplvr", "vrHeadAudio", 1, path) != 0;
    GetPrivateProfileStringA("gplvr", "vrHeadAudioWidth", "1.0", text, sizeof(text), path);
    g_options.vrHeadAudioWidth = static_cast<float>(atof(text));
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
    g_options.windowScreen = GetPrivateProfileIntA("gplvr", "windowScreen", 0, path);
    if (g_options.windowScreen < 0) g_options.windowScreen = 0;
    g_options.renderWhenUnfocused =
        GetPrivateProfileIntA("gplvr", "renderWhenUnfocused", 0, path) != 0;
    g_options.menuFilter = GetPrivateProfileIntA("gplvr", "menuFilter", 2, path);
    if (g_options.menuFilter < 0 || g_options.menuFilter > 2) g_options.menuFilter = 2;
    g_options.enableLetterbox =
        GetPrivateProfileIntA("gplvr", "enableLetterbox", 0, path) != 0;
    g_options.widescreenFix =
        GetPrivateProfileIntA("gplvr", "widescreenFix", 1, path) != 0;
    g_options.exitMode = GetPrivateProfileIntA("gplvr", "exitMode", 2, path);
    if (g_options.exitMode < 0 || g_options.exitMode > 2) g_options.exitMode = 2;
    g_options.mirrorDriverArms =
        GetPrivateProfileIntA("gplvr", "mirrorDriverArms", 1, path) != 0;
    g_options.vrGplMenuMode = GetPrivateProfileIntA("gplvr", "vrGplMenuMode", 1, path);
    if (g_options.vrGplMenuMode < 0 || g_options.vrGplMenuMode > 1) g_options.vrGplMenuMode = 1;
    GetPrivateProfileStringA("gplvr", "vrGplMenuScale", "0.5", text, sizeof(text), path);
    g_options.vrGplMenuScale = static_cast<float>(atof(text));
    if (!(g_options.vrGplMenuScale >= 0.05f)) g_options.vrGplMenuScale = 0.05f;
    if (g_options.vrGplMenuScale > 1.0f) g_options.vrGplMenuScale = 1.0f;
    GetPrivateProfileStringA("gplvr", "vrGplMenuDistance", "2.0", text, sizeof(text), path);
    g_options.vrGplMenuDistance = static_cast<float>(atof(text));
    if (!(g_options.vrGplMenuDistance >= 0.75f)) g_options.vrGplMenuDistance = 0.75f;
    if (g_options.vrGplMenuDistance > 5.0f) g_options.vrGplMenuDistance = 5.0f;
    GetPrivateProfileStringA("gplvr", "vrGamma", "1.2", text, sizeof(text), path);
    g_options.vrGamma = static_cast<float>(atof(text));
    if (g_options.vrGamma < 0.5f) g_options.vrGamma = 0.5f;
    if (g_options.vrGamma > 3.0f) g_options.vrGamma = 3.0f;
    GetPrivateProfileStringA("gplvr", "vrGplTextDepth", "2.0", text, sizeof(text), path);
    g_options.vrGplTextDepth = static_cast<float>(atof(text));
    g_options.vrHudMode = GetPrivateProfileIntA("gplvr", "vrHudMode", 1, path);
    if (g_options.vrHudMode < 0 || g_options.vrHudMode > 2) g_options.vrHudMode = 1;
    g_options.vrGplTextScale = GetPrivateProfileIntA("gplvr", "vrGplTextScale", 2, path);
    if (g_options.vrGplTextScale < 1) g_options.vrGplTextScale = 1;
    if (g_options.vrGplTextScale > 4) g_options.vrGplTextScale = 4;
    GetPrivateProfileStringA("gplvr", "vrGplTextSpread", "0.25", text, sizeof(text), path);
    g_options.vrGplTextSpread = static_cast<float>(atof(text));
    // 0 is every run on one point at the eye's centre, which reads as the text being gone
    // rather than as a setting being wrong.
    if (g_options.vrGplTextSpread < 0.1f) g_options.vrGplTextSpread = 0.1f;
    if (g_options.vrGplTextSpread > 1.0f) g_options.vrGplTextSpread = 1.0f;
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
    g_options.vrHudLayerKey = GetPrivateProfileIntA("gplvr", "vrHudLayerKey", VK_HOME, path);
    g_options.vrHudTweakKey = GetPrivateProfileIntA("gplvr", "vrHudTweakKey", VK_END, path);
    GetPrivateProfileStringA("gplvr", "vrHudTweakSpeed", "0.10", text, sizeof(text), path);
    g_options.vrHudTweakSpeed = static_cast<float>(atof(text));
    if (g_options.vrHudTweakSpeed < 0.005f) g_options.vrHudTweakSpeed = 0.005f;
    if (g_options.vrHudTweakSpeed > 2.0f) g_options.vrHudTweakSpeed = 2.0f;
    g_options.frameRate = GetPrivateProfileIntA("gplvr", "frameRate", 1, path);
    if (g_options.frameRate < 0 || g_options.frameRate > 2) g_options.frameRate = 1;

    // FRAME RATE IS ONE LEVER, AND VSYNC FALLS OUT OF IT.
    //
    // They used to be two settings with one subtle relationship, and the combination in the
    // shipped file was a lie: `frameRate = 1` in flat mode means "one frame per refresh", and
    // it achieves that by letting SwapBuffers block -- so with vsync off it silently ran to
    // the 7-frames-per-tick ceiling instead. No wording fixes a setting whose meaning depends
    // on another setting three hundred lines away.
    //
    //   0  vanilla    NO CALL AT ALL, because that is what vanilla is. GPL never touches the
    //                 swap interval, so the specified initial value of 1 stands and vanilla
    //                 GPL therefore runs with vsync ON. Forcing 0 here would make "vanilla"
    //                 less vanilla than leaving it alone.
    //   1  display    vsync on in flat mode, where the swap IS the pace.
    //   2  unlimited  vsync off, or nothing is unlimited.
    //
    // VR takes the question away: the rate comes from the presenter, each frame waiting for a
    // newly published view set. SwapBuffers still runs, because the game still presents to the
    // monitor window, so an interval above 0 would block the frame loop on the MONITOR'S
    // refresh and cap the headset with it -- a 60 Hz monitor holding a 90 Hz headset at 60,
    // which reads as a performance fault rather than as a setting.
    //
    // TWO SPACES MEET HERE AND ONLY ONE IS THE DRIVER'S. What the ini offers is -1 follow,
    // 0 force off, 1..4 force on; what g_options.swapInterval carries afterwards is the
    // resolved interval, where -1 is "make no call". They share the number -1 and do not
    // share its meaning, which is why the ini's value stays in a local and never reaches the
    // option field unresolved.
    int wanted = GetPrivateProfileIntA("gplvr", "swapInterval", -1, path);
    if (wanted < -1) wanted = -1;
    if (wanted > 4) wanted = 4;

    int derived = 0;
    if (!g_options.vr) {
        if (g_options.frameRate == 0)
            derived = -1;
        else if (g_options.frameRate == 1)
            derived = 1;
    }

    g_options.swapInterval = derived;
    if (wanted >= 0) {
        if (g_options.vr && wanted != 0) {
            logf("options: swapInterval = %d refused -- vsync in VR blocks the frame loop on "
                 "the monitor's refresh and caps the headset with it",
                 wanted);
        } else {
            g_options.swapInterval = wanted;
            if (wanted != derived)
                logf("options: swapInterval = %d overrides the %d that frameRate = %d would "
                     "have chosen. -1 follows it again.",
                     wanted, derived, g_options.frameRate);
        }
    }
    g_options.vrRendererType = GetPrivateProfileIntA("gplvr", "vrRendererType", 1, path);
    if (g_options.vrRendererType < 0 || g_options.vrRendererType > 2) g_options.vrRendererType = 1;
    g_options.flatRendererType = GetPrivateProfileIntA("gplvr", "flatRendererType", 0, path);
    if (g_options.flatRendererType < 0 || g_options.flatRendererType > 2)
        g_options.flatRendererType = 0;
    g_options.flatOffscreenFrame =
        GetPrivateProfileIntA("gplvr", "flatOffscreenFrame", 0, path) != 0;
    GetPrivateProfileStringA("gplvr", "vrRenderScale", "1.0", text, sizeof(text), path);
    g_options.vrRenderScale = static_cast<float>(atof(text));

    // Flat mode is the vanilla game in a borderless window: the window itself, the
    // letterbox and widescreen fixes, front-end scaling, roglv2's glFinish drop and swap
    // interval, and mouselook. The tools built to develop and measure the headset path --
    // the mirror keys, the stage and polygon profilers -- are for vr only. Switching them off
    // here keeps every installer and key that reads them inert, without a second check at
    // each one. Stereo itself needs no entry: it IS vr now, not a setting of its own.
    if (!g_options.vr) {
        g_options.vrMirrorTweakKey = 0;
        g_options.vrMirrorModeKey = 0;
        g_options.profilePolys = false;
        // profileWorkWait survives: the work/wait split is the one measurement that has to
        // read the same frame in both modes, and it owns no GPU marks, so it does not
        // collide with profilePost.
        g_options.profileStages = false;
        // frameRate SURVIVES. It is not about the headset: 144 fps on a 36 Hz simulation is
        // the same mechanism doing the same thing in flat GPL, and switching it off here is
        // what used to make it read as a VR feature.
    }

    // The hotkeys are debug controls; the VR menu has what each of them toggles. Off, every key
    // reads 0, which each poll, each swallow and the clash table already take as "no key".
    g_options.debugKeys = GetPrivateProfileIntA("gplvr", "debugKeys", 0, path) != 0;
    if (!g_options.debugKeys) {
        g_options.vrMirrorTweakKey = 0;
        g_options.vrMirrorModeKey = 0;
        g_options.vrRecentreKey = 0;
        g_options.vrMouseLookKey = 0;
        g_options.vrHudLayerKey = 0;
        g_options.vrHudTweakKey = 0;
    }
    logf("options: debugKeys=%d (%s)", g_options.debugKeys ? 1 : 0,
         g_options.debugKeys ? "the hotkeys of gplvr.ini are polled"
                             : "no hotkey is polled; the VR menu has every setting they toggle");

    logf("build: GPLVR_MIRROR_SKIP_OUT_OF_VIEW=%d (%s) GPLVR_MIRROR_COPY_REBIND=%d",
         kMirrorSkipOutOfView ? 1 : 0,
         kMirrorSkipOutOfView
             ? "a mirror pane whose glass no eye can see this frame is not rendered; watch "
               "`mirror panes N, out of view M` on the fps line"
             : "off: every pane is rendered on every mirror frame",
         kMirrorCopyRebind ? 1 : 0);
    logf("options: mirrorMode=%d (%s) mirrorUpdateInterval=%d (%s) -- the aim of "
         "the glasses is per car, in gplvr_mirrors.ini, and has no gplvr.ini keys",
         g_options.mirrorMode, mirrorModeText(g_options.mirrorMode),
         g_options.mirrorUpdateInterval,
         g_options.mirrorUpdateInterval <= 0
             ? "the mirror is OFF and its texture is filled with black"
         : g_options.mirrorUpdateInterval > 1
             ? "the mirror is rendered on one frame in mirrorUpdateInterval, counting from the "
               "frame that latched the tick; the rest keep the last image"
             : "the mirror is rendered on every frame");
    logf("options: vrMirrorTweakKey=0x%02X vrMirrorTweakAngleSpeed=%.2f "
         "vrMirrorTweakFovSpeed=%.2f vrMirrorModeKey=0x%02X",
         g_options.vrMirrorTweakKey, g_options.vrMirrorTweakAngleSpeed,
         g_options.vrMirrorTweakFovSpeed, g_options.vrMirrorModeKey);
    mirrorConfigReset();
    g_options.vrMonitorEye = GetPrivateProfileIntA("gplvr", "vrMonitorEye", kMonitorRight, path);
    if (g_options.vrMonitorEye < kMonitorOff || g_options.vrMonitorEye > kMonitorBoth)
        g_options.vrMonitorEye = kMonitorRight;
    g_options.vrTransferFinish =
        GetPrivateProfileIntA("gplvr", "vrTransferFinish", 1, path) != 0;
    g_options.vrDirectToShared =
        GetPrivateProfileIntA("gplvr", "vrDirectToShared", 0, path) != 0;
    logf("options: vrDirectToShared=%d (%s)", g_options.vrDirectToShared ? 1 : 0,
         g_options.vrDirectToShared
             ? "the frame is rendered straight into the shared texture, and the render target "
               "is a depth buffer alone"
             : "the frame is rendered into a colour texture of our own and copied across at "
               "publish");
    g_options.pribludaUpdateInterval =
        GetPrivateProfileIntA("gplvr", "pribludaUpdateInterval", 2, path);
    if (g_options.pribludaUpdateInterval < 1) g_options.pribludaUpdateInterval = 1;
    if (g_options.pribludaUpdateInterval > 8) g_options.pribludaUpdateInterval = 8;
    logf("options: vrTransferFinish=%d pribludaUpdateInterval=%d (the HUD panel is redrawn on one "
         "frame in %d)",
         g_options.vrTransferFinish ? 1 : 0, g_options.pribludaUpdateInterval,
         g_options.pribludaUpdateInterval);
    g_options.vrNoSignalDelay = GetPrivateProfileIntA("gplvr", "vrNoSignalDelay", 250, path);
    if (g_options.vrNoSignalDelay < 0) g_options.vrNoSignalDelay = 0;
    if (g_options.vrNoSignalDelay > 10000) g_options.vrNoSignalDelay = 10000;
    logf("options: vrNoSignalDelay=%d ms (GPL's no-signal static: a race holds the last frame "
         "for that long, then the static fills both eyes; the replay screen fills them at once "
         "-- watch the `vr: no signal` lines)",
         g_options.vrNoSignalDelay);

    logf("options: vrGplMenuMode=%d (%s) vrGplMenuScale=%.2f vrGplMenuDistance=%.2f",
         g_options.vrGplMenuMode,
         g_options.vrGplMenuMode == 1 ? "a panel fixed in front of the seat"
                                      : "head-locked in the eye images",
         g_options.vrGplMenuScale, g_options.vrGplMenuDistance);
    logf("options: vrGamma=%.2f vrGplTextDepth=%.2f vrRenderScale=%.2f "
         "vrRecentreKey=0x%02X vrMonitorEye=%d",
         g_options.vrGamma, g_options.vrGplTextDepth,
         g_options.vrRenderScale, g_options.vrRecentreKey, g_options.vrMonitorEye);
    logf("options: vrHorizonLock=%.2f vrReplayHorizonLock=%.2f vrWorldScale=%.2f vrHeadAudio=%d "
         "vrHeadAudioWidth=%.2f",
         g_options.vrHorizonLock, g_options.vrReplayHorizonLock, g_options.vrWorldScale,
         g_options.vrHeadAudio ? 1 : 0, g_options.vrHeadAudioWidth);
    logf("options: vrHudMode=%d (%s) vrHudBackdropOpacity=%.2f", g_options.vrHudMode,
         g_options.vrHudMode == 0   ? "off"
         : g_options.vrHudMode == 1 ? "fixed"
                                    : "head-locked",
         g_options.vrHudBackdropOpacity);
    logf("options: vrGplTextScale=%d vrGplTextSpread=%.2f -- in VR gplvr draws GPL's stamped "
         "glyphs from its own atlas, once per eye, scaled and pulled in -- watch the `fast "
         "text:` lines for how many glyphs it took and which term refused the rest",
         g_options.vrGplTextScale, g_options.vrGplTextSpread);
    logf("options: frameRate=%d (%s)", g_options.frameRate,
         g_options.frameRate == 0
             ? "THE SIM'S RATE: one frame per sim tick, unblended, as GPL ships"
         : g_options.frameRate == 1
             ? (g_options.vr ? "THE PRESENT RATE: the loop's wait for the next sim tick is "
                               "skipped and one frame is drawn per view set the presenter "
                               "publishes -- the HEADSET's rate, whatever the monitor is doing"
                             : "THE DISPLAY'S RATE: the loop's wait for the next sim tick is "
                               "skipped; swapInterval decides whether SwapBuffers then blocks "
                               "for the refresh")
             : "UNLOCKED: the loop's wait for the next sim tick is skipped and nothing else "
               "paces it, so the rate is 1000 / the cost of one frame");
    if (g_options.frameRate > 0)
        logf("options: the world is therefore BLENDED -- every car and the view are drawn "
             "between the two newest latched sim ticks at the frame's own display time, which "
             "is what makes a frame between two ticks a new picture rather than a copy. It "
             "costs one sim tick of latency (6.94 ms at 144 Hz); frameRate = 0 does not pay "
             "it and does not get the rate.");
    logf("options: flatOffscreenFrame=%d (%s)", g_options.flatOffscreenFrame ? 1 : 0,
         g_options.flatOffscreenFrame
             ? "flat world frames are drawn off-screen and copied into the window at the swap; "
               "watch the `flat frame:` lines"
             : "flat world frames are drawn straight into the window");
    logf("options: exitMode=%d (%s)", g_options.exitMode,
         g_options.exitMode == 0   ? "vanilla: the credits roll, Alt+F4 and Close do nothing"
         : g_options.exitMode == 1 ? "no credits; Alt+F4 and Close do nothing"
                                   : "no credits; Alt+F4 and Close exit through GPL's own exits");
    logf("options: vrRendererType=%d flatRendererType=%d, this mode's %d (%s)",
         g_options.vrRendererType, g_options.flatRendererType, rendererType(),
         rendererType() == 0
             ? "vanilla: GPL draws everything; in VR the VR menu's Renderer row switches it live"
         : rendererType() == 1
             ? "gplvr's renderer: the track baked on a worker thread and drawn with real depth, "
               "the other cars, the flagmen, the cockpit and the mirrors drawn by gplvr, GPL the "
               "rest, tested against that depth"
             : "gplvr's renderer, and a red square marking every rectangle it drew");
    logf("options: profilePost=%d (%s)", g_options.profilePost ? 1 : 0,
         g_options.profilePost ? "post is split into roglv2's flushes and the rest, CPU and GPU"
                               : "post is one number");
    logf("options: glTrace=%d (%s)", g_options.glTrace,
         g_options.glTrace == 0   ? "off"
         : g_options.glTrace == 1 ? "the driver's GL debug messages are logged"
                                  : "the driver's GL debug messages are logged, and every step of "
                                    "the first 300 frames from the first baked pass is finished "
                                    "on the GPU and logged -- slow");
    logf("options: profileWorkWait=%d profileStallUs=%d (%s)",
         g_options.profileWorkWait ? 1 : 0, g_options.profileStallUs,
         g_options.profileWorkWait
             ? "every millisecond of the frame is reported as CPU WORK or CPU WAIT"
             : "no work/wait split -- pre and present stay single numbers");
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
    logf("options: window=%dx%d at %d,%d windowScreen=%d menuFilter=%d enableLetterbox=%d "
         "widescreenFix=%d renderWhenUnfocused=%d%s",
         g_options.windowWidth, g_options.windowHeight, g_options.windowX,
         g_options.windowY, g_options.windowScreen, g_options.menuFilter,
         g_options.enableLetterbox ? 1 : 0,
         g_options.widescreenFix ? 1 : 0, g_options.renderWhenUnfocused ? 1 : 0,
         (!g_options.renderWhenUnfocused && g_options.vr) ? " (forced on by vr)" : "");

    hotkeyCollisionReport();
}

// The eight keys loadOptions reads just above, written back where the panel tweak mode
// leaves them. Both layers go every time: vrHudMode decides which one is live, and the
// other one's numbers are still the driver's own from an earlier session.
//
// The leading space in the format is the value's: WritePrivateProfileString keeps the text
// left of the `=` as the file has it and replaces the rest, so "0.70" would turn a
// hand-written `vrHudSeatAhead = 0.70` into `vrHudSeatAhead =0.70`. GetPrivateProfile*
// skips it coming back in.
bool hudPlacementSave() {
    char path[MAX_PATH] = {};
    pathsOwnFile(path, sizeof(path), "gplvr.ini");

    int  failed = 0;
    auto write = [&](const char* key, float value) {
        char text[32] = {};
        _snprintf_s(text, sizeof(text), _TRUNCATE, " %.3f", static_cast<double>(value));
        if (!WritePrivateProfileStringA("gplvr", key, text, path)) ++failed;
    };
    write("vrHudHeadAhead", g_options.vrHudHead.ahead);
    write("vrHudHeadDown", g_options.vrHudHead.down);
    write("vrHudHeadRight", g_options.vrHudHead.right);
    write("vrHudHeadWidth", g_options.vrHudHead.width);
    write("vrHudSeatAhead", g_options.vrHudSeat.ahead);
    write("vrHudSeatDown", g_options.vrHudSeat.down);
    write("vrHudSeatRight", g_options.vrHudSeat.right);
    write("vrHudSeatWidth", g_options.vrHudSeat.width);

    if (failed) {
        logf("pribluda: %d of 8 HUD panel placements could not be written to %s (error %lu)",
             failed, path, GetLastError());
        return false;
    }
    logf("pribluda: HUD panel placement written to %s -- head ahead %.3f down %.3f "
         "right %.3f width %.3f | seat ahead %.3f down %.3f right %.3f width %.3f",
         path, g_options.vrHudHead.ahead, g_options.vrHudHead.down,
         g_options.vrHudHead.right, g_options.vrHudHead.width, g_options.vrHudSeat.ahead,
         g_options.vrHudSeat.down, g_options.vrHudSeat.right, g_options.vrHudSeat.width);
    return true;
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
    // and the widescreen field of view reach the camera on the flat path. Stereo and the
    // off-screen target, and the eye-frustum hook on cameraBind below, are the headset's.
    if (g_options.vr) {
        stereoBind(binding);
        renderTargetBind(binding);
    }
    // Both modes: the baked cockpit overlay reaches mirror.mip's GL texture through it.
    renderTargetBindMips(binding);

    // BOTH MODES. It resolves four exported scheduler functions and patches nothing, and what it
    // answers -- the simulation's tick rate -- is read by the blend and by three report lines.
    // Behind the VR gate it left taskRateSimHz() returning 0 for every flat session, which is
    // not "slow" but "wrong": Interpolation could not tell 36 Hz from 144 and had to measure the
    // period for itself.
    taskRateBind(&binding.image);

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
    // Not conditional on the mirror being ON: mirrorUpdateInterval can go to 0 and back from the
    // menu at any time, and the redirect is installed once at bind. The baked renderer needs it
    // in flat mode as well: the main walk is where it finds the track and, later, draws it.
    g_mainWalkPending =
        (g_options.vr || rendererType() != 0) && binding.sceneRender == 0x00487620;

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

unsigned hooksFrameCount() { return g_frames; }

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

// SKIPPING THE CREDITS IS ONE BYTE, AND GPL ALREADY HAS THE SWITCH.
//
// The quit path is sub_423C20: it calls sub_40ECF0 and branches on the result -- true shows the
// credits screen (the global at 0x5B6170, whose vtable 0x537298 names credits.win), false calls
// the object's own +0x28 and the game exits.
//
//     0x423C3F  call sub_40ECF0
//     0x423C44  test al, al
//     0x423C46  jz   short loc_423C57     <- the exit without credits
//
// sub_40ECF0 is `return byte_54C5CC;` and that byte has exactly two references in the whole
// image: this read, and a write inside an accessor at 0x40ED00 that NOTHING calls. It ships as 1
// and nothing ever changes it, so writing 0 once is permanent and needs no code patch, no hook
// and no cave -- the branch GPL already has is simply taken.
//
// THE ADDRESS IS INSIDE GPLPATCHDLL'S REBASE BAND (0x0054C5B8..0x00971FFF), so it is one of the
// globals that moves between the UK and US links. It goes through rebase() like everything else
// that does; reading it raw would poke a neighbour on one of the two.
constexpr uintptr_t kRetailCreditsOnExit = 0x0054C5CC;

void creditsSkipInstall() {
    if (g_options.exitMode < 1) return;

    uintptr_t flag = rebase(kRetailCreditsOnExit);
    if (!flag) {
        logf("credits: the exit-credits flag could not be placed in this image -- the credits "
             "roll as they always did");
        return;
    }

    unsigned char before = 0;
    std::memcpy(&before, reinterpret_cast<const void*>(flag), sizeof(before));
    unsigned char off = 0;
    if (!patchBytes(reinterpret_cast<void*>(flag), &off, sizeof(off))) {
        logf("credits: %08X could not be written -- the credits roll as they always did",
             static_cast<unsigned>(flag));
        return;
    }
    logf("credits: quitting now exits straight away -- %08X %u -> 0, so the quit path takes the "
         "branch past credits.win. exitMode = 0 leaves it alone.",
         static_cast<unsigned>(flag), before);
}

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
    // The game's thread loads and unloads DLLs while this runs (dump 8160: a module listed here
    // was gone when its header was read). Each module is pinned while its imports are read, and
    // one that is no longer loaded is skipped.
    for (DWORD i = 0; i < count && i < 512; ++i) {
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(modules[i]), &pinned) ||
            pinned != modules[i]) {
            if (pinned) FreeLibrary(pinned);
            continue;
        }
        patchDisplaySettingsImports(modules[i]);
        FreeLibrary(pinned);
    }
}

}  // namespace gplvr
