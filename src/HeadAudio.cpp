#include "HeadAudio.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe, report 95) --------------------------------------------------

// gpl_bgTask's call of gpl_soundSchedulerUpdate_perTick 0x477280: once per sim tick, on the sim
// thread, after gpl_simStep has updated the view and before bgSleep (SimRing's site, 0x452B74).
// Its only caller.
constexpr uintptr_t     kSchedulerCallSite = 0x00452B6F;
constexpr unsigned char kSchedulerCallStock[5] = {0xE8, 0x0C, 0x47, 0x02, 0x00};

// The live view, gpl_viewGetLive 0x451110 = 0x5FCC13. Its camera starts at view+4
// (gpl_viewUpdate 0x454350 passes this + 4 to gpl_viewUpdateSoundListener 0x454050):
//   camera+0   the camera mode; 1..3 are the cockpit (gpl_camModeHasMirror 0x4550D0)
//   camera+4   the camera's listener slot, 0 while it has none
//   camera+12  the view record, which gpl_sndListenerCreate is given as the listener's data
// Reference-link addresses, placed by rebase() at install.
constexpr uintptr_t kRetailCameraMode = 0x005FCC17;
constexpr uintptr_t kRetailListenerSlot = 0x005FCC1B;
constexpr uintptr_t kRetailViewRecord = 0x005FCC23;

// A listener slot is 21 packed bytes (gpl_sndListenerCreate 0x4E6F10). +0x11 is the unaligned
// pointer to its data, read as *(entry + 17) by gpl_sndComputePitchPan and by nothing else.
constexpr size_t kSlotData = 0x11;

// The view record: position +0, velocity +12 (per second), orientation +84 (row-major, columns
// forward/left/up). 0xA0 bytes, the block gpl_viewLatchRecord 0x454F80 copies to the renderer.
constexpr size_t kRecordSize = 0xA0;
constexpr size_t kRecordPosition = 0;
constexpr size_t kRecordOrientation = 84;

// A pose older than this is not applied: the front end, a load, anything that stops the render
// thread drawing tracked frames.
constexpr double kStaleSeconds = 0.2;

// Totals are logged this often while the scheduler runs.
constexpr double kSummarySeconds = 60.0;

// -- the pose, shared between the threads -------------------------------------------------

struct Pose {
    float     rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // the head, in the view record's axes
    float     centre[3] = {0, 0, 0};                     // the head's centre, the same axes
    long long qpc = 0;
    bool      valid = false;
};

// Written by the render thread, read by the sim thread, both only while holding g_poseLock.
SRWLOCK g_poseLock = SRWLOCK_INIT;
Pose    g_pose;

// -- the sim thread's own state ------------------------------------------------------------

enum Outcome {
    kApplied = 0,
    kNoPose,       // nothing published, or the last frame had no tracked head
    kStale,        // the newest pose is older than kStaleSeconds
    kNotCockpit,   // a TV or chase camera: GPL's own listener
    kNoListener,   // the view has no listener slot this tick
    kNotTheView,   // the slot's data is not the view record -- refused rather than guessed
    kOutcomeCount
};

const char* const kOutcomeName[kOutcomeCount] = {
    "applied", "no pose", "stale", "not the cockpit", "no listener", "slot not the view's"};

using SchedulerFn = void(__cdecl*)();

struct State {
    // Set by the install thread, read by the render thread's publish.
    std::atomic<bool> installed{false};
    SchedulerFn       scheduler = nullptr;

    uintptr_t cameraMode = 0;
    uintptr_t listenerSlot = 0;
    uintptr_t viewRecord = 0;

    long long frequency = 0;

    // The pose the sim thread last read. Used again when the lock is busy.
    Pose pose;

    // The copy the listener points at during the call. Sim thread only.
    alignas(16) unsigned char shadow[kRecordSize] = {};

    unsigned  counts[kOutcomeCount] = {};
    unsigned  busy = 0;  // ticks that reused the previous pose because the lock was held
    Outcome   last = kOutcomeCount;
    int       transitionReports = 64;
    long long summaryAt = 0;
};

State g_state;

long long now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

// out = a * b, all row-major 3x3.
void multiply(const float a[9], const float b[9], float out[9]) {
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out[row * 3 + column] = a[row * 3 + 0] * b[0 * 3 + column] +
                                    a[row * 3 + 1] * b[1 * 3 + column] +
                                    a[row * 3 + 2] * b[2 * 3 + column];
}

// out = transpose(a) * b.
void multiplyTransposed(const float a[9], const float b[9], float out[9]) {
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out[row * 3 + column] = a[0 * 3 + row] * b[0 * 3 + column] +
                                    a[1 * 3 + row] * b[1 * 3 + column] +
                                    a[2 * 3 + row] * b[2 * 3 + column];
}

void apply(const float m[9], const float v[3], float out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = m[row * 3 + 0] * v[0] + m[row * 3 + 1] * v[1] + m[row * 3 + 2] * v[2];
}

