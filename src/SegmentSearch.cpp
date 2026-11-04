#include "SegmentSearch.h"

#include "DataRebase.h"
#include "Log.h"
#include "Stereo.h"

#include <cstdint>

namespace gplvr {
namespace {

constexpr uintptr_t kSegmentCount = 0x006AFB08;
constexpr uintptr_t kSegmentTable = 0x006AEA1C;  // float rows: +4 start m, +12 scale, +16 length
constexpr int       kSegmentRowWords = 360;      // 1440 bytes a row

// GPL'S OWN CONTAINMENT ANSWER, which its search asks for and then throws away.
//
// gpl_segmentPlacePoint 0x4608D0 projects the point onto one segment and returns:
//     0  the point is INSIDE this segment -- the dlong it wrote is real
//     1  the point is past its end
//     2  the point is before its start
// On 1 and 2 it still writes a dlong, the point EXTRAPOLATED along that segment: on a straight
// `segment.baseDlong + projection`, on a curve relative to the segment without its base. That
// number is only meaningful if the track continues the way this segment does. A straight has no
// lateral bound and a curve no radial one, and a curve never answers 2: an angle behind its arc
// wraps round to "past the end".
//
// gpl_cameraTrackPositionFromWorld loops it, stepping one segment towards the point, and exits on
// `while (v8 & v9)` -- as soon as two consecutive probes stop sharing a bit. A 1 followed by a 2
// (stepped forward and overshot) shares nothing, so the loop stops there and PUBLISHES THE
// EXTRAPOLATION from a segment the point is not in. There is no step-back-and-accept.
//
// So the failure has a name and a test: the point fell in a seam between two segments'
// longitudinal spans, and the answer is an extrapolation. Asking this function directly about the
// segment the search settled on is exactly the check the engine omits -- no threshold in it.
constexpr uintptr_t kSegmentArray = 0x006B7FD8;   // dword_6B7FD8, the array 0x460B40 indexes
constexpr uintptr_t kSegmentArrayCount = 0x006AFB08;
constexpr uintptr_t kSegmentPlacePoint = 0x004608D0;
using PlacePointFn = int(__cdecl*)(const void* segment, int x, int y, int* dlong, int* dlat);

// The number of segments in the array 0x460B40 searches, or -1 when it cannot be read.
int segmentArrayCount() {
    uintptr_t countAt = rebase(kSegmentArrayCount);
    if (!countAt) return -1;
    int count = *reinterpret_cast<const int*>(countAt);
    return count > 0 && count <= 8192 ? count : -1;
}

// How many segments apart two indices are, the short way round the ring of `count` -- without the
// wrap a lap crossing reads as a jump of the whole track.
int segmentsApart(int a, int b, int count) {
    int d = (a - b) % count;
    if (d < 0) d += count;
    return d < count - d ? d : count - d;
}

// A SEARCH THAT RAN AWAY FROM ITS SEED. The hint is the segment holding the eye at the NEWEST
// tick, and the blend draws the eye up to a tick behind it. Just after that tick enters a curve,
// the drawn eye is still before the curve's start -- and a curve answers "past the end" for it.
// So the search steps FORWARD, away from the point, and a straight has no lateral bound, so the
// first straight whose span the point projects into answers "inside" with a dlong of that far
// part of the track. Measured on monza10k (segprobe, 2026-11-04): hint 106, a curve; answer
// segment 194, 88 ahead, dlong 1,015 m on and dlat -634 m, for three frames -- the walk centred
// on the far side of the track, which drew the ground plane and the horizon and nothing else.
// Containment and the table both pass that answer, and frameRate 0 never shows it, because
// unblended the eye is in its hint.
//
// The drawn eye is at most a tick behind its hint, so a sound answer is within a couple of
// segments of it. Further than that, the search is asked again from the segments next to the
// hint, behind it first, and the first answer that is placed inside and near the hint is kept.
// Report 84 recorded a reseat as a no-op for ITS defect, a search stopping a segment early from a
// right seed; this one runs away from the seed, and the seed one segment back is the eye's own.
constexpr int kSegmentReach = 2;

}  // namespace

// Both globals go through rebase because this reads a pointer out of one, indexes it, and hands
// the element to the engine as an object pointer. The non-null and range tests below cannot tell
// a wrong global from a right one -- a plausible pointer and a count of a few hundred are common
// shapes -- and the install gate is two code sites that are identical in every link of 1.2.0.2.
int placementOf(int segment, int x, int y) {
    uintptr_t arrayAt = rebase(kSegmentArray);
    uintptr_t countAt = rebase(kSegmentArrayCount);
    if (!arrayAt || !countAt) return -1;
    auto base = *reinterpret_cast<const void* const* const*>(arrayAt);
    int  count = *reinterpret_cast<const int*>(countAt);
    if (!base || count <= 0 || count > 8192 || segment < 0 || segment >= count) return -1;
    const void* node = base[segment];
    if (!node) return -1;
    int dlong = 0;
    int dlat = 0;
    return reinterpret_cast<PlacePointFn>(kSegmentPlacePoint)(node, x, y, &dlong, &dlat);
}

// THE SEGMENT FOR A DLONG, by table lookup instead of by search.
//
// gpl_cameraTrackPositionFromWorld answers the same question by walking from a hint segment, and
// that walk is not a continuous function of its input: it can settle on a segment the point is not
// inside (report 84) and which one it settles on moves with the hint and with sub-centimetre
// changes in the position. The segment sets the origin of the drawn range, so a multi-segment
// change shifts the whole world for a frame.
//
// The table at 0x006AEA1C is an ordered array of 1440-byte rows whose start dlong is at +4 and
// whose length is +16 / +12, so this is monotone: the answer moves by one segment when the dlong
// crosses a boundary and cannot move by four unless the DLONG moved by four segments' worth. It
// therefore still follows a camera cut, a car reset or a lap crossing exactly, because those move
// the dlong; what it cannot do is invent a jump the dlong did not make.
int segmentForDlong(float metres) {
    // Through rebase: the table pointer is dereferenced and then indexed by up to 4096 rows of
    // 1440 bytes, so a global belonging to another link reads megabytes away from anything.
    uintptr_t countAt = rebase(kSegmentCount);
    uintptr_t tableAt = rebase(kSegmentTable);
    if (!countAt || !tableAt) return -1;
    int          count = *reinterpret_cast<const int*>(countAt);
    const float* table = *reinterpret_cast<const float* const*>(tableAt);
    if (count <= 0 || count > 4096 || !table) return -1;

    int low = 0;
    int high = count - 1;
    while (low <= high) {
        int          mid = low + (high - low) / 2;
        const float* row = table + kSegmentRowWords * mid;
        float        start = row[1];
        if (!(row[3] > 0.0f)) return -1;
        float end = start + row[4] / row[3];
        if (metres < start) {
            high = mid - 1;
        } else if (metres >= end) {
            low = mid + 1;
        } else {
            return mid;
        }
    }
    return -1;
}

int askNearHint(WalkSegmentFn original, int hint, int* out, int x, int y, int z, bool& runaway) {
    int segment = original(hint, out, x, y, z);
    int count = segmentArrayCount();
    runaway = count > 0 && hint >= 0 && hint < count &&
          segmentsApart(segment, hint, count) > kSegmentReach;
    if (!runaway) return segment;

    // The first few of each outcome are logged, then every 100th, so a session says whether the
    // case occurred and whether the reseat held.
    static unsigned reseated = 0;
    static unsigned unsettled = 0;
    int first[3] = {out[0], out[1], out[2]};
    for (int step : {-1, -2, 1}) {
        int seed = (hint + step + count) % count;
        int again = original(seed, out, x, y, z);
        if (segmentsApart(again, hint, count) <= kSegmentReach && placementOf(again, x, y) == 0) {
            runaway = false;
            if (++reseated <= 8 || reseated % 100 == 0)
                logf("walk: the segment search ran from hint %d to segment %d (dlong %.1f m); "
                     "asked again from %d it answered %d (dlong %.1f m) -- %u reseat(s) so far",
                     hint, segment, first[0] / kDlongUnitsPerMetre, seed, again,
                     out[0] / kDlongUnitsPerMetre, reseated);
            return again;
        }
    }
    out[0] = first[0];
    out[1] = first[1];
    out[2] = first[2];
    if (++unsettled <= 8 || unsettled % 100 == 0)
        logf("walk: the segment search ran from hint %d to segment %d (dlong %.1f m) and no seed "
             "beside the hint placed the point -- the answer goes to the car fallback; %u so far",
             hint, segment, first[0] / kDlongUnitsPerMetre, unsettled);
    return segment;
}

}  // namespace gplvr
