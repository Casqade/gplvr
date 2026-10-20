#include "Interpolation.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"
#include "RenderPacing.h"
#include "SimRing.h"
#include "TaskRate.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// gplc67.exe addresses (IDA primary). The call sites are checked against their stock bytes
// before anything is patched; the data addresses are what the functions at those sites read and
// write.
constexpr uintptr_t kLatchedTickCount = 0x00582114;  // gpl_fgLastLatchedBgCount
// gpl_fgWaitForTickAndSnapshot 0x4E8F20's fgDisableBG call, which opens the latch. RenderPacing
// searches for this; flat mode has no RenderPacing and takes it as an address.
constexpr uintptr_t kFlatTickWaitSite = 0x004E8F51;
constexpr uintptr_t kSegmentCount = 0x006AFB08;      // track segments
constexpr uintptr_t kSegmentTable = 0x006AEA1C;      // float rows: +4 start m, +12 scale, +16 length
constexpr int       kSegmentRowWords = 360;          // 1440 bytes a row
constexpr uintptr_t kLiveCars = 0x006BF080;          // gpl_carLiveGet(0)
constexpr uintptr_t kReplayCars = 0x00790848;        // gpl_carReplayGet(0)
constexpr uintptr_t kCarSize = 1544;
constexpr int       kCarsPerArray = 20;
constexpr int       kCarSlots = 2 * kCarsPerArray;   // live cars 0..19, replay cars 20..39
constexpr uintptr_t kCarRecord = 0x4FC;              // R = &car[319]
constexpr uintptr_t kViewCarDriverIndex = 0x00776144;
constexpr uintptr_t kViewCameraMode = 0x00776164;
constexpr uintptr_t kViewIsLive = 0x00776168;        // byte: 1 live, 0 replay
constexpr uintptr_t kViewRecord = 0x00776175;        // scene params p[3..42]
constexpr uintptr_t kMirrorRecord = 0x00776219;      // scene params p[44..83]
constexpr uintptr_t kPitBoardRecord = 0x0074E990;    // gpl_pitBoardRenderRecord

// The same globals as this image keeps them, resolved once in interpolationInstall and zero
// until then. Every read and write below goes through these rather than the constants above, so
// an image whose data sits elsewhere either resolves completely or does not install at all --
// there is no state in which some of the blend's inputs are this image's and the rest are
// another's.
struct ResolvedAt {
    uintptr_t latchedTickCount = 0;
    uintptr_t segmentCount = 0;
    uintptr_t segmentTable = 0;
    uintptr_t liveCars = 0;
    uintptr_t replayCars = 0;
    uintptr_t viewCarDriverIndex = 0;
    uintptr_t viewCameraMode = 0;
    uintptr_t viewIsLive = 0;
    uintptr_t viewRecord = 0;
    uintptr_t mirrorRecord = 0;
    uintptr_t pitBoardRecord = 0;
};

ResolvedAt g_at;

// Fills g_at, or says which global could not be placed. All eleven or none.
bool resolveAddresses(const char*& missing) {
    struct Entry {
        uintptr_t  reference;
        uintptr_t* out;
        const char* name;
    };
    const Entry entries[] = {
        {kLatchedTickCount, &g_at.latchedTickCount, "gpl_fgLastLatchedBgCount"},
        {kSegmentCount, &g_at.segmentCount, "the segment count"},
        {kSegmentTable, &g_at.segmentTable, "the segment table"},
        {kLiveCars, &g_at.liveCars, "the live car array"},
        {kReplayCars, &g_at.replayCars, "the replay car array"},
        {kViewCarDriverIndex, &g_at.viewCarDriverIndex, "the viewed car's driver index"},
        {kViewCameraMode, &g_at.viewCameraMode, "the camera mode"},
        {kViewIsLive, &g_at.viewIsLive, "the live/replay flag"},
        {kViewRecord, &g_at.viewRecord, "the view record"},
        {kMirrorRecord, &g_at.mirrorRecord, "the mirror record"},
        {kPitBoardRecord, &g_at.pitBoardRecord, "the pit board record"},
    };
    for (const Entry& entry : entries) {
        uintptr_t at = rebase(entry.reference);
        if (!at) {
            missing = entry.name;
            g_at = ResolvedAt();
            return false;
        }
        *entry.out = at;
    }
    return true;
}

struct StockCall {
    uintptr_t     site;
    unsigned char bytes[5];
};

// gpl_simFrameSetup 0x48A9A0: gpl_carLatchRenderRecord 0x46D030 in the two loops over the other
// cars (live and replay arrays) and for the viewed car; gpl_viewLatchRecord 0x454F80 into
// 0x776175; gpl_viewGetMirrorRecord 0x454FB0 into 0x776219; gpl_pitBoardGetRenderRecord 0x47FE10
// into 0x74E990 (live branch only, report 58).
constexpr StockCall kCarLatchCalls[3] = {{0x0048AE95, {0xE8, 0x96, 0x21, 0xFE, 0xFF}},
                                          {0x0048AF7A, {0xE8, 0xB1, 0x20, 0xFE, 0xFF}},
                                          {0x0048B024, {0xE8, 0x07, 0x20, 0xFE, 0xFF}}};
constexpr StockCall kViewLatchCall = {0x0048B065, {0xE8, 0x16, 0x9F, 0xFC, 0xFF}};
constexpr StockCall kMirrorLatchCall = {0x0048B09F, {0xE8, 0x0C, 0x9F, 0xFC, 0xFF}};
constexpr StockCall kPitBoardLatchCall = {0x0048AEE2, {0xE8, 0x29, 0x4F, 0xFF, 0xFF}};

enum Blend {
    kKeep,    // the newest tick
    kLinear,  // straight line
    kAngle,   // radians, the short way round
    kSpin,    // radians, the short way round when under pi/2 between the ticks, else the newest
    kDlong,   // metres along the track, unwrapped at the lap length
    kMatrix,  // 3x3 rotation, row-major, slerped through a quaternion
    kQuat,    // quaternion x, y, z, w, slerped; `matrix` is rebuilt from it
};

// THE RECORD'S OWN VELOCITIES ARE NOT USED, and what is known about them is worth keeping.
//
// The view and mirror records carry the sim's published linear velocity at +12 and its angular
// velocity at +136, both `kKeep`. Measured and decoded:
//
//   - gpl_viewRecordSetVelocity 0x4E3A90 copies phys+12..20 in verbatim, no scaling.
//   - Both are per SECOND, not per tick: the eye's move over one tick is 0.03 of the published
//     magnitude and 1/36 is 0.028, and gpl_rbIntegrateEuler integrates with dt = 1/36 s.
//   - gpl_aiPredictOneTickBallistic hands the rigid body's angular velocity into the +136 slot
//     beside +244's linear one, and gpl_netCarRecordStampLocalSim takes the four as
//     (position, orientation, linear, angular).
//   - GPL's own orientation step is h = 0.5 * dt * omega, q' = normalise(q + q (x) h), where the
//     product is q (x) h -- sub_4E4D10 0x4E4D10 gives result.xyz = q.w * h + q x h.
//   - Cars carry NEITHER velocity, so nothing of this applies to them.
//
// They were used for a tick of extrapolation and it worked -- the linear scaling measured 1.00 and
// the angular step sat within a quarter of the arc's own reach -- but the arc's reach past a tick
// is only about 0.2 degrees, so the whole mechanism is sub-degree and could not be the jolt it was
// reached for. It is out again with the forward targeting that needed it.

struct Field {
    int         offset;  // bytes from the record's base
    int         words;
    Blend       blend;
    const char* name;
    int         matrix = -1;  // kQuat: the offset of the matrix gpl_quatToMatrix derives from it
};

// The car render record R = car+0x4FC, R+4k = car[319+k]. Mapped from gpl_carLatchRenderRecord
// 0x46D030, its only writer, and its readers gpl_carRecordDraw 0x486E50,
// gpl_carModelPoseFromRecord 0x46D490, gpl_carModelSetWheelSpinLod 0x46D850,
// gpl_carModelApplyWheelLoss 0x406F60, gpl_carModelPoseShiftHand 0x46D8D0 and
// gpl_carDrawTrackProjected 0x487060. Floats unless noted; angles in radians (the draw compares
// R+144 against pi/2).
//
//   R+    car[]      field                                         blend
//   4     320        0                                             keep
//   8     321        back-pointer to the car                       keep
//   12    322        dlong, metres (phys+60)                       unwrapped at the lap length
//   16    323..325   position (phys+0..8)                          linear
//   28    326        3.0                                           keep
//   32    327        int driver index (not written by the latch)   keep, snap on change
//   36    328        dlat (phys+64)                                linear
//   40    329        int segment hint (phys+80)                    keep
//   44    330        int dial range index                          keep, snap on change
//   48    331        int cockpit/mirror flag, set by the caller    keep
//   52    332        model handle                                  keep, snap on change
//   56    333..341   3x3 matrix (phys+84), no reader in the draw   rotation
//   92    342        int (car[275]>>3)&3                           keep, snap on change
//   96    343        int material register value (car[40])        keep
//   100   344..350   shift hand target, weight at R+112            linear
//   128   351..352   shifter angles                                linear
//   136   353..355   body eulers                                   angle
//   148   356..359   wheel heights                                 linear
//   164   360..363   wheel spin angles                             spin
//   180   364..367   wheel rates, LOD steps at |x| 20 and 60       keep
//   196   368        steer                                         linear
//   200   369..371   head position                                 linear
//   212   372..374   head yaw (unread), pitch, roll                angle
//   224   375..381   dial values                                   linear
//   252   382..385   int wheel-state words (wheel loss)            keep, snap on change
constexpr int kCarWords = 66;  // car[320..385]
constexpr Field kCarFields[] = {
    {4, 2, kKeep, "zero/car"},         {12, 1, kDlong, "dlong"},
    {16, 3, kLinear, "position"},      {28, 2, kKeep, "3.0/driver"},
    {36, 1, kLinear, "dlat"},          {40, 4, kKeep, "segment/dials/cockpit/model"},
    {56, 9, kMatrix, "matrix"},        {92, 2, kKeep, "flags/material"},
    {100, 7, kLinear, "shift hand"},   {128, 2, kLinear, "shifter"},
    {136, 3, kAngle, "body eulers"},   {148, 4, kLinear, "wheel heights"},
    {164, 4, kSpin, "wheel spin"},     {180, 4, kKeep, "wheel rates"},
    {196, 1, kLinear, "steer"},        {200, 3, kLinear, "head position"},
    {212, 3, kAngle, "head angles"},   {224, 7, kLinear, "dials"},
    {252, 4, kKeep, "wheel state"},
};
// A change here means the two copies describe DIFFERENT objects, so a blend between them is
// meaningless and the pair snaps: R+32 is the driver index and R+52 the model handle. The other
// words the record keeps -- the dial range R+44, the two bits of R+92, the wheel-state words at
// R+252..264 -- are drawn-state selectors. They are `kKeep` fields, so a blended frame already
// takes the newest value of each; snapping the whole record when one of them changes throws away
// a position blend that was still valid. R+92 is `(car[275] >> 3) & 3`, recomputed from live
// state every tick, and it was the likeliest single source of the snaps that put the camera and
// the car at different ticks (report 78).
constexpr int kCarIdentity[] = {32, 52};

