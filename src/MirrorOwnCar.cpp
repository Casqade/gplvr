#include "MirrorOwnCar.h"

#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarWalk.h"

#include "Hooks.h"
#include "Log.h"
#include "Pe.h"
#include "Stereo.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe, IDA 2026-11-16) -----------------------------------------------

// gpl_sceneRender's `push 1; call gpl_matrixPop` closing the loop that queues p[88]'s objects
// under the main eye's view (0x48771E..0x487744).
constexpr uintptr_t     kAfterQueueSite = 0x00487748;
constexpr unsigned char kAfterQueueStock[5] = {0xE8, 0xE3, 0xFE, 0x03, 0x00};
constexpr uintptr_t     kMatrixPop = 0x004C7630;
// Its `call gpl_objectEnqueueInSegment` for p[86], after the mirror pass.
constexpr uintptr_t     kViewedQueueSite = 0x00487DFF;
constexpr unsigned char kViewedQueueStock[5] = {0xE8, 0x6C, 0xEF, 0xFF, 0xFF};
constexpr uintptr_t     kEnqueue = 0x00486D70;
// The callback that call is handed: the immediate of `push offset gpl_dynamicObjectSelectNode` at
// 0x487DF3, read when queueing, since LodCensus.cpp may point it at its own thunk.
constexpr uintptr_t     kViewedCallbackImmediate = 0x00487DF4;

constexpr unsigned kParamsViewedRecord = 0x158;  // p[86]
constexpr unsigned kRecordCockpit = 48;          // gpl_carRecordDraw: cockpit model when set
constexpr unsigned kRecordDlong = 12;            // the float gpl_objectEnqueueInSegment takes
constexpr unsigned kTrackObject = 56;            // p[0]'s track root, gpl_sceneRender 0x48768B

typedef char(__cdecl* MatrixPopFn)(char);
typedef int(__cdecl* EnqueueFn)(int, int, int, int, float);

struct State {
    bool      installed = false;
    bool      paneSet = false;
    float     plane[4] = {};
    float     shift[3] = {};  // world
    // The record queued for this sceneRender, until GPL's own call for it.
    uintptr_t queued = 0;
    // Around the walk: the word +48 taken off, or the record skipped.
    bool      flipped = false;
    uint32_t  savedCockpit = 0;
    bool      skipping = false;
    // Said once each.
    bool      saidQueued = false;
    bool      saidDrawn = false;
    bool      saidSkipped = false;
    bool      saidRefusal = false;
};

State g_own;

// mirrorOwnCarPlaced's sums since the last report, per model: 0 the cockpit, 1 the outside model.
struct Placed {
    uint32_t count = 0;
    double   sum[3] = {};
    float    worst = 0.0f;
    float    worstTurn = 0.0f;  // degrees
};

struct PlacementCheck {
    bool          frameSet = false;
    float         origin[3] = {};
    float         R[9] = {};
    Placed        model[2];
    LARGE_INTEGER since = {};
};

PlacementCheck g_placement;

constexpr long long kPlacementReportSeconds = 5;

template <typename T>
T& at(uintptr_t address) {
    return *reinterpret_cast<T*>(address);
}

void refused(const char* why) {
    State& s = g_own;
    if (s.saidRefusal) return;
    s.saidRefusal = true;
    logf("mirror own car: not queued for this mirror render (first time) -- %s", why);
}

// The viewed car into the render tree as gpl_sceneRender's call at 0x487DFF puts it, from the
// parameter block GPL is rendering.
void queueViewedCar() {
    State& s = g_own;
    s.queued = 0;
    if (!s.paneSet || !mirrorOwnCar()) return;
    if (rendererType() == 0) return;
    auto* params = static_cast<SceneParams*>(hooksLastSceneParams());
    if (!params || !params->mirrorCamera) return;
    auto base = reinterpret_cast<uintptr_t>(params);
    uintptr_t record = at<uint32_t>(base + kParamsViewedRecord);
    if (!record) return;
    if (!at<uint32_t>(record + kRecordCockpit)) {
        refused("the viewed car is not drawn as a cockpit");
        return;
    }
    if (!carWalkInstalled()) {
        refused("the car hook is not installed, so a walk GPL draws could not skip it");
        return;
    }
    // gpl_sceneRender 0x487681..0x4876CD: the segment table, the root GROUP's second child when
    // it has two, and its last entry.
    uintptr_t track = at<uint32_t>(base);
    if (!track) return;
    auto* table = reinterpret_cast<int32_t*>(at<uint32_t>(track + kTrackObject));
    if (!table) return;
    if (table[1] == 2) table = reinterpret_cast<int32_t*>(table[2]);
    if (!table) return;
    int32_t last = table[table[1] + 1];
    int32_t callback = at<int32_t>(kViewedCallbackImmediate);
    reinterpret_cast<EnqueueFn>(kEnqueue)(static_cast<int32_t>(reinterpret_cast<intptr_t>(table)),
                                          last, static_cast<int32_t>(record), callback,
                                          at<float>(record + kRecordDlong));
    s.queued = record;
    if (!s.saidQueued) {
        s.saidQueued = true;
        logf("mirror own car: the viewed car (record %08X) queued before the mirror walk; GPL's "
             "own queueing of it after the walk skips",
             static_cast<unsigned>(record));
    }
}

