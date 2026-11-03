#pragma once

// Car program builds on a worker thread (docs/audit-2026-09-18/105-baked-renderer-rewrite.md
// section 4.61). A car call that needs a build walks the car as before on the render thread and
// hands the build to one worker thread, with a snapshot of what the build reads that GPL or gplvr
// rewrites per call (CarWalk.h, CarBuildSnapshot); the worker walks GPL's graph and compiles the
// program into heap scratch of its own, and the render thread keeps the result at a later pass
// (CarProgram.cpp). This file is the thread, its bounded queue and the gate that keeps its reads
// safe; the jobs themselves are CarProgram.cpp's, by slot.
//
// The gate. The worker reads GPL's car graph, which GPL frees when it leaves a session (on its
// foreground thread, the render thread, outside every pass), and gplvr's car bake (CarBake.h) and
// configuration tables (CarConfig.h), which the render thread grows during a pass. So the worker
// reads only while the gate is open -- from a baked pass's begin, after the car bake has followed
// GPL's reloads, to that pass's end -- and only while no CarBuildPause is held:
// - carBuildGateClose returns once the worker has stepped out (it holds a shared lock while it
//   reads and gives it up at its next checkpoint, a node of its walk); every pass end and every
//   pass begin closes the gate;
// - a render-thread write to what the worker reads holds a CarBuildPause around it, which asks the
//   worker out the same way and lets it back in after;
// - the worker, let back in, goes on only while its job's epoch and car bake generation hold, so
//   a cancel (carBuildWorkerCancel), a dropped bake or a new car set ends its build at once.
// A wait of the render thread on the worker is a checkpoint away.

#include <cstdint>

namespace gplvr {

// false: every car program is built on the render thread, as before section 4.61 (a rebuild
// bisects the worker with it).
constexpr bool kCarBuildWorker = true;

// false: a car whose build is on the worker is walked as before until the build lands; true: it is
// drawn from its most recent program meanwhile where that is safe (CarProgram.cpp, report 105
// section 4.63).
constexpr bool kCarStaleWhileBuilding = true;

// Jobs queued or in flight at once; a request past it is built on the render thread.
constexpr uint32_t kCarBuildQueue = 24;

// A job, on the worker: `slot` is CarProgram.cpp's.
using CarBuildRun = void (*)(uint32_t slot);

// From carProgramInstall, once, on the render thread after the program region is placed. False
// when the thread could not be made; the reason is in `why`.
bool carBuildWorkerStart(CarBuildRun run, const char*& why);
bool carBuildWorkerRunning();

// Render thread: `slot` queued for the worker; false when the queue is full. Every slot queued
// comes back exactly once through carBuildWorkerTakeDone, built or not.
bool carBuildWorkerSubmit(uint32_t slot);
bool carBuildWorkerTakeDone(uint32_t& slot);

// The epoch a job is queued under; carBuildWorkerCancel moves it, and every job of an older one
// stops at its next checkpoint (or before it starts) and comes back unbuilt.
uint32_t carBuildWorkerEpoch();
void     carBuildWorkerCancel();

// At DLL_PROCESS_DETACH: asks the worker to end. It is not waited for under the loader lock; at
// process exit the system has ended it already.
void carBuildWorkerStop();

// Render thread, around each baked pass (see above).
void carBuildGateOpen();
void carBuildGateClose();

// Render thread: while one is alive the worker reads nothing. Nests.
class CarBuildPause {
public:
    CarBuildPause();
    ~CarBuildPause();
    CarBuildPause(const CarBuildPause&) = delete;
    CarBuildPause& operator=(const CarBuildPause&) = delete;
};

// The worker, around its reads: carBuildEnter waits for the gate and returns false when `epoch`
// is no longer the current one (or the worker is stopping); carBuildCheckpoint steps out while
// the gate is closed or a pause is held and comes back in as carBuildEnter does.
enum CarBuildCheck : int {
    kCarBuildGoOn,     // nothing happened
    kCarBuildResumed,  // it stepped out and came back: what the render thread may have changed
                       // meanwhile (the car bake's generation) must be checked again
    kCarBuildStop,     // the epoch moved or the worker is stopping: it is out, and stays out
};
bool carBuildEnter(uint32_t epoch);
int  carBuildCheckpoint(uint32_t epoch);
void carBuildLeave();

}  // namespace gplvr
