#include "FlatFrames.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Interpolation.h"
#include "Log.h"
#include "RenderProfile.h"
#include "TickClock.h"
#include "Window.h"
#include "presenter/Producer.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe) ---------------------------------------------------------------

// gpl_fgEventLoop 0x4F3570's call to gpl_fgFrameSnapshotBegin 0x4EF5F0, the first thing in every
// frame. The only call to it in the image.
constexpr uintptr_t     kSnapshotBeginCall = 0x004F3582;
constexpr uintptr_t     kSnapshotBegin = 0x004EF5F0;
constexpr unsigned char kSnapshotBeginStock[5] = {0xE8, 0x69, 0xC0, 0xFF, 0xFF};

// gpl_fgFrameSnapshotEnd 0x4EF650, called at the bottom of the same loop iteration.
//
// IT DESTROYS THE FRAME. gpl_fgSnapshotRelease 0x4E8FA0 runs each slot's release callback, zeroes
// the slot's data pointer in the frame descriptor and clears its valid byte (byte_94D35C), so
// between End and the next Begin the game HAS no frame to draw. gpl_drivingViewWidgetDraw
// 0x423100 asks its getter for one, gets 0, and takes its else branch: a black clear and a noise
// texture on a randomly rotated quad, with a sound channel started alongside it -- GPL's own
// "no signal" screen. An extra frame that merely skipped Begin drew exactly that.
//
// So the snapshot is held OPEN across the tick's extra frames and End is skipped with Begin. It
// is not leaked: gpl_fgFrameSnapshotBegin opens with `if (dword_94D714) gpl_fgFrameSnapshotEnd()`,
// so the next real frame performs the release before it latches again. The snapshot's lifetime is
// unchanged -- in stock GPL it already spans a whole tick, because Begin is where the frame waits.
constexpr uintptr_t     kSnapshotEndCall = 0x004F35FF;
constexpr uintptr_t     kSnapshotEnd = 0x004EF650;
constexpr unsigned char kSnapshotEndStock[5] = {0xE8, 0x4C, 0xC0, 0xFF, 0xFF};

// gpl_fgLastLatchedBgCount: the tick the last latch copied. gpl_fgWaitForTickAndSnapshot waits
// until getBGCount() differs from it, so the two being equal is exactly "the sim has nothing new
// and the loop is about to sleep".
constexpr uintptr_t kLastLatchedBgCount = 0x00582114;

// The same global as this image keeps it, placed in flatFramesInstall. Zero until then.
uintptr_t g_lastLatchedBgCountAt = 0;

using BeginFn = void(__cdecl*)();
using EndFn = void(__cdecl*)();
using CountFn = int(__cdecl*)();
using FlagFn = bool(__cdecl*)();

struct State {
    bool    installed = false;
    BeginFn begin = nullptr;
    EndFn   end = nullptr;
    CountFn getBGCount = nullptr;
    FlagFn  bgRequested = nullptr;

    long long frequency = 0;
    // The presenter's view counter as it stood when this loop last let a frame through.
    //
    // Ours, not `presenterAcquiredViewSequence` -- that is updated by the eye cameras, which
    // only run on a tracked stereo frame, so menus and replays left it frozen and the gate
    // never closed: frameRate = 1 free-ran at 286 fps, which is what "despite frameRate = 1 it
    // renders >90 fps" was.
    int32_t   lastViewSequence = 0;
    long long tickStart = 0;   // when the tick now being drawn was latched
    double    tickPeriod = 0.0;
    bool      haveTick = false;
    int       extras = 0;      // extra frames already drawn against this tick
    int       deferrals = 0;   // frames that left a waiting tick unlatched since the last latch

    // The window since the last report.
    unsigned windowFrames = 0;
    unsigned windowExtra = 0;
    unsigned windowBlended = 0;   // extra frames that actually moved the world
    unsigned windowWaited = 0;    // extra frames that had to wait for their slice
    unsigned windowPreempted = 0; // of those, the ones that gave up to keep a tick latched
    unsigned windowOverdue = 0;   // stock frames taken because the tick was overdue
    unsigned windowBackground = 0;  // stock frames taken because nothing will be presented
    unsigned windowTickless = 0;  // refused because no tick has been latched yet
    unsigned windowMaxExtra = 0;  // the most drawn against any one tick in the window
    unsigned windowHeld = 0;      // ends deferred so the frame survived into an extra draw
    unsigned windowReleased = 0;  // ends let through because a BG request was waiting
    unsigned windowCapped = 0;    // frames that waited for the frame cap's slot