char __cdecl afterQueueThunk(char discard) {
    queueViewedCar();
    return reinterpret_cast<MatrixPopFn>(kMatrixPop)(discard);
}

int __cdecl viewedQueueThunk(int table, int last, int record, int callback, float dlong) {
    State& s = g_own;
    if (s.queued && static_cast<uintptr_t>(static_cast<uint32_t>(record)) == s.queued) {
        s.queued = 0;
        return 0;
    }
    s.queued = 0;
    return reinterpret_cast<EnqueueFn>(kEnqueue)(table, last, record, callback, dlong);
}

bool redirect(uintptr_t site, const unsigned char stock[5], const void* thunk, const char* what) {
    auto bytes = reinterpret_cast<unsigned char*>(site);
    if (std::memcmp(bytes, stock, 5) != 0) {
        logf("mirror own car: NOT installed -- %08X (%s) holds %02X %02X %02X %02X %02X, not GPL's "
             "call; 3D+car mirrors draw no car",
             static_cast<unsigned>(site), what, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
        return false;
    }
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    if (!patchBytes(bytes + 1, &rel, sizeof(rel))) {
        logf("mirror own car: NOT installed -- could not write the call at %08X (%s)",
             static_cast<unsigned>(site), what);
        return false;
    }
    return true;
}

}  // namespace

void mirrorOwnCarInstall() {
    State& s = g_own;
    if (s.installed) return;
    // Both sites are checked before either is written.
    auto after = reinterpret_cast<const unsigned char*>(kAfterQueueSite);
    auto viewed = reinterpret_cast<const unsigned char*>(kViewedQueueSite);
    if (std::memcmp(after, kAfterQueueStock, 5) != 0 ||
        std::memcmp(viewed, kViewedQueueStock, 5) != 0) {
        logf("mirror own car: NOT installed -- %08X holds %02X %02X %02X %02X %02X and %08X "
             "%02X %02X %02X %02X %02X, not GPL's calls; 3D+car mirrors draw no car",
             static_cast<unsigned>(kAfterQueueSite), after[0], after[1], after[2], after[3],
             after[4], static_cast<unsigned>(kViewedQueueSite), viewed[0], viewed[1], viewed[2],
             viewed[3], viewed[4]);
        return;
    }
    if (!redirect(kViewedQueueSite, kViewedQueueStock,
                  reinterpret_cast<const void*>(&viewedQueueThunk), "the viewed car's queueing"))
        return;
    if (!redirect(kAfterQueueSite, kAfterQueueStock,
                  reinterpret_cast<const void*>(&afterQueueThunk), "the matrix pop after the cars")) {
        // The viewed car's thunk alone passes every call through.
        return;
    }
    s.installed = true;
    logf("mirror own car: installed -- %08X and %08X redirected; with mirrorMode 2 and "
         "mirrorPlayerCar each mirror pane draws the viewed car's outside model, cut at its glass, "
         "its own mirrors left out",
         static_cast<unsigned>(kAfterQueueSite), static_cast<unsigned>(kViewedQueueSite));
}

void mirrorOwnCarPaneSet(const float plane[4], const float shift[3]) {
    State& s = g_own;
    std::memcpy(s.plane, plane, sizeof(s.plane));
    std::memcpy(s.shift, shift, sizeof(s.shift));
    s.paneSet = true;
}

void mirrorOwnCarPaneClear() {
    g_own.paneSet = false;
    g_placement.frameSet = false;
}

