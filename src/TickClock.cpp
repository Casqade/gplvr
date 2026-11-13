#include "TickClock.h"

#include "TaskRate.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

// The fit's noise falls with the number of stamps, whatever time they span: 64 put one tick's
// scheduling jitter into the grid at an eighth of its size, at 36 Hz (1.8 s of ticks) and at
// 144 Hz (0.44 s) alike.
constexpr int kStamps = 64;
constexpr int kSlopeStamps = 16;  // from here the spacing is fitted too; below it is the nominal

// TWO KINDS OF STAMP ARE OFF THE GRID, and they want opposite treatment.
//
// A LATE STAMP is one tick's own delay -- the BG thread was descheduled, or the tick was a heavy
// one. The next stamp is back on the grid, so the late one is left out of the fit and the grid
// stays where it was. A NEW RUN is permanent: a dropped tick moves every later stamp by a whole
// tick, a pause or a load by more, and the grid has to start again from there.
//
// They cannot be told apart from one stamp, so the newest stamp alone never decides. The run is
// read from the median of the newest five: one or two stamps somewhere else are outvoted and left
// out, and the fit starts again once three of the five agree on a new place. The price is that
// after a real dropped tick the grid is the old one for two more ticks.
constexpr int    kSeedStamps = 5;
constexpr int    kRunEnd = 3;            // stamps in a row off the run, walking back, that end it
constexpr double kRunTolerance = 0.45;   // of a tick: how far from the run a stamp may be
constexpr double kReferenceGain = 0.25;  // how fast the run's place follows the stamps kept

// Within the run, a stamp further from the fitted line than this many typical distances is left
// out and the line fitted again. The floor is a share of a tick, so that a very regular run does
// not discard stamps over a few microseconds.
constexpr double kOutlierSpreads = 4.0;
constexpr double kOutlierFloor = 0.02;

struct Stamp {
    int       count;
    long long qpc;
};

// Written by the BG thread, read by the frame thread. `sequence` is odd while a stamp is being
// written, so a reader that sees it odd or changed has a torn copy and reads again. The stamps
// hold 64-bit values in a 32-bit build, which is why a plain read is not enough.
struct Shared {
    volatile LONG sequence = 0;
    volatile LONG pushed = 0;  // stamps written so far; the newest is ring[(pushed - 1) % kStamps]
    Stamp         ring[kStamps] = {};
    int           lastCount = 0;  // BG thread only
    bool          haveLast = false;
};

// Frame thread only.
struct Fit {
    long long frequency = 0;
    LONG      fittedAt = -1;  // `pushed` when this fit was made
    bool      valid = false;
    int       anchorCount = 0;  // the newest stamped tick
    double    anchorQpc = 0.0;  // the grid's time for it
    double    period = 0.0;
};

// A stamp relative to the newest one, which keeps the sums small: x in ticks, y in QPC.
struct Point {
    double x;
    double y;
};

Shared g_shared;
Fit    g_fit;

bool snapshot(Stamp* out, LONG& pushed) {
    Shared& s = g_shared;
    for (int attempt = 0; attempt < 8; ++attempt) {
        LONG before = s.sequence;
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        pushed = s.pushed;
        std::memcpy(out, s.ring, sizeof(s.ring));
        if (s.sequence == before) return true;
    }
    return false;
}

