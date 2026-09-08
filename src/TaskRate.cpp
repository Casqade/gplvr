#include "TaskRate.h"

#include "Log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

// The Papyrus task API, straight out of gplc67.exe's export table. The mangled names are
// the contract: `?name@@YA<sig>` is MSVC's C++ decoration and it encodes the signature,
// so if a build ever changed one of these the lookup fails loudly instead of calling
// something with the wrong shape.
struct Api {
    int (__cdecl* taskGetRate)() = nullptr;
    float (__cdecl* taskGetRateAdjust)() = nullptr;
    int (__cdecl* getBGCount)() = nullptr;
    int (__cdecl* getSVCount)() = nullptr;
    int (__cdecl* getIntCount)() = nullptr;
    int (__cdecl* getIgnoredIntCount)() = nullptr;
    bool (__cdecl* inBackground)() = nullptr;
    bool (__cdecl* inForeground)() = nullptr;
    bool (__cdecl* inServer)() = nullptr;

    bool attempted = false;
    bool resolved = false;

    // Window accumulators. Read on the render thread while the tasks write them; these
    // are plain aligned dwords behind accessor functions, monotonic, so a delta across a
    // race is still a true delta.
    // BG duty cycle, accumulated on the BG thread by the bgSleep hook and read on the
    // render thread. These are 64-bit and this is a 32-bit build, so a plain read CAN
    // tear -- half of one value and half of the next -- which would not be a slightly
    // wrong percentage but an absurd one. Snapshot-and-clear goes through
    // InterlockedExchange64. `lastReturn` is touched only by the BG thread and needs
    // nothing.
    void (__cdecl* bgSleep)() = nullptr;
    long long busyTicks = 0;   // time spent OUTSIDE bgSleep
    long long worstTicks = 0;  // worst single stretch
    long long lastReturn = 0;
    bool      dutyHooked = false;

    bool haveBase = false;
    int  baseBg = 0;
    int  baseSv = 0;
    int  baseInt = 0;
    int  baseIgnored = 0;
    int  slowReportsLeft = 8;
};

Api g_api;

template <typename T>
bool resolve(HMODULE module, const char* name, T& out, int& missing) {
    out = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!out) {
        logf("tick: gplc67.exe does not export %s", name);
        ++missing;
        return false;
    }
    return true;
}

// Runs ON THE BG THREAD, once per BG loop iteration (and a few other places that also
// yield -- see the note in the header; the duty cycle is a ratio of totals, so it does not
// care how the calls are distributed).
void __cdecl bgSleepHook() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (g_api.lastReturn) {
        const long long busy = now.QuadPart - g_api.lastReturn;
        if (busy > 0) {
            InterlockedExchangeAdd64(&g_api.busyTicks, busy);
            // Racy against the reader's clear, which can only lose a worst case to the
            // previous window. Not worth a compare-exchange loop on BG's hot path.
            if (busy > g_api.worstTicks) g_api.worstTicks = busy;
        }
    }

    if (g_api.bgSleep) g_api.bgSleep();

    QueryPerformanceCounter(&now);
    g_api.lastReturn = now.QuadPart;
}

}  // namespace

void taskRateBind(const ModuleImage* image) {
    if (g_api.attempted) return;
    g_api.attempted = true;

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return;

    int missing = 0;
    resolve(exe, "?taskGetRate@@YAHXZ", g_api.taskGetRate, missing);
    resolve(exe, "?taskGetRateAdjust@@YAMXZ", g_api.taskGetRateAdjust, missing);
    resolve(exe, "?getBGCount@@YAHXZ", g_api.getBGCount, missing);
    resolve(exe, "?getSVCount@@YAHXZ", g_api.getSVCount, missing);
    resolve(exe, "?getIntCount@@YAHXZ", g_api.getIntCount, missing);
    resolve(exe, "?getIgnoredIntCount@@YAHXZ", g_api.getIgnoredIntCount, missing);
    resolve(exe, "?inBackground@@YA_NXZ", g_api.inBackground, missing);
    resolve(exe, "?inForeground@@YA_NXZ", g_api.inForeground, missing);
    resolve(exe, "?inServer@@YA_NXZ", g_api.inServer, missing);

    if (missing) {
        logf("tick: %d task exports missing -- tick reporting is off. This is not a "
             "pattern that drifted; the exe genuinely does not have them.",
             missing);
        return;
    }

    g_api.resolved = true;
    logf("tick: task API resolved from the exe's exports -- no byte patterns involved");

    // The duty-cycle hook is optional: without it every other number still works.
    if (!image) return;

    auto sleepFn =
        reinterpret_cast<void(__cdecl*)()>(GetProcAddress(exe, "?bgSleep@@YAXXZ"));
    if (!sleepFn) {
        logf("tick: bgSleep is not exported, so BG duty cycle is unavailable");
        return;
    }
    g_api.bgSleep = sleepFn;

    const std::vector<uintptr_t> sites =
        findCallSites(*image, reinterpret_cast<uintptr_t>(sleepFn));
    if (sites.empty()) {
        logf("tick: found no call sites for bgSleep -- BG duty cycle is unavailable");
        g_api.bgSleep = nullptr;
        return;
    }

    int patched = 0;
    for (uintptr_t site : sites) {
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&bgSleepHook) - (site + 5));
        if (patchBytes(reinterpret_cast<void*>(site + 1), &rel, sizeof(rel))) ++patched;
    }

    if (!patched) {
        logf("tick: could not redirect any of bgSleep's %zu call sites -- BG duty cycle "
             "is unavailable",
             sites.size());
        g_api.bgSleep = nullptr;
        return;
    }

    g_api.dutyHooked = true;
    logf("tick: bgSleep redirected at %d of %zu call sites -- BG duty cycle will be "
         "reported. This measures how much of its slot the simulation actually uses, "
         "which is what decides whether a higher sim rate is possible.",
         patched, sites.size());
}

