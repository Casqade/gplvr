#include "RenderPacing.h"

#include "Hooks.h"
#include "Log.h"
#include "Pe.h"
#include "TaskRate.h"
#include "presenter/Producer.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

using BoolFn = bool(__cdecl*)();
using CountFn = int(__cdecl*)();

// A published frame and the sim tick it latched, for the ticks between displayed frames.
struct Published {
    int32_t sequence = 0;
    int     ticks = 0;
    bool    tracked = false;
};
constexpr int kPublishedRing = 8;

// One reporting window.
struct Window {
    unsigned trackedFrames = 0;
    unsigned between[3] = {};  // 1, 2, 3+ ticks
    // Where `now + cost` fell inside the display period, in tenths. THE LOAD INDICATOR: this is
    // the input the old rounding quantised, so bins 1, 9 and 10 together are "the phase is on a
    // boundary", which is the condition the integer aim exists to survive. Kept after the fix so
    // a quiet run can be told from a run that was never stressed.
    unsigned phase[10] = {};
};

// THE PUBLISH-SPACING HISTOGRAM IS GONE and the phase distribution beside it came back, because
// the two were removed together and only one of them was spent.
//
// The publish gap was a NEGATIVE, and negatives are the ones that get re-asked: over 42 windows
// the mean gap was 1.00 with nothing beyond 1.25 periods in 38 of them, so no publish ever waits
// for the next display, and the presenter's own log agreed at 5 skipped displays in 277 windows,
// all at startup. Frame delivery is not a source of the jolt.
//
// The phase was not spent. It turned out to be the instrument that identified the jolt -- the
// rounding in renderPacingDisplayTimeAt flips slot exactly when the phase sits on a period
// boundary, and the phase moves there when render cost rises. What killed the earlier reading of
// it was the question asked of it: "do publishes fall near an edge" (they did not, 12 of 17
// windows) is not "does the TARGET's input fall near an edge" (it did, whenever a mirror was on).

struct State {
    // bgIsRunning is never called. It is how the tick wait is IDENTIFIED: the one function
    // that calls both it and fgDisableBG.
    BoolFn    bgIsRunning = nullptr;
    CountFn   getBGCount = nullptr;
    uintptr_t fgDisableBG = 0;
    uintptr_t site = 0;
    bool      installed = false;
    long long frequency = 0;

    // This frame, taken at the latch.
    long long latchQpc = 0;    // when the tick wait ended and the latch began
    int       latchTicks = 0;  // getBGCount then
    long long publishQpc = 0;      // the frame's first publish after its latch

    // Latch to publish, tracked frames only, in QPC ticks: a smoothed mean and a smoothed
    // absolute deviation, the estimate being mean + 2 deviations. The display-time model
    // needs it, and the latch is the only place it can be measured from.
    double costMean = 0.0;
    double costDeviation = 0.0;
    bool   haveCost = false;

    // The sim tick clock interpolation reads: tick `count` arrived at tickBase + count * T.
    // Each getBGCount reading places the base one latch delay late, and the base is the smoothed
    // average of those rather than a bound on them -- see noteTickCount. What the blend needs
    // from this grid is that it holds still, not that it is exact.
    double    tickBase = 0.0;
    bool      haveTickBase = false;
    int       tickBaseHz = 0;
    int       tickLastCount = 0;
    long long tickLastChange = 0;

    Published ring[kPublishedRing];
    int       ringNext = 0;
    int32_t   lastPublished = 0;
    int32_t   lastConsumed = 0;


    Window window;
};

State g_state;

double msFromTicks(double ticks) {
    return 1000.0 * ticks / static_cast<double>(g_state.frequency);
}

// A getBGCount reading for the tick clock. The upper bound (the tick has arrived) always
// applies; the lower bound (the next one has not) only while ticks are arriving, or a stopped
// sim would drag the base later with every reading.
void noteTickCount(State& s, long long qpc, int count) {
    int simHz = taskRateSimHz();
    if (simHz <= 0 || s.frequency <= 0) return;
    double period = static_cast<double>(s.frequency) / simHz;
    double latest = static_cast<double>(qpc) - count * period;
    if (!s.haveTickBase || simHz != s.tickBaseHz || count < s.tickLastCount ||
        count - s.tickLastCount > 4 * simHz) {
        s.tickBase = latest;
        s.haveTickBase = true;
        s.tickBaseHz = simHz;
        s.tickLastCount = count;
        s.tickLastChange = qpc;
        return;
    }
    if (count != s.tickLastCount) {
        s.tickLastCount = count;
        s.tickLastChange = qpc;
    }

    // FITTED, NOT BOUNDED. This was a bound -- pull the base down to the earliest reading, then
    // clamp it back to within a period of the newest -- which is the right shape for a
    // scheduler that must never claim a tick arrived before it did, and the wrong one for
    // interpolation.
    //
    // The blend divides by this grid. Every millisecond the base MOVES is a millisecond of world
    // motion applied to every car and to the view at once, and it moves once per tick, so a
    // wandering base is a step at the simulation's rate laid over otherwise smooth motion --
    // which is exactly "not 36 and not 90, somewhere in between". A bound tracks the extremes of
    // the latch delay; an average tracks its middle and then stays there. A constant offset in
    // this grid is invisible, because it shifts every object equally and the blend only ever
    // uses differences.
    //
    // The readings got noisier when the frame loop began waiting for the display before
    // latching: the delay from a tick arriving to its latch running is now up to a display
    // period rather than a Sleep(1) granule.
    s.tickBase += 0.02 * (latest - s.tickBase);
}