    // The frame cap (`frameLimit`): when the next frame may start, in QPC ticks, and the timer
    // that sleeps towards it.
    double limitDue = 0.0;
    HANDLE limitTimer = nullptr;
    bool   limitTimerTried = false;
    bool   limitTimerPrecise = false;
};

State g_state;

long long now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

uintptr_t callTarget(uintptr_t site) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const uint8_t*>(site) + 1, sizeof(rel));
    return site + 5 + rel;
}

bool redirect(uintptr_t site, uintptr_t to) {
    auto    displacement = reinterpret_cast<volatile int32_t*>(site + 1);
    int32_t rel = static_cast<int32_t>(to - (site + 5));
    DWORD   previous = 0;
    if (!VirtualProtect(const_cast<int32_t*>(displacement), sizeof(rel), PAGE_EXECUTE_READWRITE,
                        &previous))
        return false;
    *displacement = rel;
    DWORD ignored = 0;
    VirtualProtect(const_cast<int32_t*>(displacement), sizeof(rel), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), const_cast<int32_t*>(displacement), sizeof(rel));
    return true;
}

// frameRate 1 and 2 both skip the loop's wait for the next tick; only the gate in front of each
// extra frame differs. 0 never draws one.
bool extraFramesOn() { return g_options.frameRate > 0; }

