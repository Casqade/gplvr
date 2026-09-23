#include "RenderPacing.h"

#include "FrameBudget.h"
#include "Hooks.h"
#include "Interpolation.h"
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

// Why a frame was not paced, counted per frame and named on the pacing line.
enum Unpaced { kUnpacedOff, kUnpacedMenu, kUnpacedClock, kUnpacedStale, kUnpacedRatio,
               kUnpacedCount };

const char* const kUnpacedName[kUnpacedCount] = {"off", "menu", "no clock", "stale", "ratio"};

const char* const kUnpacedWhy[kUnpacedCount] = {
    "vrLockToHeadset = 0",
    "the previous frame was not a tracked stereo world frame",
    "the presenter is not publishing its display clock",
    "the presenter's newest latch is more than 3 display periods old",
    "the display period is not a whole number of sim ticks and vrInterpolate is off"};

// Where the presenter's display period came from, named on the pace: change lines.
const char* const kSourceQuery = "refresh rate query";
const char* const kSourceRuntime = "runtime frame period";

// How a pacing wait ended.
enum Exit { kExitTarget, kExitBgRequest, kExitBgStopped, kExitTimeout, kExits };

// A published frame and the sim tick it latched, for the ticks between displayed frames.
struct Published {
    int32_t sequence = 0;
    int     ticks = 0;
    bool    tracked = false;
};
constexpr int kPublishedRing = 8;

// Over budget is left only after this many consecutive frames whose cost estimate and margin
// fit in this fraction of a display period.
constexpr int    kBudgetCalmFrames = 10;
constexpr double kBudgetLeave = 0.9;

// One reporting window.
struct Window {
    unsigned trackedFrames = 0;
    unsigned paced = 0;
    unsigned overBudget = 0;
    unsigned unpaced[kUnpacedCount] = {};
    unsigned between[3] = {};  // 1, 2, 3+ ticks
    double   marginSum = 0.0;
    double   marginMin = 0.0;
    unsigned margins = 0;
    unsigned missed = 0;
    double   phaseSum = 0.0;
    double   phaseSquares = 0.0;
    unsigned phases = 0;
    double   edgeSquares = 0.0;
    unsigned edges = 0;
    unsigned slips = 0;
    unsigned exits[kExits] = {};
};

struct State {
    BoolFn    bgIsRunning = nullptr;
    BoolFn    bgHasRequestedFGAction = nullptr;
    CountFn   getBGCount = nullptr;
    uintptr_t fgDisableBG = 0;
    uintptr_t site = 0;
    bool      installed = false;
    long long frequency = 0;

    // This frame, decided at the latch.
    bool      engaged = false;
    long long latchQpc = 0;     // when the wait ended and the latch began
    int       latchTicks = 0;   // getBGCount then
    long long targetLatch = 0;  // the presenter latch a paced frame aims for
    long long publishQpc = 0;   // the frame's first publish after its latch

    bool previousTracked = false;

    // Latch to publish, tracked frames only, in QPC ticks: a smoothed mean and a smoothed
    // absolute deviation, the estimate being mean + 2 deviations.
    double costMean = 0.0;
    double costDeviation = 0.0;
    bool   haveCost = false;

    // The cost estimate and the margin fill a display period: the frame cannot make even the
    // next display, so its latch starts at once. calmFrames counts frames back under
    // kBudgetLeave of a period.
    bool overBudget = false;
    int  calmFrames = 0;
    int  budgetReports = 24;

    // The sim tick grid: one estimated tick edge, refined by every edge a wait observes, and
    // how far the target is moved earlier to sit in the middle of a tick.
    double    tickEdge = 0.0;
    bool      haveTickEdge = false;
    double    pull = 0.0;
    long long lastSlipQpc = 0;
    double    lastSlipSeconds = 0.0;
    bool      coarseSleep = false;

    // The sim tick clock interpolation reads: tick `count` arrived at tickBase + count * T.
    // Every getBGCount reading bounds the base -- tick `count` has arrived, tick count+1 has
    // not -- and the base moves only as far as a reading proves it wrong.
    double    tickBase = 0.0;
    bool      haveTickBase = false;
    int       tickBaseHz = 0;
    int       tickLastCount = 0;
    long long tickLastChange = 0;

    Published ring[kPublishedRing];
    int       ringNext = 0;
    int32_t   lastPublished = 0;
    int32_t   lastConsumed = 0;