// The view record, 0xA0 bytes copied by gpl_viewLatchRecord 0x454F80 from the view object +16
// into scene params p[3..42]; the mirror record at p[44..83] (gpl_viewGetMirrorRecord 0x454FB0)
// has the same layout. Written by gpl_viewCompute 0x4546D0 through gpl_viewRecordSetPosition
// (-> sub_4E3990), gpl_viewRecordSetTrackPosition 0x4E3A10, gpl_viewRecordSetVelocity 0x4E3A90
// and gpl_viewRecordSetOrientationQuat 0x4E3B20; read by gpl_sceneRender 0x487620 and by
// Stereo.cpp for the eye cameras.
//
//   r+    p[]      field                                            blend
//   0     3..5     eye position                                     linear
//   12    6..8     velocity (from phys+12)                          keep
//   24    9..11    track point, +32 ground height (sceneRender)     linear
//   36    12..15   sub_461970 outputs                               keep
//   52    16..17   track direction (sceneRender atan2(p17, p16))    linear
//   60    18       dlong, metres                                    unwrapped at the lap length
//   64    19       dlat                                             linear
//   68    20..21   velocity along and across the track              keep
//   76    22..23   int, int segment hint (sceneRender (u16)p[23])   keep
//   84    24..32   orientation, gpl_quatToMatrix(r+120)             rebuilt from the quaternion
//   120   33..36   orientation quaternion x, y, z, w                slerp
//   136   37..39   from phys+136                                    keep
//   148   40..42   fov, cx, cy                                      keep
constexpr int kViewWords = 40;
constexpr Field kViewFields[] = {
    {0, 3, kLinear, "eye"},              {12, 3, kKeep, "velocity"},
    {24, 3, kLinear, "track point"},     {36, 4, kKeep, "track outputs"},
    {52, 2, kLinear, "track direction"}, {60, 1, kDlong, "dlong"},
    {64, 1, kLinear, "dlat"},            {68, 4, kKeep, "track velocity/segment"},
    {84, 9, kKeep, "orientation"},       {120, 4, kQuat, "quaternion", 84},
    {136, 6, kKeep, "spin/fov/cx/cy"},
};

// The pit board record 0x74E990, filled by gpl_pitBoardGetRenderRecord 0x47FE10 from
// gpl_pitBoardPos 0x74E8A8..B4, which gpl_pitBoardTrackCar_perTick 0x480960 moves on bg a tick at a
// time. While the board is hidden the latch returns 0 and the record is not in the draw list.
// gpl_pitBoardRecordDraw 0x47F2B0 reads only the position; the node keeps its own orientation.
// Nothing but the latch reads or writes r+4..r+27 (xrefs); r+0 is the vtable (sub_47F240).
//
//   r+    field                             blend
//   4     int 4, written by the latch       keep, snap on change
//   8     no reader or writer               keep
//   12    dlong, metres (0x74E8B4)          unwrapped at the lap length
//   16    position (0x74E8A8..B0)           linear
constexpr int kPitBoardWords = 6;
constexpr Field kPitBoardFields[] = {
    {4, 2, kKeep, "type"}, {12, 1, kDlong, "dlong"}, {16, 3, kLinear, "position"},
};

// Two latched ticks further apart than this are not blended.
constexpr int   kMaxBlendTicks = 4;
constexpr float kSnapMetres = 5.0f;
constexpr float kSnapTurnRadians = 0.5f;
constexpr float kPi = 3.14159265f;
// Metres. A linear or dlong value past it comes from a diverged sim body (report 50).
constexpr float kMaxBlendMagnitude = 1e6f;

enum Snap { kSnapFirst, kSnapIdentity, kSnapGap, kSnapJump, kSnapCamera, kSnapNonFinite, kSnaps };

// One record's two newest latched copies. words[0] is the older, words[1] the newest.
//
// A THIRD COPY WAS TRIED AND REVERTED. The target rides the presenter's display grid
// while the copies ride the sim's latch grid, so it lands a few milliseconds before words[0] about
// one frame in five and is clamped there, freezing the world for that frame. Keeping the tick
// before words[0] let those frames blend against a pair that brackets them, and it worked --
// clamped low went to 0 and alpha's mean from 0.33 to 0.51.
//
// It is out because the frames it fixed were not the problem. The decision has to be taken once
// for the WHOLE frame or records drawn from different pairs disagree with each other, which is a
// worse artifact than the freeze; taken that way it still did not settle the jolt, and the driver
// preferred the stepping. Anyone rebuilding it needs the frame-level decision from the start.
struct History {
    uint32_t words[2][kCarWords] = {};
    int      ticks[2] = {};
    int      copies = 0;
    bool     snapped = false;       // this pair is drawn at the newest copy
    float    stepPerTick = -1.0f;   // position change a tick across the pair, -1 unknown
    int      identity[3] = {};      // view: camera mode, view car, live flag
};

struct Window {
    unsigned  frames = 0;  // latches that published a world frame
    unsigned  blended = 0;
    unsigned  frozen = 0;
    unsigned  untimed = 0;
    unsigned  presenterTimed = 0;
    unsigned  costTimed = 0;
    double    alphaSum = 0.0;
    double    alphaMin = 0.0;
    double    alphaMax = 0.0;
    unsigned  alphas = 0;
    unsigned  extraFrames = 0;   // frames drawn between two ticks (frameRate)
    unsigned  extraBlended = 0;  // of those, the ones that found a usable pair
    long long extraCostTicks = 0;
    unsigned  clampedLow = 0;
    unsigned  clampedHigh = 0;
    unsigned  targetHeldBack = 0;    // frames whose target regressed and was held instead

    // How far outside the pair the target was when it clamped, and how often the alpha reported
    // for the frame was the VIEW's rather than some other record's.
    //
    // These two settled a contradiction and are worth their width: the clamp is the
    // view's own (70 of 70) and the excursion is real (7 to 11 ms of a 27.78 ms window), which is
    // what ruled out both "the counter is measuring another record" and "it is floating-point dust
    // at the boundary". Any future claim about the clamp has to survive these.
    unsigned  sampledView = 0;
    double    clampedWorstMs = 0.0;


    // How evenly the sampled world instant advanced, in ms, on frames that kept the same pair as
    // the frame before and on frames that changed pair.
    unsigned  steadySteps = 0;
    double    steadyMin = 0.0;
    double    steadyMax = 0.0;
    unsigned  snaps[kSnaps] = {};
    unsigned  gaps[4] = {};  // view pair: 1, 2, 3, 4+ ticks
    unsigned  carsBlended = 0;
    unsigned  viewBlended = 0;
    unsigned  mirrorBlended = 0;
    unsigned  pitBoardBlended = 0;
    unsigned  pitBoardSnaps = 0;  // also in snaps[]
    unsigned  viewCarCoupled = 0;  // frames where the view and the viewed car were snapped together
    unsigned  viewMirrorCoupled = 0;  // the same for the view and the mirror record
    long long costTicks = 0;

};

using CountFn = int(__cdecl*)();

struct State {
    bool      installed = false;
    long long frequency = 0;
    CountFn   getBGCount = nullptr;

    History cars[kCarSlots];
    History view;
    History mirror;
    History pitBoard;

    // Filled by the hooks inside this latch, consumed after it.
    int  latched[kCarSlots] = {};
    int  latchedCount = 0;
    int  viewedSlot = -1;   // the car the view record is attached to, -1 when it was not latched

    // The same list, kept past the latch so an extra frame can re-blend the cars the last latch
    // touched. `latched` is cleared when the latch ends; this is not.
    int  blendCars[kCarSlots] = {};
    int  blendCarCount = 0;
    bool blendMirror = false;
    bool blendPitBoard = false;
    bool blendable = false;  // the last latch produced a pair worth re-blending
    float    lastAlpha = 0.0f;
    int      lastTickGap = 0;

    // THE SAMPLED INSTANT'S OWN SMOOTHNESS, which no alpha figure can show.
    //
    // alpha is a position within a pair, so it says nothing about the world time the frame was
    // drawn for once the pair changes underneath it. What the eye sees is that instant advancing
    // evenly: at 90 Hz it should step 11.11 ms every frame, whatever alpha or which pair produced
    // it. An uneven step is judder of the whole picture, because the eye position is itself a
    // blended field.
    //
    // Split by whether the pair changed this frame. The older-pair fallback makes about one frame
    // in five blend from (T-1, T0) and the rest from (T0, T1), so it switches roughly 36 times a
    // second, and the two pairs' ends come from tickArrival's MODEL rather than from one clock. If
    // the model places them inconsistently, every switch is a step -- which would be a judder the
    // fallback introduced, and the driver reports the whole-frame stutter appearing with it.
    double   lastTarget = 0.0;
    bool     haveLastTarget = false;

    // THE TARGET MUST NEVER GO BACKWARDS, and measured it sometimes did.
    //
    // The drawn instant stepped -0.24 ms on one frame and 131 ms on another, in six of 37 windows,
    // about once every six seconds -- the same rate as the hiccups. A negative step draws the world
    // BEFORE where the last frame drew it, and the frame after jumps two periods to catch up.
    //
    // It comes from the quantisation in renderPacingDisplayTime: the display is a presenter latch
    // plus a whole number of periods, and when the presenter's own latch time updates between two
    // frames that number can recompute one period lower. Real time does not do that, so the
    // regression is arithmetic rather than a real event, and holding the previous target is closer
    // to the truth than following it back.
    double   guardTarget = 0.0;
    bool     haveGuardTarget = false;
    bool viewLatched = false;
    bool mirrorLatched = false;
    bool pitBoardLatched = false;

    int  lastTick = 0;
    bool haveLastTick = false;

    // Display time less the time right after the latch, smoothed, in QPC ticks.
    double latency = 0.0;
    bool   haveLatency = false;


    // When the ticks arrive, measured here rather than taken from RenderPacing, which is a VR
    // module and is not installed in flat mode. One latch to the next, smoothed, with the tick
    // it belonged to, so any tick number can be dated: arrival(n) = lastLatchQpc +
    // (n - lastLatchTick) * period.
    long long lastLatchQpc = 0;
    int       lastLatchTick = 0;
    double    tickPeriod = 0.0;
    bool      haveTickModel = false;

    Window window;
};

State g_state;

// -- arithmetic -------------------------------------------------------------------------------

struct Quat {
    float x, y, z, w;
};

float floatAt(const uint32_t* words, int base, int offset) {
    float value;
    std::memcpy(&value, &words[(offset - base) / 4], sizeof(value));
    return value;
}

void storeFloat(uint8_t* record, int offset, float value) {
    std::memcpy(record + offset, &value, sizeof(value));
}