void mirrorOwnCarFrameSet(const float origin[3], const float R[9]) {
    PlacementCheck& p = g_placement;
    std::memcpy(p.origin, origin, sizeof(p.origin));
    std::memcpy(p.R, R, sizeof(p.R));
    p.frameSet = true;
}

void mirrorOwnCarPlaced(bool cockpit, const float pose[12]) {
    PlacementCheck& p = g_placement;
    if (!p.frameSet) return;
    // The pose's origin in car axes: R's columns are the car's axes in world.
    float t[3] = {pose[3] - p.origin[0], pose[7] - p.origin[1], pose[11] - p.origin[2]};
    float local[3];
    for (int column = 0; column < 3; ++column)
        local[column] =
            p.R[column] * t[0] + p.R[3 + column] * t[1] + p.R[6 + column] * t[2];
    // The angle between the two rotations: trace(R^T M) = 1 + 2 cos(angle).
    float trace = 0.0f;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            trace += p.R[row * 3 + column] * pose[row * 4 + column];
    float cosine = 0.5f * (trace - 1.0f);
    cosine = cosine > 1.0f ? 1.0f : (cosine < -1.0f ? -1.0f : cosine);
    float turn = std::acos(cosine) * 57.2957795f;

    Placed& m = p.model[cockpit ? 0 : 1];
    ++m.count;
    for (int i = 0; i < 3; ++i) m.sum[i] += local[i];
    float size = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
    if (size > m.worst) m.worst = size;
    if (turn > m.worstTurn) m.worstTurn = turn;

    static LARGE_INTEGER frequency = {};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!p.since.QuadPart) p.since = now;
    if (now.QuadPart - p.since.QuadPart < kPlacementReportSeconds * frequency.QuadPart) return;
    p.since = now;

    char line[2][192] = {"no main pass drew it", "no own-car mirror pass drew it"};
    for (int k = 0; k < 2; ++k) {
        const Placed& q = p.model[k];
        if (!q.count) continue;
        double n = static_cast<double>(q.count);
        _snprintf_s(line[k], sizeof(line[k]), _TRUNCATE,
                    "%+.3f %+.3f %+.3f m (mean of %u; worst %.3f m, turned up to %.2f deg)",
                    q.sum[0] / n, q.sum[1] / n, q.sum[2] / n, q.count, q.worst, q.worstTurn);
        p.model[k] = Placed{};
    }
    logf("mirror placement: from the car frame the glasses are placed in (car axes, X forward), "
         "the cockpit model sat at %s; the outside model at %s",
         line[0], line[1]);
}

bool mirrorOwnCarClip(float plane[4]) {
    State& s = g_own;
    if (!s.paneSet || !mirrorOwnCar()) return false;
    std::memcpy(plane, s.plane, sizeof(s.plane));
    return true;
}

void mirrorOwnCarShift(float shift[3]) {
    State& s = g_own;
    bool   on = s.paneSet && mirrorOwnCar();
    for (int k = 0; k < 3; ++k) shift[k] = on ? s.shift[k] : 0.0f;
}

void mirrorOwnCarWalkBegin(bool drawn) {
    State& s = g_own;
    s.flipped = false;
    s.skipping = false;
    if (!s.queued) return;
    if (drawn) {
        s.savedCockpit = at<uint32_t>(s.queued + kRecordCockpit);
        at<uint32_t>(s.queued + kRecordCockpit) = 0;
        s.flipped = true;
        if (!s.saidDrawn) {
            s.saidDrawn = true;
            logf("mirror own car: first mirror walk with the viewed car as an outside car");
        }
        return;
    }
    s.skipping = true;
    if (!s.saidSkipped) {
        s.saidSkipped = true;
        logf("mirror own car: a mirror walk GPL VR's renderer does not draw (first time) -- the "
             "viewed car is left out of it");
    }
}

void mirrorOwnCarWalkEnd() {
    State& s = g_own;
    if (s.flipped && s.queued) at<uint32_t>(s.queued + kRecordCockpit) = s.savedCockpit;
    s.flipped = false;
    s.skipping = false;
}

bool mirrorOwnCarSkips(const void* record) {
    State& s = g_own;
    return s.skipping && reinterpret_cast<uintptr_t>(record) == s.queued;
}

bool mirrorOwnCarHidesBody(const void* record) {
    State& s = g_own;
    return s.flipped && reinterpret_cast<uintptr_t>(record) == s.queued;
}

}  // namespace gplvr
