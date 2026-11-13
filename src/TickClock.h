#pragma once

// When each simulation tick happened, stamped on the simulation's own thread.
//
// The blend places every drawn frame between two sim ticks, so it has to know when those ticks
// happened. The frame loop only learns of a tick when one of its frames latches it, which is up
// to a whole frame after the tick -- a different delay every tick unless the frame rate is a
// whole multiple of the sim rate. Dating ticks by their latch therefore moves the world unevenly:
// 36 Hz ticks on a 60 Hz display advance it 27.8, 16.7 and 11.1 ms on consecutive 16.7 ms frames.
//
// The BG thread stamps the tick counter with QPC as it moves (TaskRate's bgSleep hook), and the
// frame loop reads a uniform grid fitted to the newest stamps. The grid is a line through them:
// one tick's scheduling jitter moves it by a fraction of itself, and the sim's own time is
// uniform, so a uniform grid is the right map from wall time to sim time.
//
// A stamp that is merely late is left out of the fit. The grid starts again only when the
// stamps stay somewhere else -- after a dropped tick, a pause or a load -- which it takes three
// stamps to tell from lateness, so for two ticks after such a break the grid is still the old
// one.

#include <cstddef>

namespace gplvr {

// BG thread only. The tick counter as it stands at `qpc`; a stamp is kept when it has moved.
void tickClockNote(int count, long long qpc);

// Frame thread only. When sim tick `count` happened, in QPC ticks, on the fitted grid. False
// until the first tick has been stamped and the scheduler has published its rate.
bool tickClockArrival(int count, double& qpc);

// Frame thread only. The grid's spacing in QPC ticks per sim tick, 0 while there is no grid.
double tickClockPeriod();

}  // namespace gplvr