void applyTransposed(const float m[9], const float v[3], float out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = m[0 * 3 + row] * v[0] + m[1 * 3 + row] * v[1] + m[2 * 3 + row] * v[2];
}

bool normalise(float v[3]) {
    float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1e-6f) return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

// The head's rotation from the two eyes'. A runtime with canted displays gives each eye its own
// rotation; the head is between them. Forward and left are averaged and made orthonormal, and up
// is their cross product, so the result is a proper rotation whatever the two eyes held.
bool headRotation(const VrEye eyes[2], float out[9]) {
    float forward[3], left[3];
    for (int i = 0; i < 3; ++i) {
        forward[i] = eyes[0].rotation[i * 3 + 0] + eyes[1].rotation[i * 3 + 0];
        left[i] = eyes[0].rotation[i * 3 + 1] + eyes[1].rotation[i * 3 + 1];
    }
    if (!normalise(forward)) return false;
    float along = left[0] * forward[0] + left[1] * forward[1] + left[2] * forward[2];
    for (int i = 0; i < 3; ++i) left[i] -= along * forward[i];
    if (!normalise(left)) return false;
    float up[3] = {forward[1] * left[2] - forward[2] * left[1],
                   forward[2] * left[0] - forward[0] * left[2],
                   forward[0] * left[1] - forward[1] * left[0]};
    for (int i = 0; i < 3; ++i) {
        out[i * 3 + 0] = forward[i];
        out[i * 3 + 1] = left[i];
        out[i * 3 + 2] = up[i];
    }
    return true;
}

// Degrees the head is turned from straight ahead, positive to the left, for the log.
float yawDegrees(const float rotation[9]) {
    return std::atan2(rotation[1 * 3 + 0], rotation[0 * 3 + 0]) * 57.29578f;
}

// The newest pose, or the previous one when the render thread holds the lock this instant.
void readPose(State& s) {
    if (!TryAcquireSRWLockShared(&g_poseLock)) {
        ++s.busy;
        return;
    }
    s.pose = g_pose;
    ReleaseSRWLockShared(&g_poseLock);
}

// Whether to move the listener this tick, and if so the slot whose pointer to swap, with the
// shadow record already written.
Outcome prepare(State& s, unsigned char*& slot) {
    readPose(s);
    if (!s.pose.valid) return kNoPose;
    if (s.frequency <= 0 ||
        static_cast<double>(now() - s.pose.qpc) / static_cast<double>(s.frequency) >
            kStaleSeconds)
        return kStale;

    int mode = *reinterpret_cast<const int*>(s.cameraMode);
    if (mode < 1 || mode > 3) return kNotCockpit;

    slot = *reinterpret_cast<unsigned char* const*>(s.listenerSlot);
    if (!slot) return kNoListener;
    uintptr_t data = 0;
    std::memcpy(&data, slot + kSlotData, sizeof(data));
    if (data != s.viewRecord) return kNotTheView;

    // The view as this tick left it, then moved to the head: its orientation turned by the
    // head's rotation, its position moved to the head's centre. The velocity stays the car's,
    // so the Doppler shift is unchanged.
    std::memcpy(s.shadow, reinterpret_cast<const void*>(s.viewRecord), kRecordSize);
    float orientation[9];
    float position[3];
    std::memcpy(orientation, s.shadow + kRecordOrientation, sizeof(orientation));
    std::memcpy(position, s.shadow + kRecordPosition, sizeof(position));

    float turned[9];
    multiply(orientation, s.pose.rotation, turned);
    float offset[3];
    apply(orientation, s.pose.centre, offset);
    for (int i = 0; i < 3; ++i) position[i] += offset[i];

    std::memcpy(s.shadow + kRecordOrientation, turned, sizeof(turned));
    std::memcpy(s.shadow + kRecordPosition, position, sizeof(position));
    return kApplied;
}

void note(State& s, Outcome outcome) {
    ++s.counts[outcome];

    if (outcome != s.last && s.transitionReports > 0) {
        --s.transitionReports;
        if (outcome == kApplied)
            logf("head audio: the cockpit listener follows the head (turned %.0f deg, centre "
                 "%.3f %.3f %.3f)",
                 yawDegrees(s.pose.rotation), s.pose.centre[0], s.pose.centre[1],
                 s.pose.centre[2]);
        else
            logf("head audio: GPL's own listener -- %s", kOutcomeName[outcome]);
    }
    s.last = outcome;

    long long t = now();
    if (!s.summaryAt) s.summaryAt = t;
    if (static_cast<double>(t - s.summaryAt) / static_cast<double>(s.frequency) <
        kSummarySeconds)
        return;
    s.summaryAt = t;
    logf("head audio: %u ticks in the last %.0f s -- applied %u, no pose %u, stale %u, not the "
         "cockpit %u, no listener %u, slot not the view's %u; the previous pose reused %u times",
         s.counts[kApplied] + s.counts[kNoPose] + s.counts[kStale] + s.counts[kNotCockpit] +
             s.counts[kNoListener] + s.counts[kNotTheView],
         kSummarySeconds, s.counts[kApplied], s.counts[kNoPose], s.counts[kStale],
         s.counts[kNotCockpit], s.counts[kNoListener], s.counts[kNotTheView], s.busy);
    std::memset(s.counts, 0, sizeof(s.counts));
    s.busy = 0;
}

