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
// thread, after gpl_simStep has updated the view and before bgSleep (0x452B74).
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
//   camera+8   the car the camera rides in (gpl_viewCompute 0x4546D0's car)
constexpr uintptr_t kRetailCameraCar = 0x005FCC1F;

// The car, as gpl_viewCompute reads it for cockpit cameras 1..3:
//   car+780    its body record, the view record's layout (position +0, orientation +84)
//   car+1160   the eye in the body's axes: the team's head rest plus the head's sway, which
//              gpl_ffbFillControlState 0x4A21C0 adds from the player's dynamics each tick
// The view's orientation is body * Euler(look yaw, head pitch, head roll): the yaw is GPL's
// look left/right, pitch and roll the head's smoothed lag behind the body over bumps and kerbs.
constexpr size_t kCarBody = 780;
constexpr size_t kCarEye = 1160;

// sub_46B970, __thiscall (car): the team model's head rest, car+1160 without the sway.
constexpr uintptr_t     kHeadRestFn = 0x0046B970;
constexpr unsigned char kHeadRestHead[7] = {0x8B, 0x81, 0x10, 0x03, 0x00, 0x00, 0x50};
constexpr unsigned char kHeadRestTail[6] = {0x05, 0x1C, 0x01, 0x00, 0x00, 0xC3};
constexpr size_t        kHeadRestTailAt = 0x18;

// A listener slot is 21 packed bytes (gpl_sndListenerCreate 0x4E6F10). +0x11 is the unaligned
// pointer to its data, read as *(entry + 17) by gpl_sndComputePitchPan and by nothing else.
constexpr size_t kSlotData = 0x11;

// The view record: position +0, velocity +12 (per second), orientation +84 (row-major, columns
// forward/left/up). 0xA0 bytes, the block gpl_viewLatchRecord 0x454F80 copies to the renderer.
constexpr size_t kRecordSize = 0xA0;
constexpr size_t kRecordPosition = 0;
constexpr size_t kRecordOrientation = 84;

// gpl_sndComputePitchPan 0x4E6B60 (emitter, listener slot, &pitch, &pan), __cdecl. Its only two
// callers, both inside the scheduler: the new-voice pass and gpl_sndVoiceRetarget 0x4E6CD0.
struct StockCall {
    uintptr_t     site;
    unsigned char bytes[5];
};
constexpr StockCall kPitchPanCalls[2] = {{0x004E678C, {0xE8, 0xCF, 0x03, 0x00, 0x00}},
                                         {0x004E6CE8, {0xE8, 0x73, 0xFE, 0xFF, 0xFF}}};

// An emitter (gpl_sndEmitterCreate 0x4E6100) points at its position at +4.
constexpr size_t kEmitterData = 4;

// The widened pan (vrHeadAudioWidth). The louder side never goes fully silent: at most
// kMaxSeparation of the way to a hard pan, which leaves the quieter ear about -26 dB under Miles'
// pan law. A source this close to the head is pulled toward the centre by half, where GPL's own
// curve uses 2 m and so centres the car's own engine at the driver's seat.
constexpr float kMaxSeparation = 0.95f;
constexpr float kNearField = 0.5f;

// A pose older than this is not applied: the front end, a load, anything that stops the render
// thread drawing tracked frames.
constexpr double kStaleSeconds = 0.2;

// Totals are logged this often while the scheduler runs.
constexpr double kSummarySeconds = 60.0;

// -- the pose, shared between the threads -------------------------------------------------

struct Pose {
    float     rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // the head, in the view record's axes
    float     centre[3] = {0, 0, 0};                     // the head's centre, the same axes
    // vrHeadAudioWidth as the render thread saw it. The VR menu changes the option on that
    // thread, so it reaches the sim thread here, under the same lock as the pose.
    float     width = 0.0f;
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
using PitchPanFn = int(__cdecl*)(void* emitter, unsigned char* listener, float* pitch, float* pan);
// A __thiscall taken as __fastcall: the car in ecx, edx unused.
using HeadRestFn = const float*(__fastcall*)(const void* car, void* unused);

struct State {
    // Set by the install thread, read by the render thread's publish.
    std::atomic<bool> installed{false};
    SchedulerFn       scheduler = nullptr;

    // GPL's pan function. Null when the two calls could not be taken, and then GPL's own pan
    // stands whatever the width. `panInstalled` is read by the VR menu.
    PitchPanFn        pitchPan = nullptr;
    std::atomic<bool> panInstalled{false};

    uintptr_t cameraMode = 0;
    uintptr_t listenerSlot = 0;
    uintptr_t viewRecord = 0;
    uintptr_t cameraCar = 0;

    // Null when sub_46B970 does not hold its stock bytes: the listener then keeps the head's
    // sway and drops only its lean.
    HeadRestFn headRest = nullptr;

    long long frequency = 0;

    // The pose the sim thread last read. Used again when the lock is busy.
    Pose pose;

    // The copy the listener points at during the call. Sim thread only.
    alignas(16) unsigned char shadow[kRecordSize] = {};