Published* findPublished(State& s, int32_t sequence) {
    for (Published& entry : s.ring)
        if (entry.sequence == sequence) return &entry;
    return nullptr;
}

// A new frame reached the presenter: how many sim ticks newer it is than the last one it drew.
void noteConsumed(State& s) {
    int32_t consumed = presenterConsumedSequence();
    if (!consumed || consumed == s.lastConsumed) return;
    Published* now = findPublished(s, consumed);
    Published* before = s.lastConsumed ? findPublished(s, s.lastConsumed) : nullptr;
    if (now && before && now->tracked && before->tracked) {
        int ticks = now->ticks - before->ticks;
        if (ticks >= 1) ++s.window.between[ticks >= 3 ? 2 : ticks - 1];
    }
    s.lastConsumed = consumed;
}

// ANSWERING THE DISPLAY TIME FROM AN INTEGER VIEW-SEQUENCE LAG WAS TRIED AND REVERTED.
// Do not rebuild it.
//
// It replaced the rounding below with `the newest published view set's own display time, plus
// (presenterViewSequence() - presenterAcquiredViewSequence() + 1) periods`, on the reasoning that an
// integer from two counters cannot sit on a boundary the way a rounded timestamp can.
//
// It engaged perfectly -- every frame on the integer path, no fallback -- and it did not help,
// because the integer sits on a boundary of its own. The lag is a race between the render finishing
// and the presenter's next publish, so when render cost approaches one display period it becomes a
// coin flip: measured `aimed past its views 1:86 2:5` at 8.8-11.3 ms of an 11.11 ms period, and each
// flip moves the target a WHOLE period. `0.07 to 22.34 ms` in the drawn-instant step with no clamp,
// no hold and no rebase in that window is the flip and nothing else. Below about 8 ms of cost it was
// a clean single column for 190 seconds -- which is exactly where the rounding was also fine.
//
// The phase histogram it brought with it stays, because it is what identified the boundary in the
// first place and it reads on either arithmetic.

// WHERE THE TICK WAIT ENDED, which is the moment this file exists to be told about.

void __stdcall paceBeforeLatch() {
    State& s = g_state;
    LARGE_INTEGER entry;
    QueryPerformanceCounter(&entry);
    long long now = entry.QuadPart;

    s.publishQpc = 0;
    s.latchQpc = now;
    s.latchTicks = s.getBGCount();
    noteTickCount(s, now, s.latchTicks);
    noteConsumed(s);

    // The presenter locates its next views for the display the NEXT frame reaches, and with
    // no pace there is no prediction to make about which one that is: it is told nothing,
    // which is what it was told on every frame the pace declined to act on.
    presenterSetViewLead(0);
}

using PaceFn = void(__stdcall*)();
PaceFn g_paceFn = &paceBeforeLatch;
void*  g_paceNext = nullptr;

// The same shape as RenderProfile's marker thunks: every register and flag goes back as the
// caller left it, then the original call target runs with its return address untouched.
__declspec(naked) void paceThunk() {
    __asm { pushad
            pushfd
            call dword ptr [g_paceFn]
            popfd
            popad
            jmp  dword ptr [g_paceNext] }
}

}  // namespace

