#include "Interpolation.h"

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
    unsigned  extraFrames = 0;   // frames drawn between two ticks (flatExtraFrames)
    unsigned  extraBlended = 0;  // of those, the ones that found a usable pair
    long long extraCostTicks = 0;
    unsigned  clampedLow = 0;
    unsigned  clampedHigh = 0;
    unsigned  snaps[kSnaps] = {};
    unsigned  gaps[4] = {};  // view pair: 1, 2, 3, 4+ ticks
    unsigned  carsBlended = 0;
    unsigned  viewBlended = 0;
    unsigned  mirrorBlended = 0;
    unsigned  pitBoardBlended = 0;
    unsigned  pitBoardSnaps = 0;  // also in snaps[]
    unsigned  viewCarCoupled = 0;  // frames where the view and the viewed car were snapped together
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
    int          count = *reinterpret_cast<const int*>(kSegmentCount);
    const float* table = *reinterpret_cast<const float* const*>(kSegmentTable);
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
void blendRotation(const History& h, uint8_t* record, int base, const Field& field, float a) {
    if (field.blend == kQuat) {
        Quat older = quatAt(h.words[0], base, field.offset);
        Quat newest = quatAt(h.words[1], base, field.offset);
        float olderMatrix[9], newestMatrix[9];
        matrixAt(h.words[0], base, field.matrix, olderMatrix);
        matrixAt(h.words[1], base, field.matrix, newestMatrix);
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
    matrixAt(h.words[0], base, field.offset, olderMatrix);
    matrixAt(h.words[1], base, field.offset, newestMatrix);
    Quat older, newest;
    if (!quatFromMatrix(olderMatrix, older) || !quatFromMatrix(newestMatrix, newest)) return;
    float blended[9];
    matrixFromQuat(slerp(older, newest, a), blended);

    std::memcpy(record + field.offset, blended, sizeof(blended));
}

// Writes every blended field of `record` from the pair, at `a` from the older copy to the
// newest. Kept fields are left as the latch wrote them, which is the newest copy.
void blendFields(const History& h, uint8_t* record, int base, const Field* fields, size_t count,
                 float a, float length) {
    for (size_t i = 0; i < count; ++i) {
        const Field& field = fields[i];
        if (field.blend == kKeep) continue;
        if (field.blend == kMatrix || field.blend == kQuat) {
            blendRotation(h, record, base, field, a);
            continue;
        }
        for (int k = 0; k < field.words; ++k) {
            int   offset = field.offset + 4 * k;
            float older = floatAt(h.words[0], base, offset);
            float newest = floatAt(h.words[1], base, offset);
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
    uintptr_t array = slot < kCarsPerArray ? kLiveCars : kReplayCars;
    return reinterpret_cast<uint8_t*>(array + (slot % kCarsPerArray) * kCarSize + kCarRecord);
}

int carSlot(uintptr_t car) {
    uintptr_t span = kCarSize * kCarsPerArray;
    if (car >= kLiveCars && car < kLiveCars + span && (car - kLiveCars) % kCarSize == 0)
        return static_cast<int>((car - kLiveCars) / kCarSize);
    if (car >= kReplayCars && car < kReplayCars + span && (car - kReplayCars) % kCarSize == 0)
        return kCarsPerArray + static_cast<int>((car - kReplayCars) / kCarSize);
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
    float worst = 0.0f;
    for (int k = 0; k < 3; ++k) {
        float older = floatAt(h.words[0], base, offset + 4 * k);
        float newest = floatAt(h.words[1], base, offset + 4 * k);
        float wrote = *reinterpret_cast<const float*>(record + offset + 4 * k);
        float low = older < newest ? older : newest;
        float high = older < newest ? newest : older;
        float over = wrote < low ? low - wrote : (wrote > high ? wrote - high : 0.0f);
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
         static_cast<double>(worst), static_cast<double>(floatAt(h.words[0], base, offset)),
         static_cast<double>(floatAt(h.words[0], base, offset + 4)),
         static_cast<double>(floatAt(h.words[0], base, offset + 8)),
         static_cast<double>(floatAt(h.words[1], base, offset)),
         static_cast<double>(floatAt(h.words[1], base, offset + 4)),
         static_cast<double>(floatAt(h.words[1], base, offset + 8)), h.ticks[0], h.ticks[1]);
}

// Blend one record to `target`. True when its pair was usable; only the first such record of a
// frame goes into the alpha statistics.
// Whether the records get blended at all. In VR that is `vrInterpolate`. In flat mode the blend
// only means anything when frames are drawn between the ticks, so it follows flatExtraFrames as
// well as its own key -- with flatInterpolate = 0 the extra frames redraw an unchanged world,
// which is the control that says how much of the smoothness is the blend.
bool blendingEnabled() {
    if (g_options.vr) return g_options.vrInterpolate;
    return g_options.flatExtraFrames > 0 && g_options.flatInterpolate;
}

// When a tick arrived, in QPC. RenderPacing's model is preferred where it exists; its own is
// fitted to the tick counter over a whole run, while this one is a two-point extrapolation from
// the last latch.
bool tickArrival(const State& s, int count, double& qpc) {
    if (renderPacingTickArrival(count, qpc)) return true;
    if (!s.haveTickModel || s.tickPeriod <= 0.0) return false;
    qpc = static_cast<double>(s.lastLatchQpc) + (count - s.lastLatchTick) * s.tickPeriod;
    return true;
}

bool blendHistory(State& s, const History& h, uint8_t* record, int base, const Field* fields,
                  size_t count, double target, float length, bool& sampled, bool rewriteAtOne) {
    if (h.copies < 2 || h.snapped) return false;
    double older = 0.0;
    double newest = 0.0;
    if (!tickArrival(s, h.ticks[0], older) || !tickArrival(s, h.ticks[1], newest) ||
        !(newest > older))
        return false;

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
    }
    // At the newest tick the record already holds what the latch wrote -- unless an earlier
    // frame of this same tick blended it somewhere else, which is what an extra frame does.
    if (alpha < 1.0 || rewriteAtOne)
        blendFields(h, record, base, fields, count, static_cast<float>(alpha), length);
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
    if ((parts & kPartView) &&
        blendHistory(s, s.view, reinterpret_cast<uint8_t*>(kViewRecord), 0, kViewFields,
                     sizeof(kViewFields) / sizeof(kViewFields[0]), target, length, sampled,
                     rewriteAtOne)) {
        ++w.viewBlended;
        any = true;
        // The view record's eye position is the first three floats; the car's is at its byte 16.
        checkBlendedPosition(s.view, reinterpret_cast<const uint8_t*>(kViewRecord), 0, 0, "the eye");
    }
    if ((parts & kPartMirror) && haveMirror &&
        blendHistory(s, s.mirror, reinterpret_cast<uint8_t*>(kMirrorRecord), 0, kViewFields,
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
        blendHistory(s, s.pitBoard, reinterpret_cast<uint8_t*>(kPitBoardRecord), 4,
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
            return;
        }
        ++w.costTimed;
        applyBlend(s, static_cast<double>(now) - s.tickPeriod, s.latched, s.latchedCount,
                   s.mirrorLatched, s.pitBoardLatched, false);
        return;
    }

    int       simHz = taskRateSimHz();
    long long display = 0;
    bool      fromPresenter = false;
    if (simHz <= 0 || !renderPacingDisplayTime(now, display, fromPresenter)) {
        ++w.untimed;
        return;
    }
    if (fromPresenter)
        ++w.presenterTimed;
    else
        ++w.costTimed;

    // The drawn state is the one a tick before the display time, less the frame's own smoothed
    // latch-to-display time, so it advances with the display clock and always lies between the
    // two latched ticks.
    double tick = static_cast<double>(s.frequency) / simHz;
    double sample = static_cast<double>(display - now);
    if (!s.haveLatency || std::fabs(sample - s.latency) > 3.0 * tick) {
        s.latency = sample;
        s.haveLatency = true;
    } else {
        s.latency += 0.05 * (sample - s.latency);
    }
    double target = static_cast<double>(display) - s.latency - tick;

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
        int  tick = *reinterpret_cast<const int*>(kLatchedTickCount);
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

        int identity[3] = {*reinterpret_cast<const int*>(kViewCameraMode),
                           *reinterpret_cast<const int*>(kViewCarDriverIndex),
                           *reinterpret_cast<const uint8_t*>(kViewIsLive)};
        pushView(s, s.view, reinterpret_cast<const uint8_t*>(kViewRecord), tick, identity, true);
        if (s.mirrorLatched)
            pushView(s, s.mirror, reinterpret_cast<const uint8_t*>(kMirrorRecord), tick, identity,
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
        }

        // A board hidden in this latch starts a new history when it shows again.
        if (s.pitBoardLatched)
            pushPitBoard(s, s.pitBoard, reinterpret_cast<const uint8_t*>(kPitBoardRecord), tick,
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
    if (copied && reinterpret_cast<uintptr_t>(destination) == kViewRecord)
        g_state.viewLatched = true;
    return copied;
}

int __fastcall mirrorLatchHook(void* self, void* unused, void* destination) {
    int copied = g_mirrorLatch(self, unused, destination);
    if (copied && reinterpret_cast<uintptr_t>(destination) == kMirrorRecord)
        g_state.mirrorLatched = true;
    return copied;
}

// 0 while the board is hidden; the record otherwise.
void* __cdecl pitBoardLatchHook() {
    void* record = g_pitBoardLatch();
    if (reinterpret_cast<uintptr_t>(record) == kPitBoardRecord) g_state.pitBoardLatched = true;
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
    if (!g_options.vr && g_options.flatExtraFrames <= 0) return;

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
        store != kLatchedTickCount || !enableMatches) {
        logf("interp: the tick wait at %08X is not the expected latch (count store %02X %08X, "
             "call at +0x44 %02X to %08X, fgEnableBG %08X) -- no interpolation",
             static_cast<unsigned>(wait), code[0x0E], static_cast<unsigned>(store), code[0x44],
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
         blendingEnabled() ? (g_options.vr ? "vrInterpolate = 1."
                                           : "flat mode: the blend follows flatExtraFrames.")
         : g_options.vr    ? "vrInterpolate = 0: records are kept but nothing is blended."
                           : "flat mode with flatInterpolate = 0: records are kept but nothing "
                             "is blended.");
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
    if (!s.installed || !blendingEnabled() || !s.haveTickModel || s.tickPeriod <= 0.0)
        return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // One tick behind the wall clock, which is where the latch's own blend put the world and
    // where it stays: at the instant of a latch this is the tick before it, and it reaches that
    // latch's own tick just as the next one lands. The lag is the sim tick interpolation has
    // always cost, not a new one.
    double target = static_cast<double>(now.QuadPart) - s.tickPeriod;

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
            "min %.2f max %.2f, clamped low %u high %u | snaps first %u identity %u tick gap %u "
            "jump %u camera %u nonfinite %u (%u of them the view and its car coupled) | view tick "
            "gaps 1:%u 2:%u 3:%u 4+:%u | cars blended %.1f a frame "
            "| view blended %u, mirror %u, pit board %u (snapped %u) | rotation: %u matrix "
            "fallbacks | blend left the pair %u times, worst %.2f m | display time from the "
            "presenter %u, from the render cost %u | target = display - %.2f ms - one tick %.2f "
            "ms | cost %.3f ms a frame | between ticks %u frames, %u re-blended, %.3f ms each",
            blendingEnabled() ? "on" : "off", w.frames, w.blended, w.frozen, w.untimed,
            w.alphas ? w.alphaSum / w.alphas : 0.0, w.alphas ? w.alphaMin : 0.0,
            w.alphas ? w.alphaMax : 0.0, w.clampedLow, w.clampedHigh, w.snaps[kSnapFirst],
            w.snaps[kSnapIdentity], w.snaps[kSnapGap], w.snaps[kSnapJump], w.snaps[kSnapCamera],
            w.snaps[kSnapNonFinite], w.viewCarCoupled, w.gaps[0], w.gaps[1], w.gaps[2],
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