    unsigned  counts[kOutcomeCount] = {};
    unsigned  busy = 0;     // ticks that reused the previous pose because the lock was held
    unsigned  widened = 0;  // pans replaced by the wider curve
    unsigned  leaned = 0;   // ticks that kept GPL's lean: the camera had no car
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

// The view without GPL's head motion, for the listener. The body's axes turned by the look yaw
// GPL composed into `view`, so looking left or right still moves the sound with the picture,
// and the eye `position` moved by the head's sway back to the head rest. The headset's own
// rotation and offset go on top of these as they would on the view's. False, with nothing
// written, when the camera has no car.
bool steadyAxes(State& s, const float view[9], float axes[9], float position[3]) {
    auto car = *reinterpret_cast<const unsigned char* const*>(s.cameraCar);
    if (!car) return false;
    auto body = *reinterpret_cast<const unsigned char* const*>(car + kCarBody);
    if (!body) return false;

    float bodyAxes[9];
    std::memcpy(bodyAxes, body + kRecordOrientation, sizeof(bodyAxes));
    float relative[9];
    multiplyTransposed(bodyAxes, view, relative);
    float yaw = std::atan2(relative[1 * 3 + 0], relative[0 * 3 + 0]);
    float c = std::cos(yaw), sn = std::sin(yaw);
    float look[9] = {c, -sn, 0, sn, c, 0, 0, 0, 1};
    multiply(bodyAxes, look, axes);

    if (s.headRest) {
        const float* rest = s.headRest(car, nullptr);
        float eye[3];
        std::memcpy(eye, car + kCarEye, sizeof(eye));
        float sway[3] = {eye[0] - rest[0], eye[1] - rest[1], eye[2] - rest[2]};
        float moved[3];
        apply(bodyAxes, sway, moved);
        for (int i = 0; i < 3; ++i) position[i] -= moved[i];
    }
    return true;
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

    // The axes the head turns in: the car's body turned by GPL's look yaw alone, with the eye
    // moved back to the head rest. The view's own when the camera has no car.
    float axes[9];
    std::memcpy(axes, orientation, sizeof(axes));
    if (!steadyAxes(s, orientation, axes, position)) ++s.leaned;

    float turned[9];
    multiply(axes, s.pose.rotation, turned);
    float offset[3];
    apply(axes, s.pose.centre, offset);
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
         "cockpit %u, no listener %u, slot not the view's %u; the previous pose reused %u times; "
         "%u pans widened; %u ticks kept GPL's head lean (no car)",
         s.counts[kApplied] + s.counts[kNoPose] + s.counts[kStale] + s.counts[kNotCockpit] +
             s.counts[kNoListener] + s.counts[kNotTheView],
         kSummarySeconds, s.counts[kApplied], s.counts[kNoPose], s.counts[kStale],
         s.counts[kNotCockpit], s.counts[kNoListener], s.counts[kNotTheView], s.busy, s.widened,
         s.leaned);
    std::memset(s.counts, 0, sizeof(s.counts));
    s.busy = 0;
    s.widened = 0;
    s.leaned = 0;
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

// The pan for a source at `local` (the listener's axes: +X forward, +Y left) `distance` away.
//
// GPL's own curve is linear in the angle, 0.5 - angle * 0.9/pi, and Miles turns a pan into
// "the far side at 2 x pan of full", so a source 30 degrees off centre leaves the far ear only 3 dB
// down and 45 degrees 5 dB. This one follows the sine of the angle instead, scaled by the width:
// at a width of 1, 30 degrees is about 6 dB, 45 about 10 and 90 about 26. Front and back stay
// folded, as GPL's are: a stereo pan cannot tell them apart.
float widePan(const float local[3], float distance, float width) {
    float across = std::sqrt(local[0] * local[0] + local[1] * local[1]);
    float side = across > 1e-6f ? local[1] / across : 0.0f;  // sin of the angle, + left
    float separation = side * width;
    if (separation > 1.0f) separation = 1.0f;
    if (separation < -1.0f) separation = -1.0f;
    float centring = kNearField / (distance + kNearField);
    return 0.5f - 0.5f * kMaxSeparation * separation * (1.0f - centring);
}

// Stands in for gpl_sndComputePitchPan at both its calls. GPL's own result is kept -- the Doppler
// pitch always, and the pan for any listener but the head, or at a width of 0 -- and only the
// head's pan is replaced. Called inside the scheduler, so on the sim thread, while schedulerHook
// has the head's copy in place and s.pose is this tick's.
int __cdecl pitchPanHook(void* emitter, unsigned char* listener, float* pitch, float* pan) {
    State& s = g_state;
    int    result = s.pitchPan(emitter, listener, pitch, pan);
    if (s.pose.width <= 0.0f) return result;

    uintptr_t data = 0;
    std::memcpy(&data, listener + kSlotData, sizeof(data));
    if (data != reinterpret_cast<uintptr_t>(s.shadow)) return result;

    auto source = *reinterpret_cast<const float* const*>(static_cast<unsigned char*>(emitter) +
                                                           kEmitterData);
    float position[3];
    float orientation[9];
    std::memcpy(position, s.shadow + kRecordPosition, sizeof(position));
    std::memcpy(orientation, s.shadow + kRecordOrientation, sizeof(orientation));

    float toSource[3] = {source[0] - position[0], source[1] - position[1],
                         source[2] - position[2]};
    float distance = std::sqrt(toSource[0] * toSource[0] + toSource[1] * toSource[1] +
                               toSource[2] * toSource[2]);
    float local[3];
    applyTransposed(orientation, toSource, local);
    *pan = widePan(local, distance, s.pose.width);
    ++s.widened;
    return result;
}

float clampWidth(float width) {
    if (width < 0.0f) return 0.0f;
    return width > 2.0f ? 2.0f : width;
}

// The far ear's level for a source `degrees` off centre and far away, under Miles' pan law,
// for the install line.
float farEarDb(float degrees, float width) {
    float radians = degrees / 57.29578f;
    float local[3] = {std::cos(radians), std::sin(radians), 0.0f};
    float pan = widePan(local, 1000.0f, width);
    float gain = 2.0f * (pan < 0.5f ? pan : 1.0f - pan);
    return gain > 1e-4f ? 20.0f * std::log10(gain) : -80.0f;
}

uintptr_t callTarget(uintptr_t site) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const uint8_t*>(site) + 1, sizeof(rel));
    return site + 5 + rel;
}