void renderPacingDiscover(const Binding& binding) {
    State& s = g_state;
    if (!g_options.vr) return;

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return;
    s.bgIsRunning = reinterpret_cast<BoolFn>(GetProcAddress(exe, "?bgIsRunning@@YA_NXZ"));
    s.getBGCount = reinterpret_cast<CountFn>(GetProcAddress(exe, "?getBGCount@@YAHXZ"));
    s.fgDisableBG = reinterpret_cast<uintptr_t>(GetProcAddress(exe, "?fgDisableBG@@YAXXZ"));
    if (!s.bgIsRunning || !s.getBGCount || !s.fgDisableBG) {
        logf("pace: the exe does not export bgIsRunning/getBGCount/fgDisableBG -- no sim tick "
             "clock, so interpolation falls back to the wall clock");
        return;
    }

    const ModuleImage&     image = binding.image;
    std::vector<uintptr_t> entries = collectCallTargets(image);
    std::vector<uintptr_t> runSites =
        findCallSites(image, reinterpret_cast<uintptr_t>(s.bgIsRunning));
    std::vector<uintptr_t> disableSites = findCallSites(image, s.fgDisableBG);

    // The tick wait is the one function that calls both bgIsRunning and fgDisableBG, and it
    // holds one fgDisableBG call: the same cardinality checks RenderProfile makes.
    std::vector<uintptr_t> owners;
    for (uintptr_t site : runSites) {
        uintptr_t owner = functionOf(entries, site);
        bool      seen = !owner;
        for (uintptr_t known : owners) seen = seen || known == owner;
        if (!seen) owners.push_back(owner);
    }
    int       matches = 0;
    size_t    inside = 0;
    uintptr_t found = 0;
    for (uintptr_t owner : owners) {
        uintptr_t end = functionEnd(entries, image, owner);
        size_t    count = 0;
        uintptr_t last = 0;
        for (uintptr_t site : disableSites)
            if (site >= owner && site < end) {
                ++count;
                last = site;
            }
        if (!count) continue;
        ++matches;
        inside = count;
        found = last;
    }
    if (matches != 1 || inside != 1) {
        logf("pace: %d functions call both bgIsRunning and fgDisableBG (%zu fgDisableBG calls in "
             "the last) -- the tick wait is not identifiable, no sim tick clock",
             matches, inside);
        return;
    }
    s.site = found;
}

void renderPacingInstall() {
    State& s = g_state;
    if (!s.site) return;

    auto call = reinterpret_cast<const uint8_t*>(s.site);
    if (call[0] != 0xE8) {
        logf("pace: the tick wait's call at %08X is no longer a near call -- no sim tick clock",
             static_cast<unsigned>(s.site));
        return;
    }
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, sizeof(rel));
    uintptr_t current = s.site + 5 + rel;

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;

    g_paceNext = reinterpret_cast<void*>(current);
    int32_t mine = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&paceThunk) - (s.site + 5));
    if (!patchBytes(reinterpret_cast<void*>(s.site + 1), &mine, sizeof(mine))) {
        logf("pace: could not redirect the tick wait's call at %08X -- no sim tick clock",
             static_cast<unsigned>(s.site));
        return;
    }
    s.installed = true;
    logf("pace: the tick wait's fgDisableBG call at %08X goes through gplvr, then %s. This is "
         "where the sim tick clock is read and where a frame's latch is timed from; the blend "
         "divides by that clock, so without this line interpolation is working from the wall "
         "clock instead.",
         static_cast<unsigned>(s.site),
         current == s.fgDisableBG ? "straight to fgDisableBG" : "to the profiler's tick marker");
}

void renderPacingNotePublished() {
    State& s = g_state;
    if (!s.installed) return;
    if (s.publishQpc) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s.publishQpc = now.QuadPart;
}


void renderPacingEndFrame(bool trackedStereo) {
    State& s = g_state;
    if (!s.installed) return;

    int32_t sequence = presenterPublishedSequence();
    if (sequence && sequence != s.lastPublished) {
        Published& entry = s.ring[s.ringNext++ % kPublishedRing];
        entry.sequence = sequence;
        entry.ticks = s.latchTicks;
        entry.tracked = trackedStereo;
        s.lastPublished = sequence;
    }

    if (trackedStereo && s.publishQpc > s.latchQpc && s.latchQpc) {
        double sample = static_cast<double>(s.publishQpc - s.latchQpc);
        if (!s.haveCost) {
            s.costMean = sample;
            s.costDeviation = 0.0;
            s.haveCost = true;
        } else {
            double deviation = std::fabs(sample - s.costMean);
            s.costMean = s.costMean * 0.9 + sample * 0.1;
            s.costDeviation = s.costDeviation * 0.9 + deviation * 0.1;
        }
    }
    if (trackedStereo) ++s.window.trackedFrames;
    noteConsumed(s);
}

uintptr_t renderPacingTickWaitSite() { return g_state.installed ? g_state.site : 0; }

void renderPacingObserveTick(long long qpc, int count) {
    if (g_state.installed) noteTickCount(g_state, qpc, count);
}

bool renderPacingTickArrival(int count, double& qpc) {
    State& s = g_state;
    if (!s.haveTickBase || s.tickBaseHz <= 0 || s.tickBaseHz != taskRateSimHz()) return false;
    qpc = s.tickBase + count * (static_cast<double>(s.frequency) / s.tickBaseHz);
    return true;
}

