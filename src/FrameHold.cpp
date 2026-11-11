#include "FrameHold.h"

#include "Log.h"

namespace gplvr {
namespace {

// Runs reported one by one before the log goes quiet; each later run still counts in the totals
// line every kQuietEvery runs.
constexpr int kRunReports = 20;
constexpr int kQuietEvery = 100;

struct State {
    int run = 0;
    int runsReported = 0;
    int runs = 0;
    int totalFrames = 0;
    int longest = 0;
};

State g_state;

void reportRun(State& s) {
    int frames = s.run;
    ++s.runs;
    s.totalFrames += frames;
    if (frames > s.longest) s.longest = frames;

    if (s.runsReported < kRunReports) {
        ++s.runsReported;
        logf("vr: GPL presented %d race frame%s in a row without a world render -- held, so "
             "the headset kept the last frame with its pose%s",
             frames, frames == 1 ? "" : "s",
             s.runsReported == kRunReports ? " (later runs are only totalled)" : "");
    } else if (s.runs % kQuietEvery == 0) {
        logf("vr: no-render runs so far -- %d runs, %d frames held, the longest %d", s.runs,
             s.totalFrames, s.longest);
    }
}

}  // namespace

void frameHoldNoteOutcome(FrameOutcome outcome) {
    State& s = g_state;
    if (outcome == FrameOutcome::HeldNoRender) {
        ++s.run;
        return;
    }
    if (s.run) reportRun(s);
    s.run = 0;
}

}  // namespace gplvr