// One 32-bit store of the displacement: the sim thread may be running the call while it is
// written.
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
    s.cameraCar = rebase(kRetailCameraCar);
    if (!s.cameraMode || !s.listenerSlot || !s.viewRecord || !s.cameraCar) {
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

    auto rest = reinterpret_cast<const uint8_t*>(kHeadRestFn);
    if (std::memcmp(rest, kHeadRestHead, sizeof(kHeadRestHead)) == 0 &&
        std::memcmp(rest + kHeadRestTailAt, kHeadRestTail, sizeof(kHeadRestTail)) == 0)
        s.headRest = reinterpret_cast<HeadRestFn>(kHeadRestFn);
    else
        logf("head audio: %08X does not hold the stock head-rest lookup -- the listener drops "
             "GPL's head lean but keeps its sway",
             static_cast<unsigned>(kHeadRestFn));

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
         "%08X, listener slot pointer %08X; GPL's head lean and sway left out%s",
         static_cast<unsigned>(kSchedulerCallSite), static_cast<unsigned>(s.viewRecord),
         static_cast<unsigned>(s.listenerSlot), s.headRest ? "" : " (lean only)");

    // The wider pan. Both calls or neither, so a voice cannot be started on one curve and
    // retargeted on the other. Taken whatever the width: the VR menu can raise it from 0 later.
    for (const StockCall& call : kPitchPanCalls) {
        auto site = reinterpret_cast<const uint8_t*>(call.site);
        if (std::memcmp(site, call.bytes, sizeof(call.bytes)) != 0) {
            logf("head audio: %08X does not hold its stock `call gpl_sndComputePitchPan` (%02X %02X "
                 "%02X %02X %02X) -- GPL's own pan curve",
                 static_cast<unsigned>(call.site), site[0], site[1], site[2], site[3], site[4]);
            return;
        }
    }
    // Set before either call is redirected: the sim thread may reach one straight away.
    s.pitchPan = reinterpret_cast<PitchPanFn>(callTarget(kPitchPanCalls[0].site));
    if (!redirect(kPitchPanCalls[0].site, reinterpret_cast<uintptr_t>(&pitchPanHook))) {
        logf("head audio: could not redirect the pan call at %08X -- GPL's own pan curve",
             static_cast<unsigned>(kPitchPanCalls[0].site));
        return;
    }
    if (!redirect(kPitchPanCalls[1].site, reinterpret_cast<uintptr_t>(&pitchPanHook))) {
        redirect(kPitchPanCalls[0].site, reinterpret_cast<uintptr_t>(s.pitchPan));
        logf("head audio: could not redirect the pan call at %08X -- GPL's own pan curve",
             static_cast<unsigned>(kPitchPanCalls[1].site));
        return;
    }
    s.panInstalled.store(true, std::memory_order_release);
    float width = clampWidth(g_options.vrHeadAudioWidth);
    if (width <= 0.0f)
        logf("head audio: vrHeadAudioWidth = 0 -- GPL's own pan curve until the VR menu raises it");
    else
        logf("head audio: the head's pan is widened, vrHeadAudioWidth %.2f -- a distant source 30 "
             "/ 45 / 60 / 90 deg off centre leaves the far ear %.1f / %.1f / %.1f / %.1f dB down "
             "(GPL's own: -3.1 / -5.2 / -8.0 / -20.0)",
             width, farEarDb(30.0f, width), farEarDb(45.0f, width), farEarDb(60.0f, width),
             farEarDb(90.0f, width));
}

bool headAudioWidthAdjustable() {
    return g_state.panInstalled.load(std::memory_order_acquire);
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
    pose.width = clampWidth(g_options.vrHeadAudioWidth);
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