bool renderPacingNextTickDue(double& qpc) {
    State& s = g_state;
    if (!s.haveTickBase || s.tickBaseHz <= 0 || s.tickBaseHz != taskRateSimHz()) return false;
    if (!s.tickLastChange) return false;

    // From the LAST TICK'S OWN ARRIVAL, not from `tickBase`.
    //
    // tickBase is a lower bound -- it moves only as far as a reading proves it wrong, and the
    // clamp below it allows the whole base to sit a full period early. Interpolation wants that
    // (a target that never runs ahead of the state it blends); a deadline does not. Asked
    // through tickBase, "when is the next tick due" answered with the CURRENT tick's arrival,
    // which is already fifteen milliseconds in the past by the time a frame has been drawn and
    // presented -- so the extra frames read "no room" on every single frame and were short by
    // an amount that never even got recorded, because they failed the deadline-in-the-past test
    // instead of the budget one.
    qpc = static_cast<double>(s.tickLastChange) +
          static_cast<double>(s.frequency) / s.tickBaseHz;
    return true;
}

bool renderPacingDisplayTimeAt(long long anchorQpc, long long now, long long& display,
                               bool& fromPresenter) {
    State& s = g_state;
    if (!s.installed) return false;
    long long latch = anchorQpc ? anchorQpc : now;
    long long cost = s.haveCost ? static_cast<long long>(s.costMean) : 0;

    PresenterClock clock;
    bool haveClock = presenterClock(clock) && clock.period > 0 &&
                     now - clock.latchTime <= 3 * clock.period;

    // The load indicator, noted wherever the clock can be read at all, so it keeps reporting on a
    // frame the integer path answers. This is the phase the old rounding tripped over.
    if (haveClock) {
        long long into = (latch + cost - clock.latchTime) % clock.period;
        if (into < 0) into += clock.period;
        int bin = static_cast<int>(into * 10 / clock.period);
        if (bin < 0) bin = 0;
        if (bin > 9) bin = 9;
        ++s.window.phase[bin];
    }

    if (haveClock) {
        // A presenter latch at latchTime + k * period is displayed this long after it.
        long long toDisplay =
            clock.displayTime - clock.viewLead * clock.period - clock.latchTime;
        // The first presenter latch this frame can make, given how long its latch has
        // historically taken to reach a publish.
        long long k = (latch + cost - clock.latchTime + clock.period - 1) / clock.period;
        if (k < 1) k = 1;
        long long presenterLatch = clock.latchTime + k * clock.period;
        display = presenterLatch + toDisplay;
        fromPresenter = true;
        return true;
    }
    display = latch + cost;
    fromPresenter = false;
    return true;
}

// What is left to say once a second: how far apart in sim ticks the frames the presenter
// actually showed were, how long a latch takes to reach a publish, and the two numbers that say
// whether the target is being built the stable way.
//
// The tick spread is the number that matters. Even ticks between displayed frames is what the
// pacing wait existed to produce and what FlatFrames produces instead, so a run of 2s and 3s
// here is the symptom that would send anyone back to the frame loop.
//
// `phase` is where `now + cost` falls inside the display period, and the display time is rounded up
// from exactly that, so bins 1, 9 and 10 busy is the rounding sitting on a boundary and flipping
// slot between consecutive frames. It is driven by render cost: 4-6 ms puts the phase mid-interval
// and 9 ms or more puts it on the edge, which is why a scene light enough to lower the cost stops
// the world stepping. This is the load indicator for anything that reads the display time.
bool renderPacingFormat(char* out, size_t size) {
    State& s = g_state;
    if (!s.installed) return false;
    Window& w = s.window;
    bool    report = w.trackedFrames > 0;
    if (report) {
        std::snprintf(out, size,
                      "pace: %u tracked frames | ticks between displayed frames 1:%u 2:%u 3+:%u "
                      "| latch to publish %.2f ms, estimate %.2f ms "
                      "| phase in tenths of the display %u %u %u %u %u %u %u %u %u %u",
                      w.trackedFrames, w.between[0], w.between[1], w.between[2],
                      s.haveCost ? msFromTicks(s.costMean) : 0.0,
                      s.haveCost ? msFromTicks(s.costMean + 2.0 * s.costDeviation) : 0.0,
                      w.phase[0], w.phase[1], w.phase[2], w.phase[3], w.phase[4], w.phase[5],
                      w.phase[6], w.phase[7], w.phase[8], w.phase[9]);
    }
    w = Window();
    return report;
}

}  // namespace gplvr