// Windows 10 1803 and later; older SDK headers do not name it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Wait until `due`, a QPC instant. A waitable timer takes the thread to within a margin of it
// and the rest is spun, because no timer wakes a thread to a tenth of a millisecond. The
// high-resolution timer is good to about half a millisecond; where the system does not have one
// the ordinary timer follows the system timer resolution, which gplvr raises to 1 ms at start-up.
void waitUntil(State& s, long long due) {
    if (!s.limitTimerTried) {
        s.limitTimerTried = true;
        s.limitTimer = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        s.limitTimerPrecise = s.limitTimer != nullptr;
        if (!s.limitTimer) s.limitTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        logf("frames: the frame cap sleeps on %s",
             s.limitTimerPrecise ? "a high-resolution waitable timer and spins the last "
                                   "millisecond"
             : s.limitTimer      ? "an ordinary waitable timer and spins the last two "
                                   "milliseconds (no high-resolution timer on this system)"
                                 : "nothing -- no waitable timer could be created, so it spins "
                                   "the whole wait");
    }

    long long margin = s.frequency * (s.limitTimerPrecise ? 1 : 2) / 1000;
    long long remaining = due - now();
    if (s.limitTimer && remaining > margin) {
        // Relative due times are negative and count 100 ns units.
        LARGE_INTEGER relative;
        relative.QuadPart = -((remaining - margin) * 10000000LL / s.frequency);
        if (SetWaitableTimer(s.limitTimer, &relative, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(s.limitTimer, 1000);
    }
    while (now() < due) YieldProcessor();
}

// The frame cap. Each frame has a slot one period after the previous slot and waits for it before
// it starts, ahead of the latch, so the frame that follows is drawn from the newest tick there is
// at that moment. Spacing slots from the previous slot keeps one late wake-up from shifting every
// frame after it. A frame more than a period late starts a new run of slots from itself.
void waitForFrameSlot(State& s) {
    if (g_options.vr || g_options.frameLimit <= 0 || s.frequency <= 0) return;

    double period = static_cast<double>(s.frequency) / g_options.frameLimit;
    double at = static_cast<double>(now());
    if (s.limitDue <= 0.0 || at - s.limitDue > period) {
        s.limitDue = at + period;
        return;
    }
    if (at < s.limitDue) {
        waitUntil(s, static_cast<long long>(s.limitDue));
        ++s.windowCapped;
    }
    s.limitDue += period;
}

// A tick late by a whole period: the sim has stalled or stopped, so the loop goes back to GPL's
// own wait, which ends when a tick lands or the sim is no longer running.
bool tickOverdue(const State& s) {
    return s.tickPeriod > 0.0 && static_cast<double>(now() - s.tickStart) > 2.0 * s.tickPeriod;
}

// Unfocused in flat mode the present hook skips the present, so nothing would pace the loop.
bool nothingPresented() { return !keepDrawingWhileUnfocused() && !windowHasFocus(); }

// Is this frame allowed to be drawn yet?
//
//   2  unlocked -- always. The next frame starts when the last one finished, which is what
//      unlocked means.
//   1  the display's rate -- in VR, when the presenter has published a view set newer than the
//      one the last render used, so exactly one frame is drawn per display frame. In flat mode
//      there is no such clock: SwapBuffers is the gate, and `swapInterval` decides whether it
//      blocks, so the answer here is always yes and the driver does the pacing.
bool displayIsReady(const State& s) {
    if (g_options.frameRate != 1) return true;
    if (!g_options.vr) return true;
    int32_t published = presenterViewSequence();
    if (!published) return true;  // not publishing yet; do not stall on a clock that is absent
    return published != s.lastViewSequence;
}

// One step of the wait for the next view set, between two looks at the view counter.
//
// The presenter signals an event as it publishes a set, which wakes this thread within the
// scheduler's own latency instead of at the next millisecond boundary. The step is capped at a
// millisecond all the same: with a presenter that never signals -- one built before the event
// existed -- or with no event at all, the wait is then the one-millisecond poll, and no state of
// the event can hold a frame longer than that poll would.
//
// A signal can be left over from a set that was published while the last frame was being drawn.
// It ends one step early and the counter says there is nothing new, so the loop comes back here
// with the event spent.
void waitForViews() {
    HANDLE event = presenterViewEvent();
    if (!event) {
        Sleep(1);
        return;
    }
    WaitForSingleObject(event, 1);
}

bool tickWaiting(const State& s) {
    // Placed on this image in flatFramesInstall; without it there is no tick to compare against,
    // and "the sim has nothing new" is the answer that decides whether a frame is drawn at all.
    if (!g_lastLatchedBgCountAt) return false;
    return s.getBGCount() != *reinterpret_cast<volatile const int*>(g_lastLatchedBgCountAt);
}

// THE FRAME CLOCK DECIDES WHICH TICKS A FLAT FRAME DRAWS FROM.
//
// A flat frame is blended at its instant less one tick, which lies between its two latched ticks
// only when the newest of them is due by that instant and the next one is not. The latch takes
// whatever has arrived when the frame starts, and frame starts land a few milliseconds either side
// of the instants the frame clock gives them (measured up to 2.6 ms at 144 Hz). When ticks arrive
// inside that band the two disagree and the target leaves the pair. At a 144 Hz sim, where display
// and sim drift through each other every few seconds, that was 5.4 % of frames clamped high, in
// bursts of 20 to 50 a second; at 36 Hz the band is a tenth of a tick and it was about three a
// second.
//
// So the tick grid and the frame's instant decide:
//   a tick has arrived but is due after the instant     the frame leaves it for the next one and
//                                                       is drawn from the current pair, exactly as
//                                                       a frame between ticks is
//   none has arrived but the next is due by the instant the frame waits for it, up to kDueGraceMs
//                                                       past its grid time and half a refresh in
//                                                       all; a tick later than that is a real
//                                                       stall, and the frame goes on as before
//
// A waiting tick is left at most kMaxDeferrals frames and never past a BG request, which the
// latch is what serves; a grid that puts it more than a tick away is not trusted to leave it.
constexpr double kDueGraceMs = 1.5;
constexpr int    kMaxDeferrals = 2;

enum class ClockLatch { AsBefore, Defer };

ClockLatch latchByFrameClock(State& s, double instant, double refresh) {
    if (!s.haveTick || !g_lastLatchedBgCountAt || s.frequency <= 0) return ClockLatch::AsBefore;
    int    latched = *reinterpret_cast<volatile const int*>(g_lastLatchedBgCountAt);
    double due = 0.0;
    double tick = tickClockPeriod();
    if (tick <= 0.0 || !tickClockArrival(latched + 1, due)) return ClockLatch::AsBefore;

    if (tickWaiting(s)) {
        if (due <= instant || due - instant > tick) return ClockLatch::AsBefore;
        if (s.deferrals >= kMaxDeferrals) return ClockLatch::AsBefore;
        if (s.bgRequested && s.bgRequested()) return ClockLatch::AsBefore;
        ++s.deferrals;
        return ClockLatch::Defer;
    }
    if (due > instant) return ClockLatch::AsBefore;

    long long start = now();
    double    limit = due + kDueGraceMs * static_cast<double>(s.frequency) / 1000.0;
    if (refresh > 0.0) limit = std::min(limit, static_cast<double>(start) + 0.5 * refresh);
    if (static_cast<double>(start) >= limit) return ClockLatch::AsBefore;
    while (!tickWaiting(s) && static_cast<double>(now()) < limit) SwitchToThread();
    return ClockLatch::AsBefore;
}

// The stock frame: gpl_fgFrameSnapshotBegin, which waits for the tick if it has not landed,
// takes the lock and runs the latch. The tick this frame draws begins when that returns, and the
// slices below are measured from there.
void takeStockFrame(State& s) {
    s.begin();
    long long at = now();
    if (s.haveTick) {
        double span = static_cast<double>(at - s.tickStart);
        if (span > 0.0)
            s.tickPeriod = s.tickPeriod > 0.0 ? s.tickPeriod * 0.9 + span * 0.1 : span;
    }
    if (static_cast<unsigned>(s.extras) > s.windowMaxExtra)
        s.windowMaxExtra = static_cast<unsigned>(s.extras);
    s.tickStart = at;
    s.haveTick = true;
    s.extras = 0;
    s.deferrals = 0;
    // A tick frame is a frame for the display too, so the gate counts from here as well.
    s.lastViewSequence = presenterViewSequence();
}

void __cdecl snapshotBeginHook() {
    State& s = g_state;
    ++s.windowFrames;

    // The frame starts here: after its slot under the frame cap, and with its instant on the flat
    // frame clock taken for every frame that may blend, so that no blend is left an earlier
    // frame's instant.
    waitForFrameSlot(s);
    double instant = 0.0;
    double refresh = 0.0;
    bool   clocked = interpolationFlatFrameInstant(instant, refresh);

    if (!extraFramesOn() || !s.getBGCount) {
        takeStockFrame(s);
        return;
    }
    if (nothingPresented()) {
        ++s.windowBackground;
        takeStockFrame(s);
        return;
    }

    // EVERY ITERATION WAITS FOR THE DISPLAY, INCLUDING THE ONE THAT LATCHES.
    //
    // This is the whole of the decoupling, and having it the other way round is what kept the
    // rate above the display's through three attempts to patch around it. Asking "has a tick
    // arrived?" FIRST makes the latching frame ungated, so a 36 Hz simulation puts 36 frames a
    // second on top of the display's 90 and the counter reads 126.
    //
    // There is no reason for it. One loop iteration is one drawn frame whether or not it
    // latches, so the loop runs at the rate frames are wanted and the latch rides on whichever
    // iteration finds a tick waiting. Rendering was decoupled from the simulation precisely so
    // that the simulation's rate would stop appearing in the frame rate.
    //
    // Delaying a latch by up to one display period is free while the display is the faster of
    // the two -- 11.1 ms against 27.8 -- because the latch still takes the newest tick and none
    // can pass unseen. The guard inside the wait is for the case where it is not.
    if (!displayIsReady(s)) {
        long long waitUntil =
            s.tickPeriod > 0.0 ? now() + static_cast<long long>(s.tickPeriod) : 0;
        ++s.windowWaited;
        for (;;) {
            if (displayIsReady(s)) break;
            // A display SLOWER than the simulation would let a tick pass unlatched, and a tick
            // that is never latched is one the blend has no pair for.
            if (waitUntil && now() >= waitUntil) {
                ++s.windowPreempted;
                break;
            }
            waitForViews();
        }
    }
    s.lastViewSequence = presenterViewSequence();

    // Flat mode: the frame's instant decides whether a waiting tick is latched now, and whether
    // to wait for one that is due.
    bool defer = clocked && latchByFrameClock(s, instant, refresh) == ClockLatch::Defer;

    // A tick is waiting, so this is the frame that latches it. gpl_fgFrameSnapshotBegin's own
    // poll returns immediately -- what it waits for has already arrived -- so this costs the
    // latch and nothing else.
    if (!defer && (tickWaiting(s) || !s.haveTick)) {
        if (!s.haveTick) ++s.windowTickless;
        takeStockFrame(s);
        return;
    }
    if (!defer && tickOverdue(s)) {
        ++s.windowOverdue;
        takeStockFrame(s);
        return;
    }

    // The frame between two ticks. gpl_fgFrameSnapshotBegin is skipped whole, so dword_94D714
    // stays clear and gpl_fgFrameSnapshotEnd at the bottom of the loop finds nothing to release
    // -- the latch and its release stay paired. Nothing here takes the BG lock or reads sim
    // state.
    ++s.extras;
    ++s.windowExtra;
    if (interpolationExtraFrame()) ++s.windowBlended;
}

void __cdecl snapshotEndHook() {
    State& s = g_state;

    // Hold the frame open, as another draw of it may come. If the next frame latches instead,
    // gpl_fgFrameSnapshotBegin's own prologue does the release.
    bool more = extraFramesOn() && s.haveTick && interpolationActive();

    // A pending BG request is serviced immediately after this call, and what it services --
    // loads, frees, a car-model swap -- can free what the open frame points at. Stock GPL always
    // has the frame released by then, so give it that.
    if (more && s.bgRequested && s.bgRequested()) {
        ++s.windowReleased;
        more = false;
    }

    if (more) {
        ++s.windowHeld;
        return;
    }
    s.end();
}

}  // namespace

void flatFramesInstall() {
    State& s = g_state;
    if (s.installed) return;
    if (g_options.frameRate <= 0) return;

    // frameRate above 0 asks for the blend by itself, so reaching this means the blend's own
    // hooks did not install -- not that anything was configured to turn it off. Without them
    // the frames between the ticks would be duplicates, so the rate is refused with them.
    if (!interpolationActive()) {
        logf("frames: the blend did not install, so frames between the ticks would be "
             "duplicates of the one before -- frameRate = %d is ignored. The interp: line "
             "above says why the hooks refused.",
             g_options.frameRate);
        return;
    }

    g_lastLatchedBgCountAt = rebase(kLastLatchedBgCount);
    if (!g_lastLatchedBgCountAt) {
        logf("frames: gpl_fgLastLatchedBgCount %08X cannot be placed in this image (deltas %s) "
             "-- no frames between the ticks",
             static_cast<unsigned>(kLastLatchedBgCount), rebaseSummary());
        return;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    s.getBGCount = exe ? reinterpret_cast<CountFn>(GetProcAddress(exe, "?getBGCount@@YAHXZ"))
                       : nullptr;
    if (!s.getBGCount) {
        logf("frames: the exe does not export getBGCount -- no frames between the ticks");
        return;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;

    s.bgRequested = exe ? reinterpret_cast<FlagFn>(
                              GetProcAddress(exe, "?bgHasRequestedFGAction@@YA_NXZ"))
                        : nullptr;
    if (!s.bgRequested) {
        logf("frames: the exe does not export bgHasRequestedFGAction, so a held frame could not be "
             "released before the game services a BG request -- no frames between the ticks");
        return;
    }

    // Both calls or neither: holding the frame open without skipping Begin would draw the static
    // screen, and skipping Begin without holding the frame open would do the same.
    //
    // The profiler's markers own both of these calls when profileWorkWait is on, which
    // profileStages implies in flat mode too. Go in BEHIND them through the slots they jump
    // through, so each marker still runs on every frame and an extra frame reads as a span of
    // nearly nothing. Only when no marker is there is the call site itself patched.
    void** beginSlot = renderProfileSnapshotBeginSlot();
    void** endSlot = renderProfileSnapshotEndSlot();
    bool   marked = beginSlot && endSlot;
    if (marked) {
        if (*beginSlot != reinterpret_cast<void*>(kSnapshotBegin) ||
            *endSlot != reinterpret_cast<void*>(kSnapshotEnd)) {
            logf("frames: the profiler's snapshot slots hold %08X/%08X, not "
                 "gpl_fgFrameSnapshotBegin/End %08X/%08X -- no frames between the ticks",
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(*beginSlot)),
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(*endSlot)),
                 static_cast<unsigned>(kSnapshotBegin), static_cast<unsigned>(kSnapshotEnd));
            return;
        }
        s.begin = reinterpret_cast<BeginFn>(*beginSlot);
        s.end = reinterpret_cast<EndFn>(*endSlot);
        *beginSlot = reinterpret_cast<void*>(&snapshotBeginHook);
        *endSlot = reinterpret_cast<void*>(&snapshotEndHook);
        s.installed = true;
        logf("frames: installed behind the profiler's snapshot markers (they keep call sites "
             "%08X/%08X; gplvr reaches gpl_fgFrameSnapshotBegin/End %08X/%08X from there)",
             static_cast<unsigned>(kSnapshotBeginCall), static_cast<unsigned>(kSnapshotEndCall),
             static_cast<unsigned>(kSnapshotBegin), static_cast<unsigned>(kSnapshotEnd));
    } else if (beginSlot || endSlot) {
        logf("frames: the profiler marks the snapshot %s but not the %s, and gplvr needs both -- "
             "no frames between the ticks",
             beginSlot ? "begin" : "end", beginSlot ? "end" : "begin");
        return;
    } else {
        auto beginBytes = reinterpret_cast<const uint8_t*>(kSnapshotBeginCall);
        auto endBytes = reinterpret_cast<const uint8_t*>(kSnapshotEndCall);
        if (std::memcmp(beginBytes, kSnapshotBeginStock, sizeof(kSnapshotBeginStock)) != 0 ||
            std::memcmp(endBytes, kSnapshotEndStock, sizeof(kSnapshotEndStock)) != 0) {
            logf("frames: %08X/%08X do not hold the stock calls to gpl_fgFrameSnapshotBegin/End "
                 "(%02X %02X %02X %02X %02X / %02X %02X %02X %02X %02X) -- no frames between the "
                 "ticks",
                 static_cast<unsigned>(kSnapshotBeginCall),
                 static_cast<unsigned>(kSnapshotEndCall), beginBytes[0], beginBytes[1],
                 beginBytes[2], beginBytes[3], beginBytes[4], endBytes[0], endBytes[1],
                 endBytes[2], endBytes[3], endBytes[4]);
            return;
        }
        s.begin = reinterpret_cast<BeginFn>(callTarget(kSnapshotBeginCall));
        s.end = reinterpret_cast<EndFn>(callTarget(kSnapshotEndCall));
        if (!redirect(kSnapshotBeginCall, reinterpret_cast<uintptr_t>(&snapshotBeginHook))) {
            logf("frames: could not redirect the frame loop's snapshot begin at %08X -- no frames "
                 "between the ticks",
                 static_cast<unsigned>(kSnapshotBeginCall));
            return;
        }
        if (!redirect(kSnapshotEndCall, reinterpret_cast<uintptr_t>(&snapshotEndHook))) {
            redirect(kSnapshotBeginCall, reinterpret_cast<uintptr_t>(s.begin));
            logf("frames: could not redirect the frame loop's snapshot end at %08X -- no frames "
                 "between the ticks",
                 static_cast<unsigned>(kSnapshotEndCall));
            return;
        }
        s.installed = true;
    }

    logf("frames: the frame loop's wait for the next sim tick is ours (frameRate = %d, %s). "
         "Frames between two ticks redraw the last latched tick with the blend advanced to that "
         "instant, with no limit on how many; the loop goes back to GPL's wait when a tick is a "
         "whole period late or nothing will be presented (unfocused). THE SIMULATION RATE IS "
         "UNTOUCHED -- an extra frame reads no simulation state.",
         g_options.frameRate,
         g_options.frameRate == 1
             ? (g_options.vr ? "one frame per view set the presenter publishes"
                             : "one frame per refresh, which swapInterval does by blocking in "
                               "SwapBuffers")
             : "unlocked: as fast as frames can be drawn");
}

bool flatFramesActive() { return g_state.installed; }

int flatFramesTickFrameIndex() { return g_state.installed ? g_state.extras : 0; }

bool flatFramesFormat(char* out, size_t size, double seconds) {
    State& s = g_state;
    if (!s.installed || !s.windowFrames) return false;

    double period = s.frequency && s.tickPeriod > 0.0
                        ? 1000.0 * s.tickPeriod / static_cast<double>(s.frequency)
                        : 0.0;
    std::snprintf(out, size,
                  "frames: %u frames in %.1f s (%.1f/s), %u of them between ticks (%u moved the "
                  "world, %u drew a repeat) | up to %u per tick against a %.1f ms tick | %u "
                  "waited for the display, %u of those gave up so a tick would not pass "
                  "unlatched | %u for an overdue tick, %u unfocused, %u before the first tick | "
                  "frame held open %u times, released early for a BG request %u | %u waited for "
                  "the frame cap",
                  s.windowFrames, seconds, seconds > 0.0 ? s.windowFrames / seconds : 0.0,
                  s.windowExtra, s.windowBlended, s.windowExtra - s.windowBlended,
                  s.windowMaxExtra, period, s.windowWaited, s.windowPreempted,
                  s.windowOverdue, s.windowBackground, s.windowTickless, s.windowHeld,
                  s.windowReleased, s.windowCapped);

    s.windowFrames = 0;
    s.windowExtra = 0;
    s.windowBlended = 0;
    s.windowWaited = 0;
    s.windowPreempted = 0;
    s.windowOverdue = 0;
    s.windowBackground = 0;
    s.windowTickless = 0;
    s.windowMaxExtra = 0;
    s.windowHeld = 0;
    s.windowReleased = 0;
    s.windowCapped = 0;
    return true;
}

}  // namespace gplvr