float wrapPi(float angle) {
    float wrapped = std::fmod(angle + kPi, 2.0f * kPi);
    if (wrapped < 0.0f) wrapped += 2.0f * kPi;
    return wrapped - kPi;
}

// gpl_quatToMatrix 0x4E48D0: row-major, m[1] = 2(xy - wz).
void matrixFromQuat(const Quat& q, float m[9]) {
    m[0] = 2.0f * (q.x * q.x + q.w * q.w) - 1.0f;
    m[1] = 2.0f * (q.x * q.y - q.w * q.z);
    m[2] = 2.0f * (q.x * q.z + q.w * q.y);
    m[3] = 2.0f * (q.x * q.y + q.w * q.z);
    m[4] = 2.0f * (q.y * q.y + q.w * q.w) - 1.0f;
    m[5] = 2.0f * (q.y * q.z - q.w * q.x);
    m[6] = 2.0f * (q.x * q.z - q.w * q.y);
    m[7] = 2.0f * (q.y * q.z + q.w * q.x);
    m[8] = 2.0f * (q.z * q.z + q.w * q.w) - 1.0f;
}

// The inverse of matrixFromQuat. False for a matrix that is not a rotation.
bool quatFromMatrix(const float m[9], Quat& q) {
    float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!(std::fabs(det - 1.0f) < 0.05f)) return false;
    float trace = m[0] + m[4] + m[8];
    if (trace > 0.0f) {
        float s = 2.0f * std::sqrt(trace + 1.0f);
        q = {(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, 0.25f * s};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = 2.0f * std::sqrt(1.0f + m[0] - m[4] - m[8]);
        q = {0.25f * s, (m[3] + m[1]) / s, (m[2] + m[6]) / s, (m[7] - m[5]) / s};
    } else if (m[4] > m[8]) {
        float s = 2.0f * std::sqrt(1.0f + m[4] - m[0] - m[8]);
        q = {(m[3] + m[1]) / s, 0.25f * s, (m[7] + m[5]) / s, (m[2] - m[6]) / s};
    } else {
        float s = 2.0f * std::sqrt(1.0f + m[8] - m[0] - m[4]);
        q = {(m[2] + m[6]) / s, (m[7] + m[5]) / s, 0.25f * s, (m[3] - m[1]) / s};
    }
    return true;
}

bool unitQuat(const Quat& q) {
    float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::fabs(norm - 1.0f) < 0.05f;
}

float quatDot(const Quat& a, const Quat& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// The rotation between two unit quaternions, radians.
float quatTurn(const Quat& a, const Quat& b) {
    float dot = std::fabs(quatDot(a, b));
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * std::acos(dot);
}

// From `from` (a = 0) to `to` (a = 1), the short way.
Quat slerp(Quat from, const Quat& to, float a) {
    float dot = quatDot(from, to);
    if (dot < 0.0f) {
        from = {-from.x, -from.y, -from.z, -from.w};
        dot = -dot;
    }
    float wFrom = 1.0f - a;
    float wTo = a;
    if (dot < 0.9995f) {
        float theta = std::acos(dot);
        float sinTheta = std::sin(theta);
        wFrom = std::sin((1.0f - a) * theta) / sinTheta;
        wTo = std::sin(a * theta) / sinTheta;
    }
    Quat q = {wFrom * from.x + wTo * to.x, wFrom * from.y + wTo * to.y,
              wFrom * from.z + wTo * to.z, wFrom * from.w + wTo * to.w};
    float norm = std::sqrt(quatDot(q, q));
    if (norm > 0.0f) q = {q.x / norm, q.y / norm, q.z / norm, q.w / norm};
    return q;
}

Quat quatAt(const uint32_t* words, int base, int offset) {
    return {floatAt(words, base, offset), floatAt(words, base, offset + 4),
            floatAt(words, base, offset + 8), floatAt(words, base, offset + 12)};
}

void matrixAt(const uint32_t* words, int base, int offset, float m[9]) {
    for (int k = 0; k < 9; ++k) m[k] = floatAt(words, base, offset + 4 * k);
}

bool matrixMatchesQuat(const float m[9], const Quat& q) {
    float derived[9];
    matrixFromQuat(q, derived);
    for (int k = 0; k < 9; ++k)
        if (!(std::fabs(derived[k] - m[k]) < 1e-3f)) return false;
    return true;
}

// Where the float dlong wraps, in metres: the end of the last track segment, as the shadow's
// segment search (sub_463C00) measures it. That search never terminates for a dlong past the last
// segment (report 49). 0 when the table does not hold a plausible length.
float trackLengthMetres() {
    // Zero until interpolationInstall has placed them on this image, and this dereferences a
    // pointer and then indexes off it.
    if (!g_at.segmentCount || !g_at.segmentTable) return 0.0f;
    int          count = *reinterpret_cast<const int*>(g_at.segmentCount);
    const float* table = *reinterpret_cast<const float* const*>(g_at.segmentTable);
    if (count <= 0 || count > 4096 || !table) return 0.0f;
    const float* row = table + kSegmentRowWords * (count - 1);
    if (!(row[3] > 0.0f)) return 0.0f;
    float end = row[1] + row[4] / row[3];
    return end > 100.0f && end < 100000.0f ? end : 0.0f;
}

float blendDlong(float older, float newest, float a, float length) {
    float d = newest - older;
    if (length > 0.0f) {
        if (d > 0.5f * length) d -= length;
        else if (d < -0.5f * length) d += length;
    } else if (std::fabs(d) > 50.0f) {
        return newest;
    }
    float value = newest - (1.0f - a) * d;
    if (length > 0.0f) {
        if (!(newest >= 0.0f && newest < length)) return newest;
        if (value < 0.0f) value += length;
        else if (value >= length) value -= length;
        if (!(value >= 0.0f && value < length)) value = newest;
    }
    return value;
}

// A matrix written apart from its quaternion is blended by itself instead; counted because a
// rising count means the two are no longer describing the same rotation.
unsigned g_rotationFallbacks = 0;

// Rotation fields: the matrix, or the quaternion and the matrix derived from it.
void blendRotation(const uint32_t* lo, const uint32_t* hi, uint8_t* record, int base,
                   const Field& field, float a) {
    if (field.blend == kQuat) {
        Quat older = quatAt(lo, base, field.offset);
        Quat newest = quatAt(hi, base, field.offset);
        float olderMatrix[9], newestMatrix[9];
        matrixAt(lo, base, field.matrix, olderMatrix);
        matrixAt(hi, base, field.matrix, newestMatrix);
        // A matrix written apart from its quaternion is blended by itself instead.
        if (!unitQuat(older) || !unitQuat(newest) || !matrixMatchesQuat(olderMatrix, older) ||
            !matrixMatchesQuat(newestMatrix, newest)) {
            ++g_rotationFallbacks;
            if (!quatFromMatrix(olderMatrix, older) || !quatFromMatrix(newestMatrix, newest)) return;
            float blended[9];
            matrixFromQuat(slerp(older, newest, a), blended);

            std::memcpy(record + field.matrix, blended, sizeof(blended));
            return;
        }
        Quat q = slerp(older, newest, a);
        storeFloat(record, field.offset, q.x);
        storeFloat(record, field.offset + 4, q.y);
        storeFloat(record, field.offset + 8, q.z);
        storeFloat(record, field.offset + 12, q.w);
        float m[9];
        matrixFromQuat(q, m);

        std::memcpy(record + field.matrix, m, sizeof(m));
        return;
    }
    float olderMatrix[9], newestMatrix[9];
    matrixAt(lo, base, field.offset, olderMatrix);
    matrixAt(hi, base, field.offset, newestMatrix);
    Quat older, newest;
    if (!quatFromMatrix(olderMatrix, older) || !quatFromMatrix(newestMatrix, newest)) return;
    float blended[9];
    matrixFromQuat(slerp(older, newest, a), blended);

    std::memcpy(record + field.offset, blended, sizeof(blended));
}

// Writes every blended field of `record` from the pair, at `a` from the older copy to the
// newest. Kept fields are left as the latch wrote them, which is the newest copy.
void blendFields(const uint32_t* lo, const uint32_t* hi, uint8_t* record, int base,
                 const Field* fields, size_t count, float a, float length) {
    for (size_t i = 0; i < count; ++i) {
        const Field& field = fields[i];
        if (field.blend == kKeep) continue;
        if (field.blend == kMatrix || field.blend == kQuat) {
            blendRotation(lo, hi, record, base, field, a);
            continue;
        }
        for (int k = 0; k < field.words; ++k) {
            int   offset = field.offset + 4 * k;
            float older = floatAt(lo, base, offset);
            float newest = floatAt(hi, base, offset);
            float d = newest - older;
            float value = newest;
            switch (field.blend) {
                case kLinear:
                    value = newest - (1.0f - a) * d;
                    break;
                case kAngle:
                    value = newest - (1.0f - a) * wrapPi(d);
                    break;
                case kSpin:
                    d = wrapPi(d);
                    if (std::fabs(d) < 0.5f * kPi) value = newest - (1.0f - a) * d;
                    break;
                case kDlong:
                    value = blendDlong(older, newest, a, length);
                    break;
                default:
                    break;
            }
            storeFloat(record, offset, value);
        }
    }
}

// -- history ----------------------------------------------------------------------------------

void shift(History& h, const uint8_t* words, int count, int tick) {
    if (h.copies > 0) {
        std::memcpy(h.words[0], h.words[1], sizeof(uint32_t) * count);
        h.ticks[0] = h.ticks[1];
    }
    std::memcpy(h.words[1], words, sizeof(uint32_t) * count);
    h.ticks[1] = tick;
    if (h.copies < 2) ++h.copies;
}


uint8_t* carRecord(int slot) {
    uintptr_t array = slot < kCarsPerArray ? g_at.liveCars : g_at.replayCars;
    return reinterpret_cast<uint8_t*>(array + (slot % kCarsPerArray) * kCarSize + kCarRecord);
}

int carSlot(uintptr_t car) {
    uintptr_t span = kCarSize * kCarsPerArray;
    if (car >= g_at.liveCars && car < g_at.liveCars + span && (car - g_at.liveCars) % kCarSize == 0)
        return static_cast<int>((car - g_at.liveCars) / kCarSize);
    if (car >= g_at.replayCars && car < g_at.replayCars + span && (car - g_at.replayCars) % kCarSize == 0)
        return kCarsPerArray + static_cast<int>((car - g_at.replayCars) / kCarSize);
    return -1;
}

bool wordsSane(const uint32_t* words, int base, int offset, int count, bool bounded) {
    for (int k = 0; k < count; ++k) {
        float value = floatAt(words, base, offset + 4 * k);
        if (!std::isfinite(value) || (bounded && std::fabs(value) > kMaxBlendMagnitude))
            return false;
    }
    return true;
}

// False when a blended value of either copy -- a quaternion's matrix included -- is NaN or
// infinite, or a linear or dlong value lies past kMaxBlendMagnitude. A diverged sim body latches
// such values (report 50), and the pair is then drawn at the newest copy as the latch wrote it.
bool pairSane(const History& h, int base, const Field* fields, size_t count) {
    for (int copy = 0; copy < 2; ++copy) {
        for (size_t i = 0; i < count; ++i) {
            const Field& field = fields[i];
            if (field.blend == kKeep) continue;
            bool bounded = field.blend == kLinear || field.blend == kDlong;
            if (!wordsSane(h.words[copy], base, field.offset, field.words, bounded)) return false;
            if (field.blend == kQuat && !wordsSane(h.words[copy], base, field.matrix, 9, false))
                return false;
        }
    }
    return true;
}

// This latch's copy of a car record becomes the newest of its pair; decide whether the pair may
// be blended.
// The car the view record is attached to. Its driver index is R+32 of the car record, which is
// the same number the view record carries at 0x776144; live cars hold slots 0..19 and replay cars
// 20..39. -1 when the viewed car was not latched this frame (a pit or menu camera).
int viewedCarSlot(const State& s, int driverIndex, bool live) {
    int first = live ? 0 : kCarsPerArray;
    int last = live ? kCarsPerArray : kCarSlots;
    for (int i = 0; i < s.latchedCount; ++i) {
        int slot = s.latched[i];
        if (slot < first || slot >= last) continue;
        const History& h = s.cars[slot];
        if (h.copies > 0 && static_cast<int>(h.words[1][(32 - 4) / 4]) == driverIndex) return slot;
    }
    return -1;
}

void pushCar(State& s, History& h, const uint8_t* record, int tick) {
    const uint8_t* words = record + 4;
    if (h.copies > 0 && h.ticks[1] == tick) {
        std::memcpy(h.words[1], words, sizeof(uint32_t) * kCarWords);
        if (h.copies == 2 && !h.snapped &&
            !pairSane(h, 4, kCarFields, sizeof(kCarFields) / sizeof(kCarFields[0]))) {
            h.snapped = true;
            h.stepPerTick = -1.0f;
            ++s.window.snaps[kSnapNonFinite];
        }
        return;
    }
    shift(h, words, kCarWords, tick);
    Window& w = s.window;
    if (h.copies < 2) {
        h.snapped = true;
        h.stepPerTick = -1.0f;
        ++w.snaps[kSnapFirst];
        return;
    }

    int   gap = h.ticks[1] - h.ticks[0];
    int   reason = -1;
    float perTick = -1.0f;
    if (gap < 1 || gap > kMaxBlendTicks) {
        reason = kSnapGap;
    } else {
        for (int offset : kCarIdentity)
            if (h.words[0][(offset - 4) / 4] != h.words[1][(offset - 4) / 4]) reason = kSnapIdentity;
    }
    if (reason < 0 && !pairSane(h, 4, kCarFields, sizeof(kCarFields) / sizeof(kCarFields[0])))
        reason = kSnapNonFinite;
    if (reason < 0) {
        float dx = floatAt(h.words[1], 4, 16) - floatAt(h.words[0], 4, 16);
        float dy = floatAt(h.words[1], 4, 20) - floatAt(h.words[0], 4, 20);
        float dz = floatAt(h.words[1], 4, 24) - floatAt(h.words[0], 4, 24);
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        perTick = distance / gap;
        // A pit teleport, a reset or a replay seek: further than a car goes between ticks.
        if (!(distance <= kSnapMetres) ||
            (h.stepPerTick >= 0.0f && perTick > 3.0f * h.stepPerTick + 0.25f))
            reason = kSnapJump;
    }
    h.snapped = reason >= 0;
    h.stepPerTick = h.snapped ? -1.0f : perTick;
    if (h.snapped) ++w.snaps[reason];
}

void pushView(State& s, History& h, const uint8_t* record, int tick, const int identity[3],
              bool countGap) {
    Window& w = s.window;
    bool    cut = h.copies > 0 && std::memcmp(h.identity, identity, sizeof(h.identity)) != 0;
    std::memcpy(h.identity, identity, sizeof(h.identity));
    if (h.copies > 0 && h.ticks[1] == tick) {
        std::memcpy(h.words[1], record, sizeof(uint32_t) * kViewWords);
        if (cut) {
            h.snapped = true;
            ++w.snaps[kSnapCamera];
        } else if (h.copies == 2 && !h.snapped &&
                   !pairSane(h, 0, kViewFields, sizeof(kViewFields) / sizeof(kViewFields[0]))) {
            h.snapped = true;
            ++w.snaps[kSnapNonFinite];
        }
        return;
    }
    shift(h, record, kViewWords, tick);
    if (h.copies < 2) {
        h.snapped = true;
        ++w.snaps[kSnapFirst];
        return;
    }

    int gap = h.ticks[1] - h.ticks[0];
    if (countGap && gap >= 1) ++w.gaps[gap >= 4 ? 3 : gap - 1];

    // DOES THE PUBLISHED VELOCITY MATCH THE MOVE THE PAIR MADE?
    //
    int reason = -1;
    if (gap < 1 || gap > kMaxBlendTicks) {
        reason = kSnapGap;
    } else if (cut) {
        reason = kSnapCamera;
    } else if (!pairSane(h, 0, kViewFields, sizeof(kViewFields) / sizeof(kViewFields[0]))) {
        reason = kSnapNonFinite;
    } else {
        float dx = floatAt(h.words[1], 0, 0) - floatAt(h.words[0], 0, 0);
        float dy = floatAt(h.words[1], 0, 4) - floatAt(h.words[0], 0, 4);
        float dz = floatAt(h.words[1], 0, 8) - floatAt(h.words[0], 0, 8);
        Quat  older = quatAt(h.words[0], 0, 120);
        Quat  newest = quatAt(h.words[1], 0, 120);
        // A cut between trackside cameras moves or turns the view further than any camera
        // moves between ticks.
        if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= kSnapMetres) ||
            (unitQuat(older) && unitQuat(newest) && quatTurn(older, newest) > kSnapTurnRadians))
            reason = kSnapCamera;
    }
    h.snapped = reason >= 0;
    if (h.snapped) ++w.snaps[reason];
}

