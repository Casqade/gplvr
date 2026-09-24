#include "FlatFrames.h"

#include "Hooks.h"
#include "Interpolation.h"
#include "Log.h"
#include "RenderProfile.h"

#include <windows.h>

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

// The hard ceiling on extra frames per tick, whatever the option says. At 36 Hz seven extra
// frames is 288 Hz.
constexpr int kMaxExtra = 7;

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
    long long tickStart = 0;   // when the tick now being drawn was latched
    double    tickPeriod = 0.0;
    bool      haveTick = false;
    int       extras = 0;      // extra frames already drawn against this tick

    // The window since the last report.
    unsigned windowFrames = 0;
    unsigned windowExtra = 0;
    unsigned windowBlended = 0;   // extra frames that actually moved the world
    unsigned windowWaited = 0;    // extra frames that had to wait for their slice
    unsigned windowPreempted = 0; // of those, the ones a real tick landed on first
    unsigned windowExhausted = 0; // refused because the tick's allowance was spent
    unsigned windowTickless = 0;  // refused because no tick has been latched yet
    unsigned windowMaxExtra = 0;  // the most drawn against any one tick in the window
    unsigned windowHeld = 0;      // ends deferred so the frame survived into an extra draw
    unsigned windowReleased = 0;  // ends let through because a BG request was waiting
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

int extraLimit() {
    int limit = g_options.flatExtraFrames;
    if (limit > kMaxExtra) limit = kMaxExtra;
    return limit;
}

// Extra frame k of a tick belongs at tickStart + k * period / (limit + 1).
//
// Spacing them is the whole point. Drawing the allowance back to back leaves the rest of the
// tick in the Sleep(1), which shows the same stepping the extra frames exist to remove -- the
// frames arrive evenly only if they are started evenly. A frame that is merely LATE is drawn at
// once; there is no catching up to do and no time to spare.
bool sliceHasCome(const State& s, long long at, int limit) {
    if (s.tickPeriod <= 0.0) return true;
    double slice = s.tickPeriod / (limit + 1);
    return static_cast<double>(at - s.tickStart) >= (s.extras + 1) * slice;
}

bool tickWaiting(const State& s) {
    return s.getBGCount() != *reinterpret_cast<volatile const int*>(kLastLatchedBgCount);
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
}

void __cdecl snapshotBeginHook() {
    State& s = g_state;
    int    limit = extraLimit();
    ++s.windowFrames;

    if (!limit || !s.getBGCount || tickWaiting(s)) {
        takeStockFrame(s);
        return;
    }
    if (!s.haveTick) {
        ++s.windowTickless;
        takeStockFrame(s);
        return;
    }
    if (s.extras >= limit) {
        // The allowance is spent; the stock path sleeps out the rest of the tick as GPL does.
        ++s.windowExhausted;
        takeStockFrame(s);
        return;
    }

    // Before its slice. Wait for it rather than letting the stock path sleep away the whole
    // remainder of the tick, which would drop this frame instead of merely delaying it. A tick
    // landing meanwhile ends the wait: the real frame always wins.
    if (!sliceHasCome(s, now(), limit)) {
        ++s.windowWaited;
        for (;;) {
            if (tickWaiting(s)) {
                ++s.windowPreempted;
                takeStockFrame(s);
                return;
            }
            if (sliceHasCome(s, now(), limit)) break;
            Sleep(1);
        }
    }

    // The extra frame. gpl_fgFrameSnapshotBegin is skipped whole, so dword_94D714 stays clear
    // and gpl_fgFrameSnapshotEnd at the bottom of the loop finds nothing to release -- the latch
    // and its release stay paired. Nothing below takes the BG lock or reads sim state.
    ++s.extras;
    ++s.windowExtra;
    if (interpolationExtraFrame()) ++s.windowBlended;
}

