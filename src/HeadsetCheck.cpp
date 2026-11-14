#include "HeadsetCheck.h"

#include "Hooks.h"
#include "Log.h"
#include "presenter/Producer.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace gplvr {
namespace {

enum Phase { kIdle, kLaunching, kLaunched, kSettled };

std::atomic<int>  g_phase{kIdle};
std::atomic<bool> g_launched{false};
std::atomic<bool> g_refused{false};
// The game's thread (the rasterizer load) and the install thread can both arrive here; the
// second one waits for the first one's answer instead of acting on it twice.
std::mutex g_settle;

double millisecondsSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * static_cast<double>(now.QuadPart - start.QuadPart) /
           static_cast<double>(frequency.QuadPart);
}

}  // namespace

void headsetCheckStart() {
    if (!g_options.vr) {
        g_phase = kSettled;
        return;
    }
    int idle = kIdle;
    if (!g_phase.compare_exchange_strong(idle, kLaunching)) return;
    logf("vr: launching the presenter now, to know whether there is a headset before "
         "anything is installed for one");
    g_launched = presenterLaunchEarly();
    g_phase = kLaunched;
}

void headsetCheckSettle(const char* where) {
    if (g_phase == kSettled || !g_options.vr) return;
    std::lock_guard<std::mutex> lock(g_settle);
    if (g_phase == kSettled) return;

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    // The install thread launches it first thing, seconds before the game reaches its
    // rasterizer. If that thread has not got there, this caller launches it instead.
    headsetCheckStart();
    while (g_phase < kLaunched) Sleep(1);

    HeadsetVerdict verdict = g_launched ? presenterHeadsetVerdict() : HeadsetVerdict::NoHeadset;
    if (verdict == HeadsetVerdict::Pending)
        logf("vr: %s waits for the presenter's answer -- is there a headset", where);
    DWORD lastNote = GetTickCount();
    while (verdict == HeadsetVerdict::Pending) {
        Sleep(5);
        verdict = presenterHeadsetVerdict();
        if (verdict == HeadsetVerdict::Pending && GetTickCount() - lastNote >= 5000) {
            lastNote = GetTickCount();
            logf("vr: still waiting for the presenter after %.1f s -- gplvr-xr.log shows how "
                 "far the runtime got; a dialog of the runtime's own may be open",
                 millisecondsSince(start) / 1000.0);
        }
    }

    double waited = millisecondsSince(start);
    if (verdict == HeadsetVerdict::Headset) {
        logf("vr: the presenter found a headset (%s, waited %.0f ms) -- installing for VR",
             where, waited);
    } else {
        presenterAbandonLaunch();
        g_refused = true;
        logf("vr: NO HEADSET -- %s (%s, waited %.0f ms); gplvr-xr.log says why. Playing flat, "
             "as with vr = 0: the options are read again for it",
             g_launched ? "the presenter's runtime has no headset to give it" :
                          "the presenter could not be launched",
             where, waited);
        loadOptions("gplvr.ini");
    }
    g_phase = kSettled;
}

bool headsetCheckRefused() { return g_refused; }

}  // namespace gplvr