// The pit board's copy, after the view's. `view` is the view pair of the same ticks: a camera cut
// switches the board's offset table (report 58), so the board is drawn at the newest copy whenever
// the view is.
void pushPitBoard(State& s, History& h, const uint8_t* record, int tick, const History& view) {
    Window&        w = s.window;
    const uint8_t* words = record + 4;
    size_t         fieldCount = sizeof(kPitBoardFields) / sizeof(kPitBoardFields[0]);
    if (h.copies > 0 && h.ticks[1] == tick) {
        std::memcpy(h.words[1], words, sizeof(uint32_t) * kPitBoardWords);
        if (h.copies == 2 && !h.snapped) {
            int reason = -1;
            if (view.snapped)
                reason = kSnapCamera;
            else if (!pairSane(h, 4, kPitBoardFields, fieldCount))
                reason = kSnapNonFinite;
            if (reason >= 0) {
                h.snapped = true;
                ++w.snaps[reason];
                ++w.pitBoardSnaps;
            }
        }
        return;
    }
    shift(h, words, kPitBoardWords, tick);
    if (h.copies < 2) {
        h.snapped = true;
        ++w.snaps[kSnapFirst];
        ++w.pitBoardSnaps;
        return;
    }

    int gap = h.ticks[1] - h.ticks[0];
    int reason = -1;
    if (gap < 1 || gap > kMaxBlendTicks) {
        reason = kSnapGap;
    } else if (h.words[0][0] != 4 || h.words[1][0] != 4) {
        reason = kSnapIdentity;
    } else if (view.snapped) {
        reason = kSnapCamera;
    } else if (!pairSane(h, 4, kPitBoardFields, fieldCount)) {
        reason = kSnapNonFinite;
    } else {
        float dx = floatAt(h.words[1], 4, 16) - floatAt(h.words[0], 4, 16);
        float dy = floatAt(h.words[1], 4, 20) - floatAt(h.words[0], 4, 20);
        float dz = floatAt(h.words[1], 4, 24) - floatAt(h.words[0], 4, 24);
        // Placed at the pit wall or set to rest at once.
        if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= kSnapMetres)) reason = kSnapJump;
    }
    h.snapped = reason >= 0;
    if (h.snapped) {
        ++w.snaps[reason];
        ++w.pitBoardSnaps;
    }
}

// DID THE BLEND WRITE WHAT IT PROMISED?
//
// Six hypotheses about how GPL reacts to these records have been measured and killed (reports
// 76-78 and the walk probe), and not one of them checked the only thing that is entirely ours:
// that a blended value lies BETWEEN the two copies it was blended from. Nothing else in this file
// checks it. `pairSane` only rejects non-finite values and magnitudes past 1e6, so a copy that is
// all zeros passes as sane -- and blending against a zeroed copy drags the eye a fraction of the
// way to the world origin, which is a camera that was never anywhere, for one frame, with the
// orientation untouched.
//
// Checked on the position, which is the field the artifact points at: the written value must lie
// within the two copies' range plus a centimetre of slack for the arithmetic.
unsigned g_blendOutOfRange = 0;
float    g_blendWorstMetres = 0.0f;

void checkBlendedPosition(const History& h, const uint8_t* record, int base, int offset,
                          const char* what) {
    // The pair the blend actually used, which is the only one the written value has to lie in.
    const uint32_t* lo = h.words[0];
    const uint32_t* hi = h.words[1];
    int             loTick = h.ticks[0];
    int             hiTick = h.ticks[1];
    float           worst = 0.0f;
    for (int k = 0; k < 3; ++k) {
        float older = floatAt(lo, base, offset + 4 * k);
        float newest = floatAt(hi, base, offset + 4 * k);
        float wrote = *reinterpret_cast<const float*>(record + offset + 4 * k);
        float low = older < newest ? older : newest;
        float high = older < newest ? newest : older;
        float over = wrote < low ? low - wrote : (wrote > high ? wrote - high : 0.0f);
        // Past the newest tick the value is SUPPOSED to leave the pair, by (alpha - 1) of the
        // pair's own span. Only what exceeds that is a fault.
        if (over > worst) worst = over;
    }
    if (worst <= 0.01f) return;

    ++g_blendOutOfRange;
    if (worst > g_blendWorstMetres) g_blendWorstMetres = worst;
    static int dumps = 8;
    if (dumps <= 0) return;
    --dumps;
    logf("interp: THE BLEND LEFT THE PAIR -- %s wrote %.2f %.2f %.2f, %.2f m outside the range of "
         "older %.2f %.2f %.2f and newest %.2f %.2f %.2f (ticks %d and %d)",
         what, static_cast<double>(*reinterpret_cast<const float*>(record + offset)),
         static_cast<double>(*reinterpret_cast<const float*>(record + offset + 4)),
         static_cast<double>(*reinterpret_cast<const float*>(record + offset + 8)),
         static_cast<double>(worst), static_cast<double>(floatAt(lo, base, offset)),
         static_cast<double>(floatAt(lo, base, offset + 4)),
         static_cast<double>(floatAt(lo, base, offset + 8)),
         static_cast<double>(floatAt(hi, base, offset)),
         static_cast<double>(floatAt(hi, base, offset + 4)),
         static_cast<double>(floatAt(hi, base, offset + 8)), loTick, hiTick);
}