// Stands in for the scheduler call, on the sim thread.
void __cdecl schedulerHook() {
    State&         s = g_state;
    unsigned char* slot = nullptr;
    Outcome        outcome = prepare(s, slot);
    note(s, outcome);
    if (outcome != kApplied) {
        s.scheduler();
        return;
    }

    auto shadow = reinterpret_cast<uintptr_t>(s.shadow);
    std::memcpy(slot + kSlotData, &shadow, sizeof(shadow));
    s.scheduler();
    std::memcpy(slot + kSlotData, &s.viewRecord, sizeof(s.viewRecord));
}

uintptr_t callTarget(uintptr_t site) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const uint8_t*>(site) + 1, sizeof(rel));
    return site + 5 + rel;
}

// One 32-bit store of the displacement, as SimRing.cpp's redirect: the sim thread may be running
// the call while it is written.
bool redirect(uintptr_t site, uintptr_t to) {
    auto    displacement = reinterpret_cast<volatile int32_t*>(site + 1);
    int32_t rel = static_cast<int32_t>(to - (site + 5));
    DWORD   previous = 0;
    if (!VirtualProtect(const_cast<int32_t*>(displacement), sizeof(rel), PAGE_EXECUTE_READWRITE,
                        &previous))
        return false;
    *displacement = rel;
    DWORD ignored = 0;
    VirtualProtect(const_cast<int32_t*>(displacement), sizeof(rel), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), const_cast<int32_t*>(displacement), sizeof(rel));
    return true;
}

}  // namespace

void headAudioInstall() {
    State& s = g_state;
    if (s.installed) return;
    if (!g_options.vr) return;
    if (!g_options.vrHeadAudio) {
        logf("head audio: vrHeadAudio = 0 -- sound is heard from the driver's eye, as in GPL");
        return;
    }

    s.cameraMode = rebase(kRetailCameraMode);
    s.listenerSlot = rebase(kRetailListenerSlot);
    s.viewRecord = rebase(kRetailViewRecord);
    if (!s.cameraMode || !s.listenerSlot || !s.viewRecord) {
        logf("head audio: the live view's globals cannot be placed in this image (deltas %s) -- "
             "not installed",
             rebaseSummary());
        return;
    }

    auto bytes = reinterpret_cast<const uint8_t*>(kSchedulerCallSite);
    if (std::memcmp(bytes, kSchedulerCallStock, sizeof(kSchedulerCallStock)) != 0) {
        logf("head audio: %08X does not hold the stock `call gpl_soundSchedulerUpdate_perTick` "
             "(%02X %02X %02X %02X %02X) -- not installed",
             static_cast<unsigned>(kSchedulerCallSite), bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4]);
        return;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;
    s.scheduler = reinterpret_cast<SchedulerFn>(callTarget(kSchedulerCallSite));

    if (!redirect(kSchedulerCallSite, reinterpret_cast<uintptr_t>(&schedulerHook))) {
        logf("head audio: could not redirect the scheduler call at %08X -- not installed",
             static_cast<unsigned>(kSchedulerCallSite));
        return;
    }
    s.installed.store(true, std::memory_order_release);
    logf("head audio: installed at %08X -- in the cockpit the listener is the head, view record "
         "%08X, listener slot pointer %08X",
         static_cast<unsigned>(kSchedulerCallSite), static_cast<unsigned>(s.viewRecord),
         static_cast<unsigned>(s.listenerSlot));
}

void headAudioPublish(const float car[9], const float levelled[9], const VrEye eyes[2]) {
    if (!g_state.installed.load(std::memory_order_acquire)) return;

    float head[9];
    if (!headRotation(eyes, head)) return;
    float centre[3];
    for (int i = 0; i < 3; ++i) centre[i] = (eyes[0].offset[i] + eyes[1].offset[i]) * 0.5f;

    // The rendered head is levelled * head in the world, at levelled * centre from the eye.
    // In the view's own axes that is transpose(car) * levelled * head, and the same for the
    // centre; with the horizon lock off, levelled is car and both reduce to the head's own.
    float levelledHead[9];
    multiply(levelled, head, levelledHead);
    Pose pose;
    multiplyTransposed(car, levelledHead, pose.rotation);
    float levelledCentre[3];
    apply(levelled, centre, levelledCentre);
    applyTransposed(car, levelledCentre, pose.centre);
    pose.qpc = now();
    pose.valid = true;

    AcquireSRWLockExclusive(&g_poseLock);
    g_pose = pose;
    ReleaseSRWLockExclusive(&g_poseLock);
}

void headAudioForget() {
    if (!g_state.installed.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_poseLock);
    g_pose.valid = false;
    ReleaseSRWLockExclusive(&g_poseLock);
}

}  // namespace gplvr
