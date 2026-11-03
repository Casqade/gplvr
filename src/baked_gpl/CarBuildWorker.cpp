#include "baked_gpl/CarBuildWorker.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace gplvr {
namespace {

// A wait for work or for the gate looks again this often, whatever woke it.
constexpr DWORD kIdleWaitMs = 100;

HANDLE      g_thread = nullptr;
// Auto-reset, one waiter: work queued, the gate opened, a pause ended, a cancel or a stop. The
// worker looks at what it waits for before every wait, so one signal taken by another wait is not
// lost.
HANDLE      g_signal = nullptr;
CarBuildRun g_run = nullptr;
bool        g_tried = false;    // render thread
bool        g_running = false;  // render thread; set before the thread starts

std::atomic<bool>     g_stop{false};
std::atomic<uint32_t> g_epoch{1};
std::atomic<int>      g_open{0};
std::atomic<int>      g_paused{0};
// Held shared by the worker while it reads, exclusive by the render thread to have it out.
SRWLOCK g_reads = SRWLOCK_INIT;
int     g_pauseDepth = 0;      // render thread
bool    g_pauseHeld = false;   // render thread: the outermost pause holds g_reads
bool    g_inside = false;      // worker: it holds g_reads shared

// The queue and the finished jobs: rings of slots. A slot is in one of them, or with the worker,
// or with CarProgram.cpp, so g_inFlight (all but the last) bounds both.
SRWLOCK  g_queueLock = SRWLOCK_INIT;
uint32_t g_queued[kCarBuildQueue];
uint32_t g_queuedHead = 0;
uint32_t g_queuedCount = 0;
uint32_t g_done[kCarBuildQueue];
uint32_t g_doneHead = 0;
uint32_t g_doneCount = 0;
uint32_t g_inFlight = 0;

DWORD WINAPI threadProc(void*) {
    while (!g_stop.load()) {
        uint32_t slot = 0;
        bool     have = false;
        AcquireSRWLockExclusive(&g_queueLock);
        if (g_queuedCount) {
            slot = g_queued[g_queuedHead];
            g_queuedHead = (g_queuedHead + 1) % kCarBuildQueue;
            --g_queuedCount;
            have = true;
        }
        ReleaseSRWLockExclusive(&g_queueLock);
        if (!have) {
            WaitForSingleObject(g_signal, kIdleWaitMs);
            continue;
        }
        // The job catches what it throws and says so in its slot; anything past it still hands
        // the slot back.
        try {
            g_run(slot);
        } catch (...) {
        }
        carBuildLeave();
        AcquireSRWLockExclusive(&g_queueLock);
        g_done[(g_doneHead + g_doneCount) % kCarBuildQueue] = slot;
        ++g_doneCount;
        ReleaseSRWLockExclusive(&g_queueLock);
    }
    carBuildLeave();
    return 0;
}

}  // namespace

bool carBuildWorkerStart(CarBuildRun run, const char*& why) {
    static char text[96];
    if (g_running) return true;
    if (g_tried) {
        why = "it could not be started before";
        return false;
    }
    g_tried = true;
    g_run = run;
    g_signal = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_signal) {
        std::snprintf(text, sizeof(text), "CreateEvent failed (error %lu)", GetLastError());
        why = text;
        return false;
    }
    g_thread = CreateThread(nullptr, 0, &threadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        std::snprintf(text, sizeof(text), "CreateThread failed (error %lu)", GetLastError());
        why = text;
        CloseHandle(g_signal);
        g_signal = nullptr;
        return false;
    }
    g_running = true;
    return true;
}

bool carBuildWorkerRunning() { return g_running; }

bool carBuildWorkerSubmit(uint32_t slot) {
    if (!g_running) return false;
    bool queued = false;
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_inFlight < kCarBuildQueue) {
        g_queued[(g_queuedHead + g_queuedCount) % kCarBuildQueue] = slot;
        ++g_queuedCount;
        ++g_inFlight;
        queued = true;
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    if (queued) SetEvent(g_signal);
    return queued;
}

bool carBuildWorkerTakeDone(uint32_t& slot) {
    if (!g_running) return false;
    bool have = false;
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_doneCount) {
        slot = g_done[g_doneHead];
        g_doneHead = (g_doneHead + 1) % kCarBuildQueue;
        --g_doneCount;
        --g_inFlight;
        have = true;
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    return have;
}

uint32_t carBuildWorkerEpoch() { return g_epoch.load(); }

void carBuildWorkerCancel() {
    g_epoch.fetch_add(1);
    if (g_signal) SetEvent(g_signal);
}

void carBuildWorkerStop() {
    g_stop.store(true);
    if (g_signal) SetEvent(g_signal);
}

void carBuildGateOpen() {
    if (!g_running) return;
    g_open.store(1);
    SetEvent(g_signal);
}

void carBuildGateClose() {
    if (!g_running) return;
    g_open.store(0);
    // An open pause holds the reads already: the worker is out.
    if (g_pauseHeld) return;
    AcquireSRWLockExclusive(&g_reads);
    ReleaseSRWLockExclusive(&g_reads);
}

CarBuildPause::CarBuildPause() {
    if (g_pauseDepth++ || !g_running) return;
    g_paused.store(1);
    AcquireSRWLockExclusive(&g_reads);
    g_pauseHeld = true;
}

CarBuildPause::~CarBuildPause() {
    if (--g_pauseDepth || !g_pauseHeld) return;
    g_pauseHeld = false;
    ReleaseSRWLockExclusive(&g_reads);
    g_paused.store(0);
    SetEvent(g_signal);
}

bool carBuildEnter(uint32_t epoch) {
    if (g_inside) return true;
    for (;;) {
        if (g_stop.load() || g_epoch.load() != epoch) return false;
        if (g_open.load() && !g_paused.load()) {
            AcquireSRWLockShared(&g_reads);
            // Looked at again under the lock: a close or a pause that came between finds the
            // lock free, returns, and leaves the flag that sends the worker out here.
            if (g_open.load() && !g_paused.load() && g_epoch.load() == epoch && !g_stop.load()) {
                g_inside = true;
                return true;
            }
            ReleaseSRWLockShared(&g_reads);
        }
        WaitForSingleObject(g_signal, kIdleWaitMs);
    }
}

int carBuildCheckpoint(uint32_t epoch) {
    if (g_open.load(std::memory_order_relaxed) && !g_paused.load(std::memory_order_relaxed) &&
        g_epoch.load(std::memory_order_relaxed) == epoch &&
        !g_stop.load(std::memory_order_relaxed))
        return kCarBuildGoOn;
    carBuildLeave();
    return carBuildEnter(epoch) ? kCarBuildResumed : kCarBuildStop;
}

void carBuildLeave() {
    if (!g_inside) return;
    g_inside = false;
    ReleaseSRWLockShared(&g_reads);
}

}  // namespace gplvr