// Blend one record to `target`. True when its pair was usable; only the first such record of a
// frame goes into the alpha statistics.
// Whether the records get blended at all -- the same question in both modes, which is why the
// option is not named after either of them. With it off the frames `frameRate` adds redraw an
// unchanged world, which is the control that says how much of the smoothness is the blend and
// how much is merely presenting more often.
// THE ONE PLACE that turns "the driver asked for a rate above the sim's" into "the world is
// blended". They were two options once, and the pair could be set to a state that
// draws duplicate frames -- a doubled picture in the headset -- so the second one is gone and
// this reads the first. Everything that wants to know whether the world moves between ticks
// asks here or asks interpolationActive().
bool blendingEnabled() { return g_options.frameRate > 0; }

// When a tick arrived, in QPC. RenderPacing's model is preferred where it exists; its own is
// fitted to the tick counter over a whole run, while this one is a two-point extrapolation from
// the last latch.
bool tickArrival(const State& s, int count, double& qpc) {
    if (renderPacingTickArrival(count, qpc)) return true;
    if (!s.haveTickModel || s.tickPeriod <= 0.0) return false;
    qpc = static_cast<double>(s.lastLatchQpc) + (count - s.lastLatchTick) * s.tickPeriod;
    return true;
}

// ONE DECISION FOR THE WHOLE FRAME, if the third copy is ever rebuilt.
//
// Taken per record, the older-pair fallback produced a worse artifact than the freeze it removed:
// each record has its own history, so one car could clamp to the newest pair while the world around
// it blended from the older one -- drawn at a different instant from everything else, eighteen times
// a second. Before the third copy a clamped frame clamped EVERY record together, and the eye reads a
// brief uniform hold far more forgivingly than one car swimming against the road. The rule is:
// either every record in the frame uses the older pair or none does.

// Hold the target at the previous frame's when the new one is behind it, and count that. A real
// discontinuity -- a tick gap, a camera cut, a mode change -- moves the target FORWARD, which is
// let through; only backwards is refused, because nothing in the world does that.
//
// The guard resets when the blend cannot be timed at all, so a session or mode change starts again
// rather than holding against an instant from before it.
double holdTargetForward(State& s, double target) {
    if (s.haveGuardTarget && target < s.guardTarget) {
        ++s.window.targetHeldBack;
        return s.guardTarget;
    }
    s.guardTarget = target;
    s.haveGuardTarget = true;
    return target;
}

// SUBSTITUTING A SECOND ANCHOR WHEN THE FIRST FALLS OUT OF RANGE WAS TRIED AND REVERTED.
// Do not rebuild it.
//
// The arithmetic behind it still holds and is worth keeping: `display - latency - tick` is the right
// expression, and with this frame's OWN latch-to-display it reduces to `the frame's instant - tick`,
// which lies between the two ticks by construction, so alpha could never leave [0, 1]. `latency` is
// not this frame's own and cannot be -- `display` is quantised to the display grid while the instant
// it is measured against is not, so the sample sweeps a whole display period and the average it
// settles on is off by up to half of one on any given frame. Raising kLatencyGain does not help,
// because the error is the sawtooth's own amplitude rather than tracking lag.
//
// So the target really does fall outside the pair, and by a real margin: measured 7 to 11 ms of a
// 27.78 ms window, on 5 to 24 frames of every 72, always low, with `70 of 70 alphas were the view's`
// confirming it is the view's own pair and not another record's.
//
// What failed was the remedy. Using the frame's own instant on just those frames mixes two anchors
// within one second -- at 72 Hz it reached 31 of 73 frames -- and the frames either side of a
// substitution disagree by whatever the two anchors differ by. That is the same fault the display
// clock was moved to fix, described where blendFrame builds the target: two formulas for one
// question, and the world steps where they meet. The driver's verdict was that it turned stepping
// objects into everything stuttering, which is what mixing anchors should do.
//
// Whatever replaces it has to answer with ONE anchor on every frame.

bool blendHistory(State& s, History& h, uint8_t* record, int base, const Field* fields,
                  size_t count, double target, float length, bool& sampled, bool rewriteAtOne) {
    if (h.copies < 2 || h.snapped) return false;
    double older = 0.0;
    double newest = 0.0;
    if (!tickArrival(s, h.ticks[0], older) || !tickArrival(s, h.ticks[1], newest) ||
        !(newest > older))
        return false;

    const uint32_t* lo = h.words[0];
    const uint32_t* hi = h.words[1];

    double alpha = (target - older) / (newest - older);
    bool   low = alpha < 0.0;
    bool   high = alpha > 1.0;
    if (low) alpha = 0.0;
    if (high) alpha = 1.0;
    if (!sampled) {
        sampled = true;
        s.lastAlpha = static_cast<float>(alpha);
        s.lastTickGap = h.ticks[1] - h.ticks[0];
        Window& w = s.window;
        if (!w.alphas || alpha < w.alphaMin) w.alphaMin = alpha;
        if (!w.alphas || alpha > w.alphaMax) w.alphaMax = alpha;
        w.alphaSum += alpha;
        ++w.alphas;
        if (low) ++w.clampedLow;
        if (high) ++w.clampedHigh;
        if ((low || high) && s.frequency > 0) {
            double outside = low ? older - target : target - newest;
            double ms = outside * 1000.0 / static_cast<double>(s.frequency);
            if (ms > w.clampedWorstMs) w.clampedWorstMs = ms;
        }

        // CORRECTING A CONSTANT BIAS WAS TRIED AND REVERTED. Do not rebuild it.
        //
        // The reasoning was that with a latency equal to this frame's own latch-to-display the
        // target reduces to `latch - tick`, so alpha is the latch's phase inside the tick and can
        // never clamp; therefore any clamp is error, and its direction says which way to move. A
        // slow integrator drove the target off whichever edge was being hit.
        //
        // It moved the mean exactly as intended and fixed nothing: at +2.17 ms of correction the
        // mean alpha reached 0.40, which is the ideal for 90/36, while `clamped low` stayed at 5
        // to 10 frames of every 70. It then integrated past the target, because a population that
        // never stops clamping never supplies the opposing push.
        //
        // The mean and the clamps are DIFFERENT POPULATIONS. The clamps are a tail -- measured 4.5
        // to 7.8 ms outside the pair while `alpha max` was already 0.97 to 0.99 -- so the
        // distribution was against the top of the window before any correction. Shifting it up
        // cannot pull a tail off the bottom.
        //
        // The arithmetic that would have refused this before it was built: the excursions in the
        // run it was designed from were 2.5 to 2.9 ms, and the offset inferred from the mean was
        // 1.9 ms. An offset smaller than the excursions cannot account for them.

        // The instant this frame was actually drawn for, against the one before it. Measured from
        // the clamped alpha, so it is what the record received rather than what was asked for --
        // which is how the clamp shows up here as a step of zero.
        double drawn = older + alpha * (newest - older);
        if (s.haveLastTarget && s.frequency > 0) {
            double step = (drawn - s.lastTarget) * 1000.0 / static_cast<double>(s.frequency);
            if (!w.steadySteps || step < w.steadyMin) w.steadyMin = step;
            if (!w.steadySteps || step > w.steadyMax) w.steadyMax = step;
            ++w.steadySteps;
        }
        s.lastTarget = drawn;
        s.haveLastTarget = true;
    }
    // The record already holds what the latch wrote, so at EXACTLY the newest tick there is nothing
    // to do -- unless an earlier frame of this same tick blended it somewhere else, which is what an
    // extra frame does. Past the newest tick the fields have to be written, because extrapolation
    // moves them; and on the older pair the top end is ticks[0] while the record holds ticks[1].
    if (alpha < 1.0 || rewriteAtOne)
        blendFields(lo, hi, record, base, fields, count, static_cast<float>(alpha), length);
    return true;
}

// RESEATING THE LOOKUP'S SEED WAS TRIED AND DISPROVED -- do not build it again.
//
// The seed gpl_sceneRender passes is p[23] = view record byte 80, a KEPT field, so it holds the
// latch's newest tick while the position handed to the search is blended. Reseating it from the
// blended position looks obviously right and is not: the instrument that did it reported
// `hint reseated 0, corrected 0`, and the decisive case had the SAME seed (12) with two positions
// 0.46 m apart answering 12 and 17. Where that search stops depends on the ORDER of its probes,
// through `while (v8 & v9)`, not on where it starts.
//
// What does work is checking its ANSWER: Stereo.cpp's walkLookupThunk replaces the dlong with the
// car's own (record byte 12) whenever the lookup's has moved further than the car did. See
// report 84.

// Rewrite every record of the last latch to `target`, a QPC instant between the two ticks it
// holds. Always from the two kept copies, never from what a previous call left in the record, so
// calling it again with a later target is exactly as correct as calling it once.
//
// `cars` and `carCount` name the cars to rewrite: the list the latch filled while it ran, or the
// copy kept past it when an extra frame re-blends.
void applyBlend(State& s, double target, const int* cars, int carCount, bool haveMirror,
                bool havePitBoard, bool rewriteAtOne) {
    Window& w = s.window;
    float   length = trackLengthMetres();
    bool    sampled = false;
    bool    any = false;
    int     parts = kPartAll;

    // ONE DECISION FOR THE WHOLE FRAME. Every record that will be blended has to be able to honour
    // it, or the frame keeps the newest pair and the clamp for all of them.
    if ((parts & kPartView) &&
        blendHistory(s, s.view, reinterpret_cast<uint8_t*>(g_at.viewRecord), 0, kViewFields,
                     sizeof(kViewFields) / sizeof(kViewFields[0]), target, length, sampled,
                     rewriteAtOne)) {
        ++w.viewBlended;
        any = true;
        // The view record's eye position is the first three floats; the car's is at its byte 16.
        checkBlendedPosition(s.view, reinterpret_cast<const uint8_t*>(g_at.viewRecord), 0, 0, "the eye");
    }
    // Whether the alpha reported for this frame is the VIEW's. Measured rather than assumed: a
    // clamp attributed to the wrong record sends the next reader after the wrong history.
    if (sampled) ++w.sampledView;
    if ((parts & kPartMirror) && haveMirror &&
        blendHistory(s, s.mirror, reinterpret_cast<uint8_t*>(g_at.mirrorRecord), 0, kViewFields,
                     sizeof(kViewFields) / sizeof(kViewFields[0]), target, length, sampled,
                     rewriteAtOne)) {
        ++w.mirrorBlended;
        any = true;
    }
    if (parts & kPartCars) {
        for (int i = 0; i < carCount; ++i) {
            int slot = cars[i];
            if (blendHistory(s, s.cars[slot], carRecord(slot), 4, kCarFields,
                             sizeof(kCarFields) / sizeof(kCarFields[0]), target, length, sampled,
                             rewriteAtOne)) {
                ++w.carsBlended;
                any = true;
                if (slot == s.viewedSlot)
                    checkBlendedPosition(s.cars[slot], carRecord(slot), 4, 16, "the viewed car");
            }
        }
    }
    if ((parts & kPartPitBoard) && havePitBoard &&
        blendHistory(s, s.pitBoard, reinterpret_cast<uint8_t*>(g_at.pitBoardRecord), 4,
                     kPitBoardFields, sizeof(kPitBoardFields) / sizeof(kPitBoardFields[0]), target,
                     length, sampled, rewriteAtOne)) {
        ++w.pitBoardBlended;
        any = true;
    }
    if (any) ++w.blended;
    s.blendable = any;
}