    int         loggedState = -2;  // -1 engaged, else an Unpaced
    const char* loggedSource = nullptr;
    int         stateReports = 24;
    bool        loggedRatio = false;

    Window window;
};

State g_state;

double ticksFromMs(double ms) { return ms * static_cast<double>(g_state.frequency) / 1000.0; }

double msFromTicks(double ticks) {
    return 1000.0 * ticks / static_cast<double>(g_state.frequency);
}

// Wrap into [0, period).
double wrap(double value, double period) {
    double r = std::fmod(value, period);
    return r < 0.0 ? r + period : r;
}

// A tick edge seen between two polls. Only polls close together place it well enough.
void noteTickEdge(State& s, long long before, long long after, double tickPeriod) {
    if (after - before > static_cast<long long>(ticksFromMs(2.0))) return;
    double edge = 0.5 * static_cast<double>(before + after);
    if (!s.haveTickEdge) {
        s.tickEdge = edge;
        s.haveTickEdge = true;
        return;
    }
    double residual = wrap(edge - s.tickEdge, tickPeriod);
    if (residual > 0.5 * tickPeriod) residual -= tickPeriod;
    s.tickEdge = edge - 0.95 * residual;
    s.window.edgeSquares += residual * residual;
    ++s.window.edges;
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
    if (s.tickBase > latest) s.tickBase = latest;
    double earliest = latest - period;
    if (static_cast<double>(qpc - s.tickLastChange) < 2.0 * period && s.tickBase < earliest)
        s.tickBase = earliest;
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

void logState(State& s, int state, double ticksPerDisplay, bool integral, double displayHz,
              int simHz, const char* source) {
    if (state == s.loggedState && source == s.loggedSource) return;
    s.loggedState = state;
    s.loggedSource = source;
    if (s.stateReports <= 0) return;
    const char* more = --s.stateReports ? "" : " (further changes not logged)";
    if (state < 0) {
        logf("pace: engaged -- %.2f sim ticks per display (%.2f Hz display, period from the %s; "
             "%d Hz sim)%s; each latch waits for the presenter's clock and the views are located "
             "for the display the frame reaches%s",
             ticksPerDisplay, displayHz, source ? source : kSourceRuntime, simHz,
             integral ? "" : ", interpolation carries the fraction of a tick", more);
    } else if (source) {
        logf("pace: unpaced -- %s (display period from the %s)%s", kUnpacedWhy[state], source,
             more);
    } else {
        logf("pace: unpaced -- %s%s", kUnpacedWhy[state], more);
    }
}

// Enter over budget when the estimate and margin reach a period; leave after kBudgetCalmFrames
// frames in a row at or under kBudgetLeave of it. Logged on change, capped.
void updateBudget(State& s, double estimate, double margin, double period) {
    bool changed = false;
    if (!s.overBudget) {
        if (s.haveCost && estimate + margin >= period) {
            s.overBudget = true;
            s.calmFrames = 0;
            changed = true;
        }
    } else if (estimate + margin <= kBudgetLeave * period) {
        if (++s.calmFrames >= kBudgetCalmFrames) {
            s.overBudget = false;
            s.calmFrames = 0;
            changed = true;
        }
    } else {
        s.calmFrames = 0;
    }
    if (!changed || s.budgetReports <= 0) return;
    const char* more = --s.budgetReports ? "" : " (further changes not logged)";
    if (s.overBudget) {
        logf("pace: over budget -- cost estimate %.2f ms + margin %.2f ms >= period %.2f ms; "
             "frames start at once%s",
             msFromTicks(estimate), msFromTicks(margin), msFromTicks(period), more);
    } else {
        logf("pace: back within budget -- cost estimate %.2f ms + margin %.2f ms <= %.2f ms "
             "(%.1f of the period) for %d frames; frames are paced again%s",
             msFromTicks(estimate), msFromTicks(margin), msFromTicks(kBudgetLeave * period),
             kBudgetLeave, kBudgetCalmFrames, more);
    }
}

// The time a paced frame's latch should start, for a given pull: the first presenter latch
// the frame can still make, less the render cost, the margin and the pull.
long long latchTarget(const PresenterClock& clock, long long now, double lead, double pull,
                      double tolerance, long long& presenterLatch, int& displays) {
    double period = static_cast<double>(clock.period);
    double k = std::ceil((static_cast<double>(now - clock.latchTime) - tolerance + pull + lead) /
                         period);
    if (k < 1.0) k = 1.0;
    presenterLatch = clock.latchTime + static_cast<long long>(k * period);
    displays = static_cast<int>(k);
    return presenterLatch - static_cast<long long>(lead + pull);
}

void __stdcall paceBeforeLatch() {
    State& s = g_state;
    LARGE_INTEGER entry;
    QueryPerformanceCounter(&entry);
    long long now = entry.QuadPart;

    s.engaged = false;
    s.targetLatch = 0;
    s.publishQpc = 0;
    s.latchQpc = now;
    s.latchTicks = s.getBGCount();
    noteTickCount(s, now, s.latchTicks);
    noteConsumed(s);

    PresenterClock clock;
    int            simHz = 0;
    double         ratio = 0.0;
    bool           integral = true;
    const char*    source = nullptr;
    int            why = -1;
    if (!g_options.vrLockToHeadset) {
        why = kUnpacedOff;
    } else if (!s.previousTracked) {
        why = kUnpacedMenu;
    } else if (!presenterClock(clock)) {
        why = kUnpacedClock;
    } else {
        source = clock.refreshRate > 0.0f ? kSourceQuery : kSourceRuntime;
        if (now - clock.latchTime > 3 * clock.period) {
            why = kUnpacedStale;
        } else {
            simHz = taskRateSimHz();
            ratio = static_cast<double>(simHz) * static_cast<double>(clock.period) /
                    static_cast<double>(s.frequency);
            long nearest = std::lround(ratio);
            integral = nearest >= 1 && std::fabs(ratio - nearest) <= 0.01 * nearest;
            // A display that is not a whole number of ticks is still paced when interpolation
            // is on: the frame keeps to the display and the blend covers the fraction.
            if (simHz <= 0 || !(ratio > 0.0) || (!integral && !interpolationActive())) {
                why = kUnpacedRatio;
                if (!s.loggedRatio && simHz > 0) {
                    s.loggedRatio = true;
                    logf("pace: a %.2f Hz display (period from the %s) against a %d Hz sim is "
                         "%.3f ticks per display, not a whole number -- frames stay unpaced "
                         "while vrInterpolate is off (logged once)",
                         static_cast<double>(s.frequency) / static_cast<double>(clock.period),
                         source, simHz, ratio);
                }
            }
        }
    }

    logState(s, why, ratio, integral,
             why < 0 ? static_cast<double>(s.frequency) / static_cast<double>(clock.period) : 0.0,
             simHz, source);
    if (why >= 0) {
        ++s.window.unpaced[why];
        presenterSetViewLead(0);
        return;
    }

    double tickPeriod = static_cast<double>(s.frequency) / simHz;
    double period = static_cast<double>(clock.period);
    double margin = ticksFromMs(g_options.vrPaceMarginMs);
    double estimate = s.haveCost ? s.costMean + 2.0 * s.costDeviation : 0.4 * period;
    updateBudget(s, estimate, margin, period);

    // Over budget the latch starts now: a wait would only push the frame past a later latch.
    // The frame reaches the display of the first presenter latch after its mean cost, the
    // display renderPacingDisplayTime gives interpolation, and the views are located for it.
    // Latch to publish is still measured, so the estimate keeps following the scene.
    if (s.overBudget) {
        double k = std::ceil((static_cast<double>(now - clock.latchTime) + s.costMean) / period);
        if (k < 1.0) k = 1.0;
        presenterSetViewLead(k > 2.0 ? 2 : static_cast<int>(k));
        s.engaged = true;
        ++s.window.overBudget;
        return;
    }

    double cost = estimate;
    if (cost > period) cost = period;
    double lead = cost + margin;
    // A target missed by less than half the margin is still made by starting now.
    double tolerance = 0.5 * margin;

    long long presenterLatch = 0;
    int       displays = 0;
    long long target = latchTarget(clock, now, lead, s.pull, tolerance, presenterLatch, displays);

    // Keep the latch in the middle of a sim tick, where the jitter of tick arrival cannot move
    // it to the neighbouring tick. The display and sim clocks drift, so this re-centres once in
    // a while: that frame is one tick nearer or further from the last, counted as a slip. A
    // display that is not a whole number of ticks crosses tick edges every few frames by
    // construction, so there it is left to interpolation.
    if (!integral && s.pull != 0.0) {
        s.pull = 0.0;
        target = latchTarget(clock, now, lead, s.pull, tolerance, presenterLatch, displays);
    }
    if (s.haveTickEdge && integral) {
        double phase = wrap(static_cast<double>(target) - s.tickEdge, tickPeriod);
        if (phase < 0.25 * tickPeriod || phase > 0.75 * tickPeriod) {
            s.pull = wrap(s.pull + phase - 0.5 * tickPeriod, tickPeriod);
            target = latchTarget(clock, now, lead, s.pull, tolerance, presenterLatch, displays);
            ++s.window.slips;
            if (s.lastSlipQpc)
                s.lastSlipSeconds =
                    static_cast<double>(now - s.lastSlipQpc) / static_cast<double>(s.frequency);
            s.lastSlipQpc = now;
        }
    }

    // The presenter locates the next views for the display this frame's successor reaches.
    presenterSetViewLead(displays > 2 ? 2 : displays);

    Exit      exit = kExitTarget;
    long long exitQpc = now;
    if (target > now) {
        frameBudgetSwitch(kSpanWaitPace);
        int       ticks = s.latchTicks;
        long long lastPoll = now;
        long long limit = now + clock.period;
        long long sleepAbove = static_cast<long long>(ticksFromMs(2.0));
        long long coarse = static_cast<long long>(ticksFromMs(4.0));
        for (;;) {
            LARGE_INTEGER poll;
            QueryPerformanceCounter(&poll);
            exitQpc = poll.QuadPart;
            if (exitQpc >= target) break;
            if (exitQpc >= limit) {
                exit = kExitTimeout;
                break;
            }
            if (s.bgHasRequestedFGAction()) {
                exit = kExitBgRequest;
                break;
            }
            if (!s.bgIsRunning()) {
                exit = kExitBgStopped;
                break;
            }
            int count = s.getBGCount();
            noteTickCount(s, exitQpc, count);
            if (count != ticks) {
                noteTickEdge(s, lastPoll, exitQpc, tickPeriod);
                ticks = count;
            }
            lastPoll = exitQpc;

            if (!s.coarseSleep && target - exitQpc > sleepAbove) {
                Sleep(1);
                LARGE_INTEGER woke;
                QueryPerformanceCounter(&woke);
                if (woke.QuadPart - exitQpc > coarse) {
                    s.coarseSleep = true;
                    logf("pace: Sleep(1) took %.1f ms -- the timer resolution is coarse, so the "
                         "pacing wait yields instead of sleeping from now on",
                         msFromTicks(static_cast<double>(woke.QuadPart - exitQpc)));
                }
            } else {
                SwitchToThread();
            }
        }
        frameBudgetSwitch(kSpanWaitBg);
        s.latchTicks = s.getBGCount();
    }

    s.engaged = true;
    s.latchQpc = exitQpc;
    s.targetLatch = presenterLatch;
    ++s.window.paced;
    ++s.window.exits[exit];
    if (s.haveTickEdge) {
        double phase = msFromTicks(wrap(static_cast<double>(exitQpc) - s.tickEdge, tickPeriod));
        s.window.phaseSum += phase;
        s.window.phaseSquares += phase * phase;
        ++s.window.phases;
    }
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
    s.bgHasRequestedFGAction =
        reinterpret_cast<BoolFn>(GetProcAddress(exe, "?bgHasRequestedFGAction@@YA_NXZ"));
    s.getBGCount = reinterpret_cast<CountFn>(GetProcAddress(exe, "?getBGCount@@YAHXZ"));
    s.fgDisableBG = reinterpret_cast<uintptr_t>(GetProcAddress(exe, "?fgDisableBG@@YAXXZ"));
    if (!s.bgIsRunning || !s.bgHasRequestedFGAction || !s.getBGCount || !s.fgDisableBG) {
        logf("pace: the exe does not export bgIsRunning/bgHasRequestedFGAction/getBGCount/"
             "fgDisableBG -- no render pacing");
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
             "the last) -- the tick wait is not identifiable, no render pacing",
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
        logf("pace: the tick wait's call at %08X is no longer a near call -- no render pacing",
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
        logf("pace: could not redirect the tick wait's call at %08X -- no render pacing",
             static_cast<unsigned>(s.site));
        return;
    }
    s.installed = true;
    logf("pace: the tick wait's fgDisableBG call at %08X goes through gplvr, then %s. %s",
         static_cast<unsigned>(s.site),
         current == s.fgDisableBG ? "straight to fgDisableBG" : "to the profiler's tick marker",
         g_options.vrLockToHeadset
             ? "Frames are paced whenever the conditions on the pace: lines hold."
             : "vrLockToHeadset = 0: frames run unpaced and the pace: line is the baseline.");
}

void renderPacingNotePublished() {
    State& s = g_state;
    if (!s.installed || s.publishQpc) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s.publishQpc = now.QuadPart;
    if (!s.targetLatch) return;

    double margin = msFromTicks(static_cast<double>(s.targetLatch - s.publishQpc));
    Window& w = s.window;
    if (!w.margins || margin < w.marginMin) w.marginMin = margin;
    w.marginSum += margin;
    ++w.margins;
    if (margin < 0.0) ++w.missed;
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
    s.previousTracked = trackedStereo;
    noteConsumed(s);
}

bool renderPacingEngaged() { return g_state.engaged; }

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

bool renderPacingDisplayTime(long long now, long long& display, bool& fromPresenter) {
    State& s = g_state;
    if (!s.installed) return false;
    long long latch = s.latchQpc ? s.latchQpc : now;
    long long cost = s.haveCost ? static_cast<long long>(s.costMean) : 0;

    PresenterClock clock;
    if (presenterClock(clock) && now - clock.latchTime <= 3 * clock.period) {
        // A presenter latch at latchTime + k * period is displayed this long after it.
        long long toDisplay =
            clock.displayTime - clock.viewLead * clock.period - clock.latchTime;
        long long presenterLatch = s.engaged ? s.targetLatch : 0;
        if (!presenterLatch) {
            long long k = (latch + cost - clock.latchTime + clock.period - 1) / clock.period;
            if (k < 1) k = 1;
            presenterLatch = clock.latchTime + k * clock.period;
        }
        display = presenterLatch + toDisplay;
        fromPresenter = true;
        return true;
    }
    display = latch + cost;
    fromPresenter = false;
    return true;
}

bool renderPacingFormat(char* out, size_t size) {
    State& s = g_state;
    if (!s.installed) return false;
    Window& w = s.window;
    bool    report = w.trackedFrames > 0;
    if (report) {
        double phaseMean = w.phases ? w.phaseSum / w.phases : 0.0;
        double phaseVariance = w.phases ? w.phaseSquares / w.phases - phaseMean * phaseMean : 0.0;
        int    simHz = taskRateSimHz();
        char   slip[48] = {};
        if (w.slips && s.lastSlipSeconds > 0.0)
            std::snprintf(slip, sizeof(slip), " (last two %.1f s apart)", s.lastSlipSeconds);
        std::snprintf(
            out, size,
            "pace: %u paced, over budget %u, unpaced %s %u / %s %u / %s %u / %s %u / %s %u | "
            "ticks between displayed frames 1:%u 2:%u 3+:%u | publish margin before latch mean "
            "%.2f min %.2f ms, %u missed | tick phase at latch mean %.2f sd %.2f of %.2f ms, "
            "edge residual %.2f ms | slips %u%s | wait exits target %u bgRequest %u bgStopped "
            "%u timeout %u | cost estimate %.2f ms",
            w.paced, w.overBudget, kUnpacedName[kUnpacedOff], w.unpaced[kUnpacedOff], kUnpacedName[kUnpacedMenu],
            w.unpaced[kUnpacedMenu], kUnpacedName[kUnpacedClock], w.unpaced[kUnpacedClock],
            kUnpacedName[kUnpacedStale], w.unpaced[kUnpacedStale], kUnpacedName[kUnpacedRatio],
            w.unpaced[kUnpacedRatio], w.between[0], w.between[1], w.between[2],
            w.margins ? w.marginSum / w.margins : 0.0, w.margins ? w.marginMin : 0.0, w.missed,
            phaseMean, phaseVariance > 0.0 ? std::sqrt(phaseVariance) : 0.0,
            simHz > 0 ? 1000.0 / simHz : 0.0,
            w.edges ? msFromTicks(std::sqrt(w.edgeSquares / w.edges)) : 0.0, w.slips, slip,
            w.exits[kExitTarget], w.exits[kExitBgRequest], w.exits[kExitBgStopped],
            w.exits[kExitTimeout],
            s.haveCost ? msFromTicks(s.costMean + 2.0 * s.costDeviation) : 0.0);
    }
    w = Window();
    return report;
}

}  // namespace gplvr
