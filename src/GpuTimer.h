#pragma once

// GPU time per span, from GL_TIMESTAMP timer queries (ARB_timer_query, core in GL 3.3).
//
// Every CPU span says how long the render thread spent between two points. None of them
// says what the GPU did, and a CPU span can absorb GPU backpressure wherever the driver
// happens to block -- which is exactly what an unexplained span that grows while the scene
// does not looks like. A timestamp query records the GPU clock at the moment the GPU
// REACHES a point in the command stream, so the difference between two of them is the GPU
// time spent on the commands issued between the two marks.
//
// The marks TILE the frame on the GPU clock the way the CPU spans tile it on the CPU
// clock: each mark closes the previous group and opens the next, so the groups sum to the
// GPU-side frame. A group also counts any time the GPU sat idle waiting for the CPU to
// issue its commands -- so read a GPU group as GPU cost where the GPU is behind (the queue
// figure below is large) and as submission pace where it is not.
//
// Nothing ever waits for a result. Each frame's queries are read back once they report
// themselves available, typically two or three frames later, and a frame for which no
// record is free is simply not measured and counted as skipped.
//
// Everything is resolved by name. A context without timer queries logs one line and the
// GPU line does not appear.

namespace gplvr {

constexpr int kGpuTimerMaxGroups = 16;

// Close the current group and open `group`. Free when no frame record is open.
void gpuTimerMark(int group);

// The frame boundary: close this frame's record, read back any finished ones, open the next
// record with `firstGroup`. Needs the render thread's context current.
void gpuTimerEndFrame(int firstGroup);

struct GpuTimerWindow {
    double   groupMs[kGpuTimerMaxGroups] = {};  // per frame read
    double   frameMs = 0.0;                     // GPU clock, frame boundary to frame boundary
    double   queueMs = 0.0;                     // how long after being issued the GPU reached
    double   queueWorstMs = 0.0;                //   the frame boundary: the pipeline's depth
    double   framesLate = 0.0;                  // frames between issue and read-back
    unsigned frames = 0;
    unsigned skipped = 0;
};

// The window's per-frame means, and a reset. False when timer queries are unavailable or
// nothing was measured.
bool gpuTimerTake(GpuTimerWindow& out);

}  // namespace gplvr