void blendFrame(State& s, long long now, bool frozen) {
    Window& w = s.window;
    // No new tick since the last latch (the sim is stopped): the records hold the newest state.
    if (frozen) {
        ++w.frozen;
        return;
    }
    // Flat mode has neither of the two clocks the VR path is built on: no presenter publishing a
    // display time, and no task-rate probe (it is bound in VR only), so taskRateSimHz answers 0.
    // The target is the one interpolationExtraFrame uses -- one measured tick behind the wall
    // clock -- so that the latched frame and the frames drawn between the ticks sweep the same
    // line instead of meeting in a sawtooth.
    if (!g_options.vr) {
        if (!s.haveTickModel || s.tickPeriod <= 0.0) {
            ++w.untimed;
            s.haveGuardTarget = false;
            return;
        }
        ++w.costTimed;
        applyBlend(s, holdTargetForward(s, static_cast<double>(now) - s.tickPeriod), s.latched,
                   s.latchedCount, s.mirrorLatched, s.pitBoardLatched, false);
        return;
    }

    // ANCHORED ON THIS FRAME, exactly as the frames between the ticks are.
    //
    // renderPacingDisplayTime anchors on the sim latch instead, and that was the only difference
    // left between the two paths: 36 frames a second asked one way and 54 the other. Where the two
    // disagree the target stands still for a frame and then advances two display periods -- measured
    // as steps of 0.00 ms followed by 22.5 ms, clustered where the render is slowest and the two
    // anchors are furthest apart. One formula for every frame cannot produce that.
    //
    // The comment in interpolationBetweenTicks records why the frame anchor was introduced for the
    // extra frames; the latched frame simply never followed.
    int       simHz = taskRateSimHz();
    long long display = 0;
    bool      fromPresenter = false;
    if (simHz <= 0 || !renderPacingDisplayTimeAt(now, now, display, fromPresenter)) {
        ++w.untimed;
        s.haveGuardTarget = false;
        return;
    }
    if (fromPresenter)
        ++w.presenterTimed;
    else
        ++w.costTimed;

    // The drawn state is the one a tick before the display time, less the frame's own smoothed
    // latch-to-display time, so it advances with the display clock and always lies between the two
    // latched ticks.
    //
    // FORWARD TARGETING WAS TRIED AND REVERTED. Dropping the tick put the target in
    // [T1, T1 + one tick), where the millisecond of noise in the estimate cannot fall off the edge
    // of the data, and it removed the clamping that freezes about one frame in five -- all of which
    // it did, measurably: clamped low went to 0 and alpha's mean to 0.51. It also needed
    // extrapolation, which brought the published linear and angular velocities in with it.
    //
    // None of it fixed the jolt the driver was chasing, and the driver's judgement was that the
    // frozen-frame stepping it replaced was the lesser problem. Six mechanisms were proposed for
    // that jolt across this session and every one was measured and disproved; it is not in this
    // expression, and the measured signal never matched the symptom's distribution.
    //
    // THE SMOOTHING HAS TO BE SLOW, and this is the whole reason the drawn instant was jittery.
    //
    // `display` is quantised to the presenter's display grid while `now` advances continuously, so
    // `sample` does not vary a little -- it SAWTOOTHS across a whole display period, 11.11 ms at
    // 90 Hz. An exponential average of a sawtooth settles on its mean, but it also ripples by
    // roughly the coefficient times the amplitude every step, and that ripple lands directly in
    // the target: at 0.05 it is 0.05 * 11.11 = 0.55 ms a frame, which is what the drawn instant
    // was measured stepping by (10.46 to 11.46 ms against an expected 11.11).
    //
    // The target must advance as evenly as `display` does, so what is subtracted from it has to be
    // effectively constant frame to frame. kLatencyGain is the ripple budget: 0.004 puts it at
    // 0.04 ms, well under a tenth of a millisecond, with a time constant of about 250 frames --
    // under three seconds, which is fast enough for anything that is not a mode change. Mode
    // changes are not left to it: the reset below takes them in one step.
    constexpr double kLatencyGain = 0.004;
    double tick = static_cast<double>(s.frequency) / simHz;
    double sample = static_cast<double>(display - now);
    if (!s.haveLatency || std::fabs(sample - s.latency) > 3.0 * tick) {
        s.latency = sample;
        s.haveLatency = true;
    } else {
        s.latency += kLatencyGain * (sample - s.latency);
    }
    double target = holdTargetForward(s, static_cast<double>(display) - s.latency - tick);

    applyBlend(s, target, s.latched, s.latchedCount, s.mirrorLatched, s.pitBoardLatched, false);
}

// Right after the latch. Every record the latch wrote this frame is copied into its history,
// then rewritten from the history -- never from what a previous frame blended, which the latch
// has just overwritten. A frame whose latch published no world record (menus, loads) draws no
// world, so nothing is written.
void __stdcall afterLatch() {
    State&        s = g_state;
    LARGE_INTEGER entry;
    QueryPerformanceCounter(&entry);
    long long now = entry.QuadPart;
    renderPacingObserveTick(now, s.getBGCount());

    if (s.viewLatched) {
        Window& w = s.window;
        ++w.frames;
        int  tick = *reinterpret_cast<const int*>(g_at.latchedTickCount);
        bool frozen = s.haveLastTick && tick == s.lastTick;

        // Date this tick, so an extra frame can place a target between it and the one before.
        // Only a latch that actually advanced the counter carries timing; a repeat says nothing
        // about the period, and a gap of more than one tick is divided by the gap.
        if (!frozen) {
            // Measured from the previous latch, which is the only source that works in both
            // modes: taskRateSimHz is the scheduler's own figure and the probe behind it is
            // bound in VR only, so in flat mode it answers 0 for the whole session. It is used
            // where it is available, to start from a sensible figure before two latches exist.
            bool   measurable = s.lastLatchQpc != 0 && tick > s.lastLatchTick;
            double span = measurable ? static_cast<double>(now - s.lastLatchQpc) /
                                           (tick - s.lastLatchTick)
                                     : 0.0;
            // A span this far outside the plausible range is a loading pause or a session
            // change, not a tick: 1 ms to 1 s covers 1000 Hz down to 1 Hz.
            double floor = static_cast<double>(s.frequency) / 1000.0;
            double ceiling = static_cast<double>(s.frequency);
            if (span >= floor && span <= ceiling) {
                s.tickPeriod = s.tickPeriod > 0.0 ? s.tickPeriod * 0.9 + span * 0.1 : span;
            } else if (s.tickPeriod <= 0.0) {
                int simHz = taskRateSimHz();
                s.tickPeriod = simHz > 0 ? static_cast<double>(s.frequency) / simHz : 0.0;
            }
            s.lastLatchQpc = now;
            s.lastLatchTick = tick;
            s.haveTickModel = s.tickPeriod > 0.0;
        }

        s.lastTick = tick;
        s.haveLastTick = true;

        int identity[3] = {*reinterpret_cast<const int*>(g_at.viewCameraMode),
                           *reinterpret_cast<const int*>(g_at.viewCarDriverIndex),
                           *reinterpret_cast<const uint8_t*>(g_at.viewIsLive)};
        pushView(s, s.view, reinterpret_cast<const uint8_t*>(g_at.viewRecord), tick, identity, true);
        if (s.mirrorLatched)
            pushView(s, s.mirror, reinterpret_cast<const uint8_t*>(g_at.mirrorRecord), tick, identity,
                     false);
        for (int i = 0; i < s.latchedCount; ++i) {
            int slot = s.latched[i];
            pushCar(s, s.cars[slot], carRecord(slot), tick);
        }
        // THE CAMERA AND THE CAR IT IS ATTACHED TO MUST BE AT THE SAME TICK.
        //
        // The view record and each car record have their own history, their own snap rules and
        // their own decision, and the rule sets are disjoint -- kSnapIdentity and kSnapJump exist
        // only for cars, kSnapCamera only for the view. So one of them could snap to the newest
        // tick while the other blended to the display time, and the cockpit, the steering wheel
        // and the front wheels were then drawn up to two ticks away from the eye they are seen
        // from: about a metre at racing speed, for one frame (report 78). It also put the eye's
        // dlong and the car's dlong in different track segments, which is what made parts of the
        // car disappear behind the road of the segment the eye was in (report 77 section 3.3).
        //
        // The pit board is already coupled to the view this way; this does the same for the car
        // the view is attached to, in both directions.
        //
        // The mirror record joins the same decision (below and after the car's coupling), so a
        // mirror that snapped on its own takes the view with it, and the view then takes its car.
        bool mirrorPair = s.mirrorLatched && s.mirror.copies == 2;
        if (mirrorPair && s.view.copies == 2 && s.mirror.snapped && !s.view.snapped) {
            s.view.snapped = true;
            ++w.snaps[kSnapCamera];
            ++w.viewMirrorCoupled;
        }
        int viewedSlot = viewedCarSlot(s, identity[1], identity[2] != 0);
        s.viewedSlot = viewedSlot;
        if (viewedSlot >= 0) {
            History& car = s.cars[viewedSlot];
            if (car.copies == 2 && s.view.copies == 2 && car.snapped != s.view.snapped) {
                History& blended = car.snapped ? s.view : car;
                blended.snapped = true;
                blended.stepPerTick = -1.0f;
                ++w.snaps[kSnapCamera];
                ++w.viewCarCoupled;
            }
            // The older pair needs no coupling here: applyBlend decides it once for the frame and
            // every record honours that decision or none does.
        }

        // THE MIRROR AND THE VIEW MUST BE AT THE SAME TICK TOO (report 101).
        //
        // GPL computes both from the same car in the same call -- gpl_viewUpdate 0x454350 runs
        // gpl_viewCompute and then gpl_mirrorViewCompute 0x454E30 -- and Stereo.cpp aims every
        // glass by putting the eye, taken from the view record, into the car frame it rebuilds
        // from the mirror record. A view snapped to the newest tick against a mirror blended
        // towards the older one puts the eye a tick's travel ahead of or behind the seat in that
        // frame, and the glasses are aimed from there: a replay step while paused moves every car
        // past 3x its last step, the viewed car snaps, the view follows it above, and the mirror
        // was drawn from the wrong place for the rest of that tick -- the flick on each step.
        if (mirrorPair && s.view.snapped && !s.mirror.snapped) {
            s.mirror.snapped = true;
            ++w.snaps[kSnapCamera];
            ++w.viewMirrorCoupled;
        }

        // A board hidden in this latch starts a new history when it shows again.
        if (s.pitBoardLatched)
            pushPitBoard(s, s.pitBoard, reinterpret_cast<const uint8_t*>(g_at.pitBoardRecord), tick,
                         s.view);
        else
            s.pitBoard.copies = 0;

        // Keep what an extra frame needs to repeat this blend at a later target.
        s.blendCarCount = s.latchedCount;
        for (int i = 0; i < s.latchedCount; ++i) s.blendCars[i] = s.latched[i];
        s.blendMirror = s.mirrorLatched;
        s.blendPitBoard = s.pitBoardLatched;
        s.blendable = false;

        if (blendingEnabled()) blendFrame(s, now, frozen);

        LARGE_INTEGER done;
        QueryPerformanceCounter(&done);
        w.costTicks += done.QuadPart - now;
    }
    s.latchedCount = 0;
    s.viewLatched = false;
    s.mirrorLatched = false;
    s.pitBoardLatched = false;
}