double medianOf(double* values, int n) {
    std::sort(values, values + n);
    return n % 2 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

// Least squares through `points`: the spacing, and the line's value at the newest stamp (x = 0).
//
// The sim's timer runs against the CPU clock and can sit a fraction of a percent off its nominal
// rate; over a long run the fitted spacing follows that. A short run cannot tell the spacing from
// jitter and takes the nominal one.
void fitLine(const Point* points, int n, double nominal, double& period, double& atNewest) {
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    for (int i = 0; i < n; ++i) {
        sumX += points[i].x;
        sumY += points[i].y;
        sumXX += points[i].x * points[i].x;
        sumXY += points[i].x * points[i].y;
    }
    period = nominal;
    double spread = n * sumXX - sumX * sumX;
    if (n >= kSlopeStamps && spread > 0.0) {
        double slope = (n * sumXY - sumX * sumY) / spread;
        if (slope > 0.98 * nominal && slope < 1.02 * nominal) period = slope;
    }
    atNewest = sumY / n - period * (sumX / n);
}

// A line through the current run of stamps, with the late ones left out. See the note on the two
// kinds of off-grid stamp at the top of the file.
void refit() {
    Fit& f = g_fit;
    if (g_shared.pushed == f.fittedAt) return;

    if (!f.frequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        f.frequency = frequency.QuadPart;
    }
    int simHz = taskRateSimHz();
    if (simHz <= 0 || f.frequency <= 0) {
        f.valid = false;
        return;
    }

    Stamp stamps[kStamps];
    LONG  pushed = 0;
    if (!snapshot(stamps, pushed)) return;  // the previous fit stands for this frame
    f.fittedAt = pushed;

    int count = pushed < kStamps ? static_cast<int>(pushed) : kStamps;
    if (count < 1) {
        f.valid = false;
        return;
    }

    double       nominal = static_cast<double>(f.frequency) / simHz;
    const Stamp& newest = stamps[static_cast<unsigned>(pushed - 1) % kStamps];

    // The stamps newest first, and how far each is from a nominal grid laid through the newest.
    // The counter only moves forward within a session, so a stamp that is not older in ticks
    // than the one after it is from another session and ends the history.
    Point  all[kStamps];
    double off[kStamps];
    int    usable = 0;
    for (int i = 0; i < count; ++i) {
        const Stamp& stamp = stamps[static_cast<unsigned>(pushed - 1 - i) % kStamps];
        Point        point = {static_cast<double>(stamp.count - newest.count),
                              static_cast<double>(stamp.qpc - newest.qpc)};
        if (i > 0 && point.x >= all[i - 1].x) break;
        all[i] = point;
        off[i] = point.y - point.x * nominal;
        ++usable;
    }

    // Where the current run is: the stamp with the median distance among the newest few.
    int seed = 0;
    int seedCount = usable < kSeedStamps ? usable : kSeedStamps;
    if (seedCount >= 3) {
        int order[kSeedStamps];
        for (int i = 0; i < seedCount; ++i) order[i] = i;
        std::sort(order, order + seedCount, [&off](int a, int b) { return off[a] < off[b]; });
        seed = order[(seedCount - 1) / 2];
    }

    // The run's stamps. Those newer than the seed are judged against it. Going back from it,
    // each is judged against where the run has been so far, which follows the stamps kept so
    // that a spacing slightly off the nominal one does not walk old stamps out of tolerance.
    // kRunEnd stamps in a row off the run are where it began; fewer are late stamps inside it.
    double tolerance = kRunTolerance * nominal;
    Point  kept[kStamps];
    int    n = 0;
    for (int i = 0; i <= seed; ++i)
        if (std::fabs(off[i] - off[seed]) <= tolerance) kept[n++] = all[i];
    double reference = off[seed];
    int    missed = 0;
    for (int i = seed + 1; i < usable; ++i) {
        if (std::fabs(off[i] - reference) <= tolerance) {
            kept[n++] = all[i];
            reference += kReferenceGain * (off[i] - reference);
            missed = 0;
        } else if (++missed >= kRunEnd) {
            break;
        }
    }

    double period = nominal;
    double atNewest = 0.0;
    fitLine(kept, n, nominal, period, atNewest);

    // Leave out the stamps far from the line next to the rest, and fit again. The typical
    // distance is the median one, so at least half the stamps always stay.
    if (n >= 4) {
        double distance[kStamps];
        double sorted[kStamps];
        for (int i = 0; i < n; ++i) {
            distance[i] = kept[i].y - (atNewest + period * kept[i].x);
            sorted[i] = distance[i];
        }
        double middle = medianOf(sorted, n);
        for (int i = 0; i < n; ++i) {
            distance[i] = std::fabs(distance[i] - middle);
            sorted[i] = distance[i];
        }
        // 1.4826 scales a median distance to a standard deviation for normally spread stamps.
        double typical = 1.4826 * medianOf(sorted, n);
        double limit = std::max(kOutlierSpreads * typical, kOutlierFloor * nominal);
        int    stay = 0;
        for (int i = 0; i < n; ++i)
            if (distance[i] <= limit) kept[stay++] = kept[i];
        if (stay < n) {
            n = stay;
            fitLine(kept, n, nominal, period, atNewest);
        }
    }

    f.anchorCount = newest.count;
    f.anchorQpc = static_cast<double>(newest.qpc) + atNewest;
    f.period = period;
    f.valid = true;
}

}  // namespace

void tickClockNote(int count, long long qpc) {
    Shared& s = g_shared;
    if (s.haveLast && count == s.lastCount) return;
    s.lastCount = count;
    s.haveLast = true;

    InterlockedIncrement(&s.sequence);
    Stamp& slot = s.ring[static_cast<unsigned>(s.pushed) % kStamps];
    slot.count = count;
    slot.qpc = qpc;
    InterlockedIncrement(&s.pushed);
    InterlockedIncrement(&s.sequence);
}

bool tickClockArrival(int count, double& qpc) {
    refit();
    const Fit& f = g_fit;
    if (!f.valid) return false;
    qpc = f.anchorQpc + (count - f.anchorCount) * f.period;
    return true;
}

double tickClockPeriod() {
    refit();
    return g_fit.valid ? g_fit.period : 0.0;
}

}  // namespace gplvr