void __cdecl snapshotEndHook() {
    State& s = g_state;
    int    limit = extraLimit();

    // Hold the frame open only while another draw of it is still to come. When the allowance is
    // spent the release happens exactly where GPL puts it.
    bool more = limit > 0 && s.haveTick && s.extras < limit && interpolationActive();

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
    if (g_options.vr || g_options.flatExtraFrames <= 0) return;

    if (!interpolationActive()) {
        logf("flat: the blend is not running, so frames between the ticks would be duplicates "
             "-- flatExtraFrames is ignored (flatInterpolate=%d)",
             g_options.flatInterpolate ? 1 : 0);
        return;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    s.getBGCount = exe ? reinterpret_cast<CountFn>(GetProcAddress(exe, "?getBGCount@@YAHXZ"))
                       : nullptr;
    if (!s.getBGCount) {
        logf("flat: the exe does not export getBGCount -- no frames between the ticks");
        return;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;

    s.bgRequested = exe ? reinterpret_cast<FlagFn>(
                              GetProcAddress(exe, "?bgHasRequestedFGAction@@YA_NXZ"))
                        : nullptr;
    if (!s.bgRequested) {
        logf("flat: the exe does not export bgHasRequestedFGAction, so a held frame could not be "
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
            logf("flat: the profiler's snapshot slots hold %08X/%08X, not "
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
        logf("flat: installed behind the profiler's snapshot markers (they keep call sites "
             "%08X/%08X; gplvr reaches gpl_fgFrameSnapshotBegin/End %08X/%08X from there)",
             static_cast<unsigned>(kSnapshotBeginCall), static_cast<unsigned>(kSnapshotEndCall),
             static_cast<unsigned>(kSnapshotBegin), static_cast<unsigned>(kSnapshotEnd));
    } else if (beginSlot || endSlot) {
        logf("flat: the profiler marks the snapshot %s but not the %s, and gplvr needs both -- "
             "no frames between the ticks",
             beginSlot ? "begin" : "end", beginSlot ? "end" : "begin");
        return;
    } else {
        auto beginBytes = reinterpret_cast<const uint8_t*>(kSnapshotBeginCall);
        auto endBytes = reinterpret_cast<const uint8_t*>(kSnapshotEndCall);
        if (std::memcmp(beginBytes, kSnapshotBeginStock, sizeof(kSnapshotBeginStock)) != 0 ||
            std::memcmp(endBytes, kSnapshotEndStock, sizeof(kSnapshotEndStock)) != 0) {
            logf("flat: %08X/%08X do not hold the stock calls to gpl_fgFrameSnapshotBegin/End "
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
            logf("flat: could not redirect the frame loop's snapshot begin at %08X -- no frames "
                 "between the ticks",
                 static_cast<unsigned>(kSnapshotBeginCall));
            return;
        }
        if (!redirect(kSnapshotEndCall, reinterpret_cast<uintptr_t>(&snapshotEndHook))) {
            redirect(kSnapshotBeginCall, reinterpret_cast<uintptr_t>(s.begin));
            logf("flat: could not redirect the frame loop's snapshot end at %08X -- no frames "
                 "between the ticks",
                 static_cast<unsigned>(kSnapshotEndCall));
            return;
        }
        s.installed = true;
    }

    logf("flat: frames between the ticks ON -- up to %d extra per sim tick, spaced evenly across "
         "it, each one redrawing the last latched tick with the blend advanced to that instant. "
         "The simulation rate is untouched; only the drawn world moves faster. flatInterpolate=%d",
         extraLimit(), g_options.flatInterpolate ? 1 : 0);
}

bool flatFramesFormat(char* out, size_t size, double seconds) {
    State& s = g_state;
    if (!s.installed || !s.windowFrames) return false;

    double period = s.frequency && s.tickPeriod > 0.0
                        ? 1000.0 * s.tickPeriod / static_cast<double>(s.frequency)
                        : 0.0;
    std::snprintf(out, size,
                  "flat: %u frames in %.1f s (%.1f/s), %u of them between ticks (%u moved the "
                  "world, %u drew a repeat) | up to %u per tick against a %.1f ms tick | %u "
                  "waited for their slice, %u of those lost it to a real tick | %u past the "
                  "allowance, %u before the first tick | frame held open %u times, released "
                  "early for a BG request %u",
                  s.windowFrames, seconds, seconds > 0.0 ? s.windowFrames / seconds : 0.0,
                  s.windowExtra, s.windowBlended, s.windowExtra - s.windowBlended,
                  s.windowMaxExtra, period, s.windowWaited, s.windowPreempted, s.windowExhausted,
                  s.windowTickless, s.windowHeld, s.windowReleased);

    s.windowFrames = 0;
    s.windowExtra = 0;
    s.windowBlended = 0;
    s.windowWaited = 0;
    s.windowPreempted = 0;
    s.windowExhausted = 0;
    s.windowTickless = 0;
    s.windowMaxExtra = 0;
    s.windowHeld = 0;
    s.windowReleased = 0;
    return true;
}

}  // namespace gplvr