// -- hooks ------------------------------------------------------------------------------------

using CarLatchFn = void*(__cdecl*)(void* car);
// gpl_viewLatchRecord and gpl_viewGetMirrorRecord are __thiscall with one stack argument;
// __fastcall with an unused edx has the same register and stack shape.
using RecordLatchFn = int(__fastcall*)(void* self, void* unused, void* destination);

CarLatchFn    g_carLatch = nullptr;
RecordLatchFn g_viewLatch = nullptr;
RecordLatchFn g_mirrorLatch = nullptr;

using PitBoardLatchFn = void*(__cdecl*)();
PitBoardLatchFn g_pitBoardLatch = nullptr;

void* __cdecl carLatchHook(void* car) {
    void*  result = g_carLatch(car);
    State& s = g_state;
    int    slot = carSlot(reinterpret_cast<uintptr_t>(car));
    if (slot >= 0 && s.latchedCount < kCarSlots) s.latched[s.latchedCount++] = slot;
    return result;
}

int __fastcall viewLatchHook(void* self, void* unused, void* destination) {
    int copied = g_viewLatch(self, unused, destination);
    if (copied && reinterpret_cast<uintptr_t>(destination) == g_at.viewRecord)
        g_state.viewLatched = true;
    return copied;
}

int __fastcall mirrorLatchHook(void* self, void* unused, void* destination) {
    int copied = g_mirrorLatch(self, unused, destination);
    if (copied && reinterpret_cast<uintptr_t>(destination) == g_at.mirrorRecord)
        g_state.mirrorLatched = true;
    return copied;
}

// 0 while the board is hidden; the record otherwise.
void* __cdecl pitBoardLatchHook() {
    void* record = g_pitBoardLatch();
    if (reinterpret_cast<uintptr_t>(record) == g_at.pitBoardRecord) g_state.pitBoardLatched = true;
    return record;
}

using AfterFn = void(__stdcall*)();
AfterFn g_afterLatchFn = &afterLatch;
void*   g_enableNext = nullptr;

// fgEnableBG first, then the blend with every register and flag as fgEnableBG left them.
__declspec(naked) void afterLatchThunk() {
    __asm { call dword ptr [g_enableNext]
            pushad
            pushfd
            call dword ptr [g_afterLatchFn]
            popfd
            popad
            ret }
}

uintptr_t callTarget(uintptr_t site) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const uint8_t*>(site) + 1, sizeof(rel));
    return site + 5 + rel;
}

// Where a `jmp rel32` at `address` goes, or 0 when there is none.
uintptr_t jumpTarget(uintptr_t address) {
    if (!address || *reinterpret_cast<const uint8_t*>(address) != 0xE9) return 0;
    return callTarget(address);
}

bool redirect(uintptr_t site, uintptr_t to) {
    int32_t rel = static_cast<int32_t>(to - (site + 5));
    return patchBytes(reinterpret_cast<void*>(site + 1), &rel, sizeof(rel));
}

void appendField(char* text, size_t size, const Field& field, const char* note) {
    size_t used = std::strlen(text);
    if (used + 1 >= size) return;
    std::snprintf(text + used, size - used, "%s%s +%d%s", used ? ", " : "", field.name,
                  field.offset, note);
}

void logFieldTable(const char* what, const Field* fields, size_t count, const char* snaps) {
    char blended[640] = {};
    char kept[640] = {};
    for (size_t i = 0; i < count; ++i) {
        const Field& field = fields[i];
        switch (field.blend) {
            case kKeep:   appendField(kept, sizeof(kept), field, ""); break;
            case kLinear: appendField(blended, sizeof(blended), field, ""); break;
            case kAngle:  appendField(blended, sizeof(blended), field, " (angle)"); break;
            case kSpin:
                appendField(blended, sizeof(blended), field, " (angle, under pi/2 between ticks)");
                break;
            case kDlong:
                appendField(blended, sizeof(blended), field, " (unwrapped at the lap)");
                break;
            case kMatrix: appendField(blended, sizeof(blended), field, " (slerp)"); break;
            case kQuat:
                appendField(blended, sizeof(blended), field, " (slerp, matrix rebuilt)");
                break;
        }
    }
    logf("interp: %s -- blended: %s | kept at the newest tick: %s | snapped on: %s", what, blended,
         kept, snaps);
}

}  // namespace

void interpolationInstall() {
    State& s = g_state;
    if (!g_options.vr && g_options.frameRate <= 0) return;

    // RenderPacing finds the tick wait by searching, and it is a VR module. In flat mode take
    // the address directly; every byte of the latch around it is checked below either way, so a
    // wrong address is refused rather than patched.
    uintptr_t wait = renderPacingTickWaitSite();
    if (!wait && !g_options.vr) wait = kFlatTickWaitSite;
    HMODULE   exe = GetModuleHandleA(nullptr);
    uintptr_t enable =
        exe ? reinterpret_cast<uintptr_t>(GetProcAddress(exe, "?fgEnableBG@@YAXXZ")) : 0;
    s.getBGCount = exe ? reinterpret_cast<CountFn>(GetProcAddress(exe, "?getBGCount@@YAHXZ"))
                       : nullptr;
    if (!wait) {
        logf("interp: render pacing is not installed at the tick wait, so the latch is not "
             "located -- no interpolation");
        return;
    }
    if (!enable || !s.getBGCount) {
        logf("interp: the exe does not export fgEnableBG/getBGCount -- no interpolation");
        return;
    }

    // Every global the blend reads or writes, placed on this image before the latch check below
    // can compare against one of them.
    const char* missing = nullptr;
    if (!resolveAddresses(missing)) {
        logf("interp: %s cannot be placed in this image (deltas %s) -- no interpolation", missing,
             rebaseSummary());
        return;
    }

    // The latch in gpl_fgWaitForTickAndSnapshot, from its fgDisableBG call at `wait`:
    //   +0x00 call fgDisableBG     +0x05 call getBGCount     +0x0A mov edi, [esp+14h]
    //   +0x0E mov [gpl_fgLastLatchedBgCount], eax   ...the latch callbacks...
    //   +0x44 call fgEnableBG
    auto                    code = reinterpret_cast<const uint8_t*>(wait);
    constexpr unsigned char kLoadCount[4] = {0x8B, 0x7C, 0x24, 0x14};
    uint32_t                store = 0;
    std::memcpy(&store, code + 0x0F, sizeof(store));
    uintptr_t enableSite = wait + 0x44;
    uintptr_t enableTarget = code[0x44] == 0xE8 ? callTarget(enableSite) : 0;
    bool      enableMatches = enableTarget &&
                         (enableTarget == enable || jumpTarget(enable) == enableTarget ||
                          jumpTarget(enableTarget) == enable);
    if (std::memcmp(code + 0x0A, kLoadCount, sizeof(kLoadCount)) != 0 || code[0x0E] != 0xA3 ||
        store != g_at.latchedTickCount || !enableMatches) {
        logf("interp: the tick wait at %08X is not the expected latch (count store %02X %08X "
             "against %08X for this image, call at +0x44 %02X to %08X, fgEnableBG %08X) -- no "
             "interpolation",
             static_cast<unsigned>(wait), code[0x0E], static_cast<unsigned>(store),
             static_cast<unsigned>(g_at.latchedTickCount), code[0x44],
             static_cast<unsigned>(enableTarget), static_cast<unsigned>(enable));
        return;
    }

    // The three car latch sites may already hold the sim ring's hook (SimRing.h), which
    // installs first and calls the stock latch itself; the blend then runs outside it.
    uintptr_t ring = simRingCarLatchHookAddress();

    const StockCall* calls[6] = {&kCarLatchCalls[0], &kCarLatchCalls[1], &kCarLatchCalls[2],
                                 &kViewLatchCall,    &kMirrorLatchCall,  &kPitBoardLatchCall};
    for (const StockCall* call : calls) {
        auto bytes = reinterpret_cast<const uint8_t*>(call->site);
        bool isCarLatch = call == calls[0] || call == calls[1] || call == calls[2];
        if (isCarLatch && ring && bytes[0] == 0xE8 && callTarget(call->site) == ring) continue;
        if (std::memcmp(bytes, call->bytes, sizeof(call->bytes)) != 0) {
            logf("interp: %08X does not hold its stock call (%02X %02X %02X %02X %02X) -- no "
                 "interpolation",
                 static_cast<unsigned>(call->site), bytes[0], bytes[1], bytes[2], bytes[3],
                 bytes[4]);
            return;
        }
    }

    g_carLatch = reinterpret_cast<CarLatchFn>(callTarget(kCarLatchCalls[0].site));
    g_viewLatch = reinterpret_cast<RecordLatchFn>(callTarget(kViewLatchCall.site));
    g_mirrorLatch = reinterpret_cast<RecordLatchFn>(callTarget(kMirrorLatchCall.site));
    g_pitBoardLatch = reinterpret_cast<PitBoardLatchFn>(callTarget(kPitBoardLatchCall.site));
    g_enableNext = reinterpret_cast<void*>(enableTarget);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;

    for (const StockCall& call : kCarLatchCalls) {
        if (!redirect(call.site, reinterpret_cast<uintptr_t>(&carLatchHook))) {
            logf("interp: could not redirect the car latch call at %08X -- no interpolation",
                 static_cast<unsigned>(call.site));
            return;
        }
    }
    if (!redirect(kViewLatchCall.site, reinterpret_cast<uintptr_t>(&viewLatchHook)) ||
        !redirect(kMirrorLatchCall.site, reinterpret_cast<uintptr_t>(&mirrorLatchHook)) ||
        !redirect(kPitBoardLatchCall.site, reinterpret_cast<uintptr_t>(&pitBoardLatchHook)) ||
        !redirect(enableSite, reinterpret_cast<uintptr_t>(&afterLatchThunk))) {
        logf("interp: could not redirect the view latch, mirror latch, pit board latch or "
             "fgEnableBG call -- no interpolation");
        return;
    }
    s.installed = true;

    logf("interp: installed -- gpl_carLatchRenderRecord calls at %08X/%08X/%08X, the view, "
         "mirror and pit board record latches at %08X/%08X/%08X and the latch's fgEnableBG call at "
         "%08X go through gplvr; the blend runs after fgEnableBG, before the event pump and the "
         "draw. %s",
         static_cast<unsigned>(kCarLatchCalls[0].site), static_cast<unsigned>(kCarLatchCalls[1].site),
         static_cast<unsigned>(kCarLatchCalls[2].site), static_cast<unsigned>(kViewLatchCall.site),
         static_cast<unsigned>(kMirrorLatchCall.site),
         static_cast<unsigned>(kPitBoardLatchCall.site), static_cast<unsigned>(enableSite),
         blendingEnabled()
             ? "interpolate = 1."
             : "interpolate = 0: records are kept but nothing is blended.");
    logFieldTable("car record R = car+0x4FC (live 006BF080, replay 00790848, 1544 bytes)",
                  kCarFields, sizeof(kCarFields) / sizeof(kCarFields[0]),
                  "driver +32, dial range +44, model +52, flags +92 or wheel state +252..264 "
                  "changing; ticks more than 4 apart; a blended value NaN, infinite or over 1e6; "
                  "a move over 5 m or 3x the last step");
    logFieldTable("view record 00776175 and mirror record 00776219", kViewFields,
                  sizeof(kViewFields) / sizeof(kViewFields[0]),
                  "camera mode, view car or live/replay changing; ticks more than 4 apart; a "
                  "blended value NaN, infinite or over 1e6; the eye moving over 5 m or turning "
                  "over 0.5 rad");
    logFieldTable("pit board record 0074E990", kPitBoardFields,
                  sizeof(kPitBoardFields) / sizeof(kPitBoardFields[0]),
                  "hidden in the latch before; type +4 not 4; ticks more than 4 apart; the view "
                  "pair snapped; a blended value NaN, infinite or over 1e6; a move over 5 m");
}