int taskRateSimHz() {
    if (!g_api.resolved) return 0;
    const int rate = g_api.taskGetRate();
    // The scheduler publishes the rate late in startup, so 0 here means "not started
    // yet", not "broken". Callers fall back to their own default.
    return rate > 0 && rate <= 1000 ? rate : 0;
}

const char* taskRateCurrentTask() {
    if (!g_api.resolved) return nullptr;
    if (g_api.inBackground()) return "bg";
    if (g_api.inForeground()) return "fg";
    if (g_api.inServer()) return "sv";
    return "main/other";
}

void taskRateReportThread(const char* what) {
    if (!g_api.resolved || !what) return;

    // One line per label, and only once. The point is to establish the mapping, not to
    // narrate it every frame.
    static const char* seen[8] = {};
    static int         count = 0;
    for (int i = 0; i < count; ++i)
        if (seen[i] == what) return;
    if (count < 8) seen[count++] = what;

    logf("tick: '%s' runs on the %s task", what, taskRateCurrentTask());
}

bool taskRateFormat(char* buffer, size_t size, double seconds) {
    if (!buffer || size == 0 || !g_api.resolved) return false;
    if (!(seconds > 0.0)) return false;

    const int sim = taskRateSimHz();
    if (sim <= 0) return false;  // scheduler not started yet

    const int bg = g_api.getBGCount();
    const int sv = g_api.getSVCount();
    const int ints = g_api.getIntCount();
    const int ignored = g_api.getIgnoredIntCount();

    if (!g_api.haveBase) {
        g_api.haveBase = true;
        g_api.baseBg = bg;
        g_api.baseSv = sv;
        g_api.baseInt = ints;
        g_api.baseIgnored = ignored;
        return false;  // no window yet, so no honest delta to report
    }

    // Per second, against the real window. See the header.
    const double bgRate = (bg - g_api.baseBg) / seconds;
    const double svRate = (sv - g_api.baseSv) / seconds;
    const double intRate = (ints - g_api.baseInt) / seconds;
    const int    ignoredDelta = ignored - g_api.baseIgnored;

    // bg/s IS the physics step rate, printed next to the rate the physics assumes. The
    // gap between those two numbers is the entire question.
    char duty[64] = {};
    if (g_api.dutyHooked) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        // Snapshot and clear. A tick or two can land between the two reads and be counted
        // in the next window instead of this one; over a second that is noise, and the
        // alternative is a lock on the BG thread's hot path.
        const long long busy = InterlockedExchange64(&g_api.busyTicks, 0);
        const long long worst = InterlockedExchange64(&g_api.worstTicks, 0);

        const double busyMs = 1000.0 * static_cast<double>(busy) / frequency.QuadPart;
        const double worstMs = 1000.0 * static_cast<double>(worst) / frequency.QuadPart;
        const double percent = 100.0 * busyMs / (seconds * 1000.0);
        // A number outside this range means the snapshot raced with something rather than
        // that BG did anything interesting. Say nothing rather than print nonsense.
        if (percent >= 0.0 && percent <= 200.0)
            std::snprintf(duty, sizeof(duty), " busy %.0f%% (worst %.1f ms)", percent,
                          worstMs);
    }

    std::snprintf(buffer, size,
                  "sim %dHz bg %.1f/s%s sv %.1f/s int %.1f/s lost %d adj %.2f", sim,
                  bgRate, duty, svRate, intRate, ignoredDelta,
                  static_cast<double>(g_api.taskGetRateAdjust()));

    // Say it in words the first few times, because the consequence is not visible in a
    // pair of numbers: the sim is not stuttering, it is running SLOW while every formula
    // inside it still divides by the nominal rate.
    // Only for a window long enough to mean something -- a 40 ms window can read 25/s
    // from rounding alone and that is not the sim running slow.
    if (seconds >= 0.5 && bgRate > 0.0 && bgRate < sim - 1.0 &&
        g_api.slowReportsLeft > 0) {
        --g_api.slowReportsLeft;
        logf("tick: the world stepped %.1f times a second but the sim thinks the rate is "
             "%d -- it is running at %.0f%% speed, and every rate-dependent term in it is "
             "scaled for %d. Force feedback reads the steering derivative as rate*delta, "
             "so it is over by the same factor.%s",
             bgRate, sim, 100.0 * bgRate / sim, sim,
             g_api.slowReportsLeft ? "" : " (further reports suppressed)");
    }

    g_api.baseBg = bg;
    g_api.baseSv = sv;
    g_api.baseInt = ints;
    g_api.baseIgnored = ignored;
    return true;
}

}  // namespace gplvr