bool interpolationActive() { return g_state.installed && blendingEnabled(); }

bool interpolationExtraFrame() {
    State& s = g_state;
    if (!s.installed || !blendingEnabled()) return false;

    if (!s.haveTickModel || s.tickPeriod <= 0.0) return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // THE INSTANT THIS FRAME WILL BE SEEN, less a tick -- not the instant it is drawn.
    //
    // Three versions of this were wrong before this one, and the third looked most right:
    //
    //   display - latency - tick   the latched frame's expression, asked again through
    //                              renderPacingDisplayTime -- which anchors on the frame's
    //                              LATCH. The latch does not move between two sim ticks, so
    //                              every frame between them blended to the same instant: 275
    //                              blended frames against 36 display times in the log.
    //   now + latency - tick       `latency` is already display - now, so adding it double
    //                              counts. Measured: alpha min 0.65 and 202 of 232 frames
    //                              CLAMPED at 1.00 -- two thirds of every tick drawn at the
    //                              newest state.
    //   now - tickPeriod           sweeps the tick correctly and still judders, because `now`
    //                              is when the frame is DRAWN. The interval from drawing to
    //                              being displayed is not constant -- the presenter shows
    //                              whichever frame is newest at its latch -- so the world's
    //                              sampled instant jitters by that much. At 55 m/s a 3 ms
    //                              jitter is 17 cm, which is roadside objects stepping as you
    //                              pass them.
    //
    // Anchoring the display query on THIS frame instead quantises the answer to the presenter's
    // own latches: frames headed for the same display get the same world, frames headed for
    // different displays get consecutive ones, and the draw-time jitter cancels because it is
    // no longer in the expression.
    //
    // Flat mode has no such clock -- renderPacingInstall is VR-only -- so there the wall clock
    // is all there is, and SwapBuffers' own blocking is what keeps the draw-to-display interval
    // steady enough for it.
    double target = 0.0;
    if (g_options.vr) {
        long long display = 0;
        bool      fromPresenter = false;
        if (!s.haveLatency ||
            !renderPacingDisplayTimeAt(now.QuadPart, now.QuadPart, display, fromPresenter))
            return false;
        target = static_cast<double>(display) - s.latency - s.tickPeriod;
    } else {
        // Flat mode has no display clock, so the wall clock is all there is; SwapBuffers' own
        // blocking is what keeps the draw-to-display interval steady enough for it.
        target = static_cast<double>(now.QuadPart) - s.tickPeriod;
    }
    target = holdTargetForward(s, target);

    Window& w = s.window;
    ++w.extraFrames;
    applyBlend(s, target, s.blendCars, s.blendCarCount, s.blendMirror, s.blendPitBoard, true);
    if (s.blendable) ++w.extraBlended;

    LARGE_INTEGER done;
    QueryPerformanceCounter(&done);
    // Its own total: `costTicks` is divided by the latched frames, which an extra frame is not.
    w.extraCostTicks += done.QuadPart - now.QuadPart;
    return s.blendable;
}

// The viewed car's own distance along the track, blended, in metres. Record byte 12, the field
// the sim maintains and the one the mark ring shows moving smoothly to a tenth of a metre while
// gpl_cameraTrackPositionFromWorld's answer for the same point was spiking 32 m (report 84).
// The VIEW's own blended dlong, byte 60 of the view record.
//
// This is the camera's position along the track, which is what decides the camera's segment. The
// car's dlong is a different quantity: a chase or trackside camera sits metres from the car, and
// substituting one for the other is what put the drawn range around the wrong point (measured at
// 4.98 m in the chase camera).
//
// `snapped` says the view history refused to blend this frame -- a camera cut, a reset, a tick gap
// -- which is a legitimate discontinuity in this value rather than a defect.
bool interpolationViewDlong(float& metres, bool& snapped) {
    State& s = g_state;
    if (!s.installed || s.view.copies < 1) return false;
    metres = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(g_at.viewRecord) + 60);
    snapped = s.view.snapped;
    return true;
}

// THE SAME QUANTITY THE BLEND STARTED FROM. interpolationViewDlong reads the live record, which
// the blend has already written, so it is the blended value by definition -- and there was no
// way to ask what it would have been without the blend. The newest latched copy is that value:
// the view history's words[1] is the record as the sim last left it, base 0 (pushView copies
// from offset 0), and 60 is the same field.
//
// This pair is the only honest test of "the artifact needs blending on". Comparing the camera
// against the VIEWED CAR's dlong is not: that one is blended too, and a chase or trackside
// camera sits metres from the car, so the two differ on every frame of a parked session. A
// walk-trace column built on it read 182 of 182 passes "moved by blending" at a standstill.
bool interpolationViewDlongAtTick(float& metres) {
    State& s = g_state;
    if (!s.installed || s.view.copies < 1) return false;
    metres = floatAt(s.view.words[1], 0, 60);
    return true;
}

bool interpolationViewedCarDlong(float& metres) {
    State& s = g_state;
    if (!s.installed || s.viewedSlot < 0) return false;
    if (s.cars[s.viewedSlot].copies < 1) return false;
    metres = *reinterpret_cast<const float*>(carRecord(s.viewedSlot) + 12);
    return true;
}

bool interpolationViewedCarPosition(float blended[3], float newestTick[3]) {
    State& s = g_state;
    if (!s.installed || s.viewedSlot < 0) return false;
    const History& h = s.cars[s.viewedSlot];
    if (h.copies < 1) return false;

    // Record bytes 16..27 are the car's position, p-record words 323..325.
    const uint8_t* record = carRecord(s.viewedSlot);
    for (int k = 0; k < 3; ++k) {
        blended[k] = *reinterpret_cast<const float*>(record + 16 + 4 * k);
        newestTick[k] = floatAt(h.words[1], 4, 16 + 4 * k);
    }
    return true;
}

bool interpolationFormat(char* out, size_t size) {
    State& s = g_state;
    if (!s.installed) return false;
    Window& w = s.window;
    bool    report = w.frames > 0;
    if (report) {
        int    simHz = taskRateSimHz();
        double toMs = 1000.0 / static_cast<double>(s.frequency);
        // snprintf returns what the line WOULD have taken, so a line that outgrows its buffer can
        // be caught instead of silently losing whatever was added last -- which is always the
        // newest instrument.
        int wanted = std::snprintf(
            out, size,
            "interp: %s | %u world frames: %u blended, %u frozen, %u untimed | alpha mean %.2f "
            "min %.2f max %.2f, clamped low %u high %u (worst %.3f ms outside, %u of %u alphas "
            "were the view's), target held back %u "
            "| the drawn instant stepped %.2f to %.2f ms over %u frames "
            "| snaps first %u identity %u tick gap %u "
            "jump %u camera %u nonfinite %u (%u of them the view and its car coupled, %u the view "
            "and its mirror) | view tick "
            "gaps 1:%u 2:%u 3:%u 4+:%u | cars blended %.1f a frame "
            "| view blended %u, mirror %u, pit board %u (snapped %u) | rotation: %u matrix "
            "fallbacks | blend left the pair %u times, worst %.2f m | display time from the "
            "presenter %u, from the render cost %u | target = display - %.2f ms - one tick %.2f "
            "ms | cost %.3f ms a frame | between ticks %u frames, %u re-blended, %.3f ms each",
            blendingEnabled() ? "on" : "off", w.frames, w.blended, w.frozen, w.untimed,
            w.alphas ? w.alphaSum / w.alphas : 0.0, w.alphas ? w.alphaMin : 0.0,
            w.alphas ? w.alphaMax : 0.0, w.clampedLow, w.clampedHigh, w.clampedWorstMs,
            w.sampledView, w.alphas, w.targetHeldBack, w.steadyMin, w.steadyMax,
            w.steadySteps, w.snaps[kSnapFirst],
            w.snaps[kSnapIdentity], w.snaps[kSnapGap], w.snaps[kSnapJump], w.snaps[kSnapCamera],
            w.snaps[kSnapNonFinite], w.viewCarCoupled, w.viewMirrorCoupled, w.gaps[0],
            w.gaps[1], w.gaps[2],
            w.gaps[3],
            static_cast<double>(w.carsBlended) / w.frames, w.viewBlended, w.mirrorBlended,
            w.pitBoardBlended, w.pitBoardSnaps, g_rotationFallbacks, g_blendOutOfRange,
            static_cast<double>(g_blendWorstMetres), w.presenterTimed, w.costTimed,
            s.haveLatency ? s.latency * toMs : 0.0,
            simHz > 0 ? 1000.0 / simHz : s.tickPeriod * toMs,
            static_cast<double>(w.costTicks) * toMs / w.frames,
            w.extraFrames, w.extraBlended,
            w.extraFrames ? static_cast<double>(w.extraCostTicks) * toMs / w.extraFrames : 0.0);
        if (wanted < 0 || static_cast<size_t>(wanted) >= size) {
            static bool said = false;
            if (!said) {
                said = true;
                logf("interp: THE LINE ABOVE IS TRUNCATED -- it wants %d bytes and has %zu, so "
                     "its last fields are missing. Grow the buffer in Hooks.cpp.",
                     wanted, size);
            }
        }
    }
    w = Window();
    return report;
}

}  // namespace gplvr
