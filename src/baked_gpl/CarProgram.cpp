#include "baked_gpl/CarProgram.h"

#include "Log.h"

#include "baked/CarScene.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarConfig.h"
#include "baked_gpl/CarOrder.h"
#include "baked_gpl/CarPassView.h"
#include "baked_gpl/CarUnion.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gplvr {
namespace {

// CarWalk's arena holds 64 a pass; a car has a Car for the main passes and one per mirror glass
// (report 105 sections 4.42, 4.54): a field of 26 with the cockpit and three glasses needs 27 + 78.
constexpr size_t   kMaxCars = 256;
// Every car's configurations together (report 105 section 4.24). The vectors they hold stay under
// kBudgetBytes: past it the program taken least recently is dropped. The pool is above what the
// budget holds of the 2020 grid's programs (~380 KiB each, section 4.55), so the budget is the
// limit (section 4.54).
constexpr size_t   kMaxPrograms = 1024;
constexpr uint32_t kConfigsPerCar = 8;
// About 700 programs of the 2020 grid's (~380 KiB each, section 4.55): the mirror passes' lists by
// glass at a full-field start. While less than kFreeFloorBytes of address space is free, a kept
// build only replaces what it displaces (budgetFor).
constexpr size_t   kBudgetBytes = size_t{256} << 20;
constexpr uint64_t kFreeFloorBytes = uint64_t{384} << 20;
constexpr size_t   kMaxFrames = 257;       // the root and CarWalk's kMaxWalkFrames
constexpr int32_t  kMaxSelectorCases = 4096;
constexpr int32_t  kMaxPoolScan = 2048;    // as CarShare's
// Main passes: a program not taken for this many is dropped; after a build that could not be kept,
// the every-gate walk is tried again after kEveryGateRetry, the margin's after kMarginRetry, and a
// build refused for another reason (a fault, a refused leaf) after kRefusedRetry. A build whose
// hazards taught CarConfig new gates is tried again on the next pass, at most kLearnRetries times
// running. A car rebuilt within kChurnPasses of its last build kChurnLimit times running is walked
// as before for kChurnBackoff.
constexpr uint32_t kEvictPasses = 600;
constexpr uint32_t kEveryGateRetry = 1024;
constexpr uint32_t kMarginRetry = 64;
constexpr uint32_t kRefusedRetry = 8;
constexpr uint32_t kLearnRetries = 16;
constexpr uint32_t kChurnPasses = 2;
constexpr uint32_t kChurnLimit = 4;
constexpr uint32_t kChurnBackoff = 64;
// One reuse in kWholeEvery is assembled whole for the car renderer to check its rows (report 105
// section 4.27).
constexpr uint32_t kWholeEvery = 61;
// One replay in kPlaneCheckEvery compares the plane tests' floats with the model's (report 105
// section 4.28) and evaluates the conditions again without skips (section 4.37); a difference
// drops the program.
constexpr uint32_t kPlaneCheckEvery = 64;
// The cockpit's kept list (report 105 section 4.50) is verified against a fresh walk on one reuse
// in kCockpitVerifyEvery (one a main pass); a MISMATCH turns its kept lists off.
constexpr uint32_t kCockpitVerifyEvery = 32;

template <typename T>
T at(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

uint32_t word(uintptr_t node, unsigned index) { return at<uint32_t>(node + 4u * index); }
float    wordFloat(uintptr_t node, unsigned index) { return at<float>(node + 4u * index); }

uint32_t floatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// -- CarWalk's arithmetic, the same expressions in the same order -------------------------------

// poseMatrix: gpl_matrixTranslateEuler(t, r) from the XFORM's words 1-6 as they are now.
void poseMatrix(uintptr_t node, float m[12]) {
    float tx = wordFloat(node, 1), ty = wordFloat(node, 2), tz = wordFloat(node, 3);
    float yaw = wordFloat(node, 4), pitch = wordFloat(node, 5), roll = wordFloat(node, 6);
    float cz = std::cos(yaw), sz = std::sin(yaw);
    float cy = std::cos(pitch), sy = std::sin(pitch);
    float cx = std::cos(roll), sx = std::sin(roll);
    float out[12] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, tx,
                     sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, ty,
                     -sy,     cy * sx,                cy * cx,                tz};
    std::memcpy(m, out, sizeof(out));
}

// enterPose's clip and eye (no user planes: a kept list's walk has none).
void enterPose(const float parentClip[16], const float parentEye[3], const float m[12],
               float clip[16], float eye[3]) {
    for (int row = 0; row < 4; ++row) {
        const float* p = parentClip + row * 4;
        for (int column = 0; column < 4; ++column) {
            float sum = p[0] * m[column] + p[1] * m[4 + column] + p[2] * m[8 + column];
            if (column == 3) sum += p[3];
            clip[row * 4 + column] = sum;
        }
    }
    float d[3] = {parentEye[0] - m[3], parentEye[1] - m[7], parentEye[2] - m[11]};
    for (int j = 0; j < 3; ++j) eye[j] = m[j] * d[0] + m[4 + j] * d[1] + m[8 + j] * d[2];
}

// enterRoot: an XFORM's poses under its parent's (root from local).
void enterRoot(const float parent[12], const float m[12], float root[12]) {
    for (int row = 0; row < 3; ++row) {
        const float* p = parent + row * 4;
        for (int column = 0; column < 4; ++column) {
            float sum = p[0] * m[column] + p[1] * m[4 + column] + p[2] * m[8 + column];
            if (column == 3) sum += p[3];
            root[row * 4 + column] = sum;
        }
    }
}

// gpl_drawQueueSidePredicate 0x4C7880 on a plane's four floats.
bool frontSideOf(const float eye[3], const float plane[4]) {
    return plane[0] * eye[0] + plane[1] * eye[1] + plane[2] * eye[2] + plane[3] >= 0.0f;
}

// The same on the gate's plane as the model holds it now.
bool frontSide(const float eye[3], uintptr_t plane) {
    float p[4] = {at<float>(plane), at<float>(plane + 4), at<float>(plane + 8),
                  at<float>(plane + 12)};
    return frontSideOf(eye, p);
}

void vertexAt(uintptr_t vertex, float out[3]) {
    out[0] = at<float>(vertex + 4);
    out[1] = at<float>(vertex + 8);
    out[2] = at<float>(vertex + 12);
}

float clipW(const float m[16], const float p[3]) {
    return m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
}

uint32_t frustumOutcode(const float m[16], const float p[3]) {
    float clip[4];
    for (int row = 0; row < 4; ++row)
        clip[row] = m[row * 4] * p[0] + m[row * 4 + 1] * p[1] + m[row * 4 + 2] * p[2] +
                    m[row * 4 + 3];
    if (floatBits(clip[2]) >> 31) return 16;
    uint32_t w = floatBits(clip[3]) << 1;
    uint32_t code = 0;
    if (w < (floatBits(clip[0]) << 1)) code |= (floatBits(clip[0]) >> 31) ? 8u : 4u;
    if (w < (floatBits(clip[1]) << 1)) code |= (floatBits(clip[1]) >> 31) ? 2u : 1u;
    return code;
}

// gpl_nodeSelectorCompare 0x4CF880.
bool selectorTakes(uint32_t op, float value, float threshold) {
    switch (op) {
    case 1: return value <= threshold;
    case 2: return value >= threshold;
    case 3: return value == threshold;
    case 4: return value != 0.0f && threshold != 0.0f;
    default: return false;
    }
}

// The child word gpl_nodeSelectorRender visits for `value`: -1 none, -2 a case count it refuses.
int32_t selectorChoice(uintptr_t node, float value) {
    uint32_t op = word(node, 3);
    auto     count = static_cast<int32_t>(word(node, 4));
    if (count <= 0 || count > kMaxSelectorCases) return -2;
    if (op == 5) {
        long long index = std::llrint(value);
        if (index < 0 || index >= count) return -1;
        return 6 + 2 * static_cast<int32_t>(index);
    }
    for (int32_t i = 0; i < count; ++i)
        if (selectorTakes(op, value, wordFloat(node, 5 + 2 * static_cast<unsigned>(i))))
            return 6 + 2 * i;
    return -1;
}

uint32_t spriteColour(uintptr_t node) {
    return (word(node, 0) & 0x200) ? word(node, 2) : 0xFFFFFFFFu;
}

bool sameMaterial(const baked::CarMaterial& a, const baked::CarMaterial& b) {
    return a.textureHandle == b.textureHandle && a.texture == b.texture &&
           a.surfaceClass == b.surfaceClass && a.textured == b.textured &&
           a.specularExponent == b.specularExponent &&
           floatBits(a.specular) == floatBits(b.specular);
}

// emitSkinned's carriage of a corner through the root.
void localToRoot(const float m[12], const float in[3], bool point, float out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = m[row * 4] * in[0] + m[row * 4 + 1] * in[1] + m[row * 4 + 2] * in[2] +
                   (point ? m[row * 4 + 3] : 0.0f);
}

void rootToLocal(const float m[12], const float in[3], bool point, float out[3]) {
    float d[3] = {in[0] - (point ? m[3] : 0.0f), in[1] - (point ? m[7] : 0.0f),
                  in[2] - (point ? m[11] : 0.0f)};
    for (int j = 0; j < 3; ++j) out[j] = m[j] * d[0] + m[4 + j] * d[1] + m[8 + j] * d[2];
}

int16_t unitToShort(float value) {
    float clamped = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
    return static_cast<int16_t>(std::lround(clamped * 32767.0f));
}

// -- state ---------------------------------------------------------------------------------------

// Why a car's program is built (again).
enum Rebuild : int {
    kNoProgram,   // no kept list: a new car, an evicted one, or the last build was not kept
    kInputs,      // the car's root, the pass's masks, MATERIAL state or lighting, or the bake moved
    kRegisters,   // a register a selector read from the pass's file
    kChild,       // an XFORM's child pointer (a tyre swap, a lost wheel)
    kSprite,      // a BILLBOARD colour (the helmet tint)
    kLod,         // a kind-0 selector (the LOD) takes another child
    kCamera,      // a kind-1 or kind-2 selector (the selector camera) takes another child
    kGate,        // a decided gate turned: the eye left the margin
    kFrames,      // the tape names a frame it does not hold
    kFault,       // the replay faulted on a read
    // A configuration gate turned (report 105 section 4.24) and none of the car's other
    // programs is on this eye's side of it.
    kConfig,
    kGlass,       // a cockpit glass polygon's corner count is not the listed one (section 4.50)
    kRebuildCount
};

// Why a car is walked as before (CarShare.h) instead.
enum Walked : int {
    kGplMemo,    // GPL's vertex memo holds one of the program's vertices
    kPoolFull,   // GPL's vertex memo too full to scan
    kNoPool,     // GPL's vertex memo was not placed at install
    kWalkedCount
};

// One distinct (plane, frame) the side conditions test (report 105 section 4.28): the plane's
// four floats as the build read them from the model, and where they live, for the check that
// they have not moved.
struct PlaneTest {
    float     plane[4];
    uintptr_t address;
    uint16_t  frame;
};
constexpr uint32_t kNoTest = 0xFFFFFFFFu;  // Program::conditionTest of a box condition

// One car's list for one configuration: the sides of the configuration gates its walk met, and
// one LOD (its kind-0 selections).
struct Program {
    int32_t   car = -1;  // its g_cars index; -1: a free slot
    bool      valid = false;
    ShareTape tape;
    // The walk's list: every condition's draws, the order nodes' halves sealed.
    baked::CarDrawList list;
    float     margin = 0.0f;  // of the kept list; 0: every gate both ways
    uint32_t  lastTaken = 0;  // main passes
    uint32_t  built = 0;
    size_t    bytes = 0;         // its vectors, counted against kBudgetBytes
    uint32_t  serial = 0;        // this kept build's own: its assemblies' contentKey
    // The tape's side conditions as plane tests (report 105 section 4.28): each distinct
    // (plane, frame) once, and per condition its test (kNoTest for a box).
    std::vector<PlaneTest> tests;
    std::vector<uint32_t>  conditionTest;
    // Its assemblies' last drawOrders, by condition bits (not counted in `bytes`), and the order
    // patch's index (counted; CarOrder.h, report 105 section 4.53).
    OrderState order;
    // list.draws[i].condition, packed for emitRange (report 105 section 4.30).
    std::vector<uint16_t> drawConditions;
    // list.draws[i].polygons, with kDynamicDraw where its object is kDynamicCarObject, packed for
    // an order memo's counts (report 105 section 4.38).
    std::vector<uint32_t> drawPolygons;
    // Report 105 section 4.37, per condition index: the condition whose descendants are the
    // conditions [index, skipEnd[index]) (0: none), which the replay passes over while it is off.
    std::vector<uint16_t> skipOwner;
    std::vector<uint32_t> skipEnd;
};

// A car's programs, most recently taken first, and when it may be built again.
struct Car {
    uintptr_t record = 0;
    // Its programs serve the mirror passes (report 105 section 4.42): a car has one Car for the
    // main passes and one for the mirrors, whose inputs (registers, LOD scale, mirror flag) differ.
    bool      mirror = false;
    // Its programs are the player's cockpit's (report 105 section 4.50): the record's view through
    // the `cockpit` handle, a model of its own apart from the same record's `car`.
    bool      cockpit = false;
    // A mirror Car's glass (report 105 section 4.54): -1 in a main pass, or where the pass's glass
    // is not known.
    int8_t    pane = -1;
    // Its team model's `car` XFORM, which every car of the team shares, or the `cockpit` XFORM.
    uintptr_t handle = 0;
    int16_t   programs[kConfigsPerCar] = {};
    uint32_t  count = 0;
    float     nextMargin = 0.0f;  // of the build asked for
    uint32_t  lastTaken = 0;
    uint32_t  built = 0;
    uint32_t  churn = 0;
    uint32_t  learning = 0;  // builds running that were not kept but taught CarConfig gates
    uint32_t  everyGateAfter = 0;
    uint32_t  marginAfter = 0;
    uint32_t  buildAfter = 0;
};

std::vector<Program> g_programs;  // reserved to kMaxPrograms once: never reallocated
std::vector<Car>     g_cars;      // reserved to kMaxCars once
size_t               g_bytes = 0;  // of the valid programs
bool                 g_installed = false;
bool                 g_active = false;  // this pass is a main or mirror pass with the program on
bool                 g_mirror = false;  // this pass is a mirror pass (report 105 section 4.42)
uint32_t             g_pass = 0;        // main passes with the program on
uint32_t             g_reuses = 0;
uint32_t             g_serial = 0;      // the last kept build's Program::serial
int                g_building = -1;   // the program carProgramTapeBegin handed out
baked::CarDrawList   g_verifyList;      // the assembled list a verify walk is compared with
bool                 g_verifyPending = false;
bool                 g_saidNoCar = false;  // past kMaxCars, logged once
// The cockpit's kept list (report 105 section 4.50): whether the call being served is the
// cockpit's (from carProgramTake to the build or verify it asked for), its own reuse count for
// kCockpitVerifyEvery, the Car a verify walk is for, and the car set a MISMATCH turned the
// cockpit's programs off in.
bool                 g_cockpitCall = false;
uint32_t             g_cockpitReuses = 0;
int                  g_verifyCar = -1;
bool                 g_cockpitOff = false;
uint32_t             g_cockpitOffGeneration = 0;

// The re-made frames, by walk frame id, and this eye's conditions (CarDrawList::conditionOn's
// layout).
float                g_clips[kMaxFrames][16];
float                g_eyes[kMaxFrames][3];
float                g_roots[kMaxFrames][12];
uint16_t             g_half[kMaxFrames];
std::vector<uint8_t> g_on;
uint32_t             g_replays = 0;    // replays run, for kPlaneCheckEvery
// Per plane test of the program being replayed: 2 not tested yet, else the eye's side (1 front).
std::vector<uint8_t> g_side;
// g_on and g_side as the skip check evaluates them, every condition visited (section 4.37).
std::vector<uint8_t> g_onCheck;
std::vector<uint8_t> g_sideCheck;

bool on(uint16_t condition) { return condition < g_on.size() && g_on[condition] != 0; }

Car* findCar(uintptr_t record, bool mirror, bool cockpit, int pane) {
    for (Car& c : g_cars)
        if (c.record == record && c.mirror == mirror && c.cockpit == cockpit && c.pane == pane)
            return &c;
    return nullptr;
}

Car* allocateCar(uintptr_t record, uintptr_t handle, bool mirror, bool cockpit, int pane) {
    if (g_cars.capacity() < kMaxCars) g_cars.reserve(kMaxCars);
    Car* car = nullptr;
    for (Car& c : g_cars)
        if (!c.record) {
            car = &c;
            break;
        }
    if (!car) {
        if (g_cars.size() >= kMaxCars) return nullptr;
        g_cars.emplace_back();
        car = &g_cars.back();
    }
    *car = Car{};
    car->record = record;
    car->mirror = mirror;
    car->cockpit = cockpit;
    car->pane = static_cast<int8_t>(pane);
    car->handle = handle;
    return car;
}

int carIndex(const Car& car) { return static_cast<int>(&car - g_cars.data()); }

enum Verdict {
    kAgrees,
    kRebuildIt,  // `reason` a Rebuild
    kWalkIt,     // `reason` a Walked
};

// GPL's vertex memo, as gplMemoSlot reads it: none of the program's vertex records the source of
// a live slot. A source in [low, high] is looked up among them (report 105 section 4.43).
Verdict scanPool(const ShareTape& tape, const ShareCall& call, int& reason) {
    if (!call.poolIndex || !call.poolSources) {
        reason = kNoPool;
        return kWalkIt;
    }
    if (tape.high < tape.low) return kAgrees;
    int32_t used = at<int32_t>(call.poolIndex);
    if (used <= 0) return kAgrees;
    if (used > kMaxPoolScan) {
        reason = kPoolFull;
        return kWalkIt;
    }
    for (int32_t i = 0; i < used; ++i) {
        uint32_t source = at<uint32_t>(call.poolSources + 4u * static_cast<uint32_t>(i));
        if (source < tape.low || source > tape.high) continue;
        if (shareReadsVertex(tape, source)) {
            reason = kGplMemo;
            return kWalkIt;
        }
    }
    return kAgrees;
}

// What the call reads at the car's dispatch besides the camera and the decisions, against the
// build's: the car's root, the pass's masks and lighting, the root MATERIAL state and the bake.
// The list's kinds are compared too (the cockpit, drawn over the eye image, a mirror's), though a
// Car's programs are all of one kind (report 105 section 4.50).
bool sameInputs(const Program& p, const ShareCall& call, const baked::CarDrawList& fresh) {
    const ShareTape& tape = p.tape;
    return tape.root == call.root && tape.handle == call.handle && tape.driver == call.driver &&
           tape.classVisible == call.classVisible && tape.lightingOn == call.lightingOn &&
           tape.textureList == call.textureList && tape.texturedMask == call.texturedMask &&
           call.planeMask == 0 && !call.cellPlanes && tape.generation == carBakeGeneration() &&
           tape.rebaked == carBakeTotals().objectsRebaked && fresh.driver == p.list.driver &&
           fresh.cockpit == p.list.cockpit && fresh.overlay == p.list.overlay &&
           fresh.mirror == p.list.mirror &&
           !p.list.flagman && !fresh.materials.empty() && !p.list.materials.empty() &&
           sameMaterial(fresh.materials[0], p.list.materials[0]);
}

// Whether box tape.boxes[box] culls for this eye: every corner off one side of the frustum, from
// the frame its outcode came from (BOXCULL, 0x4D7210).
bool boxCulled(const ShareTape& tape, size_t box, size_t frames, bool& valid) {
    if (box >= tape.boxes.size()) {
        valid = false;
        return false;
    }
    const ShareBox& b = tape.boxes[box];
    uint32_t        common = 2047;
    for (unsigned corner = 0; corner < 8; ++corner) {
        if (b.corner[corner] >= frames) {
            valid = false;
            return false;
        }
        float p[3];
        vertexAt(word(b.node, 1 + corner), p);
        common &= frustumOutcode(g_clips[b.corner[corner]], p) << 6;
    }
    return common != 0;
}

// The conditions for this eye into `on` (CarDrawList::conditionOn's layout), parents before
// children; each plane test once, its side kept in `side`. With kSkip, a run of a condition's
// descendants (Program::skipOwner, report 105 section 4.37) is passed over while that condition is
// off, its entries left 0 as the parent checks would leave them. The frames re-made into g_clips
// and g_eyes; `frames` their count.
template <bool kSkip>
Verdict evaluateConditions(const Program& p, size_t frames, std::vector<uint8_t>& on,
                           std::vector<uint8_t>& side, int& reason) {
    const ShareTape& tape = p.tape;
    on.assign(tape.conditions.size() + 1, 0);
    on[0] = 1;
    side.assign(p.tests.size(), 2);
    uint8_t* bits = on.data();
    for (size_t k = 0; k < tape.conditions.size(); ++k) {
        if constexpr (kSkip) {
            uint16_t owner = p.skipOwner[k];
            if (owner && !bits[owner]) {
                k = p.skipEnd[k] - 1u;
                continue;
            }
        }
        const ShareCondition& c = tape.conditions[k];
        if (c.parent > k) {
            reason = kFrames;
            return kRebuildIt;
        }
        if (!bits[c.parent]) continue;
        if (c.kind == kConditionBox) {
            bool valid = true;
            bool culled = boxCulled(tape, c.plane, frames, valid);
            if (!valid) {
                reason = kFrames;
                return kRebuildIt;
            }
            bits[k + 1] = culled ? 0 : 1;
        } else if (c.kind == kConditionChild) {
            if (c.frame == 0 || c.frame >= frames) {
                reason = kFrames;
                return kRebuildIt;
            }
            bits[k + 1] = word(tape.frames[c.frame - 1u].node, 8) == c.plane ? 1 : 0;
        } else {
            uint32_t index = p.conditionTest[k];
            if (index >= p.tests.size() || c.frame >= frames) {
                reason = kFrames;
                return kRebuildIt;
            }
            uint8_t& eyeSide = side[index];
            if (eyeSide == 2) eyeSide = frontSideOf(g_eyes[c.frame], p.tests[index].plane) ? 1 : 0;
            bits[k + 1] = (eyeSide != 0) == c.front ? 1 : 0;
        }
    }
    return kAgrees;
}

// The replay of a program for this call: what GPL may have moved, then the frames re-made from
// the XFORM words GPL's pose wrote for this car call, then every condition and decision from this
// eye.
Verdict replay(const Program& p, const ShareCall& call, int& reason) {
    const ShareTape& tape = p.tape;
    ++g_replays;
    for (const ShareRegister& r : tape.registers)
        if (floatBits(static_cast<float>(call.registers[r.index & 0x3FF])) != r.valueBits) {
            reason = kRegisters;
            return kRebuildIt;
        }
    size_t frames = tape.frames.size() + 1;
    if (frames > kMaxFrames || p.list.frames.size() != frames) {
        reason = kFrames;
        return kRebuildIt;
    }
    // A wheel XFORM the build took every model of may point at any of them (report 105 section
    // 4.30); any other child is a new one.
    for (const ShareFrame& f : tape.frames) {
        uint32_t child = word(f.node, 8);
        if (child == f.words[6]) continue;
        bool known = false;
        for (uint32_t k = f.switchFirst; k < f.switchFirst + f.switchCount; ++k)
            known = known || (k >= 1 && k <= tape.conditions.size() &&
                              tape.conditions[k - 1].plane == child);
        if (!known) {
            reason = kChild;
            return kRebuildIt;
        }
    }
    for (const ShareSprite& s : tape.sprites)
        if (spriteColour(s.node) != s.colour) {
            reason = kSprite;
            return kRebuildIt;
        }
    // The cockpit's glass: the assembly takes `count` UV pairs from each node (section 4.50).
    for (const ShareGlass& g : tape.glass)
        if (word(g.node, 3) != g.count) {
            reason = kGlass;
            return kRebuildIt;
        }
    Verdict pool = scanPool(tape, call, reason);
    if (pool != kAgrees) return pool;

    static const float kIdentity[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                        0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::memcpy(g_clips[0], call.clip, sizeof(g_clips[0]));
    std::memcpy(g_eyes[0], call.eye, sizeof(g_eyes[0]));
    std::memcpy(g_roots[0], kIdentity, sizeof(g_roots[0]));
    g_half[0] = 0;
    for (size_t k = 0; k < tape.frames.size(); ++k) {
        const ShareFrame& f = tape.frames[k];
        if (f.parent > k) {
            reason = kFrames;
            return kRebuildIt;
        }
        float pose[12];
        poseMatrix(f.node, pose);
        enterPose(g_clips[f.parent], g_eyes[f.parent], pose, g_clips[k + 1], g_eyes[k + 1]);
        enterRoot(g_roots[f.parent], pose, g_roots[k + 1]);
        // GPL rebuilds the half vector only where a rotation ran (enterFrame).
        bool rotates = wordFloat(f.node, 4) != 0.0f || wordFloat(f.node, 5) != 0.0f ||
                       wordFloat(f.node, 6) != 0.0f;
        g_half[k + 1] = rotates ? static_cast<uint16_t>(k + 1) : g_half[f.parent];
    }
    // The plane tests hold the planes as the build read them; now and then they are compared
    // with the model, and a plane that moved drops the program.
    if (p.conditionTest.size() != tape.conditions.size() ||
        p.skipOwner.size() != tape.conditions.size() ||
        p.skipEnd.size() != tape.conditions.size()) {
        reason = kFrames;
        return kRebuildIt;
    }
    bool check = g_replays % kPlaneCheckEvery == 0;
    if (check) {
        for (const PlaneTest& test : p.tests)
            if (std::memcmp(test.plane, reinterpret_cast<const void*>(test.address),
                            sizeof(test.plane)) != 0) {
                reason = kInputs;
                return kRebuildIt;
            }
    }
    Verdict conditions = evaluateConditions<true>(p, frames, g_on, g_side, reason);
    if (conditions != kAgrees) return conditions;
    // On the plane checks' replays the conditions are evaluated again with every one visited; a
    // difference drops the program (report 105 section 4.37).
    if (check) {
        int why = 0;
        if (evaluateConditions<false>(p, frames, g_onCheck, g_sideCheck, why) != kAgrees ||
            g_onCheck != g_on) {
            reason = kInputs;
            return kRebuildIt;
        }
    }
    // Configuration boxes (report 105 section 4.24): culled or drawn as at the build.
    for (size_t k = 0; k < tape.boxes.size(); ++k) {
        const ShareBox& b = tape.boxes[k];
        if (!b.config || !on(b.condition)) continue;
        bool valid = true;
        bool culled = boxCulled(tape, k, frames, valid);
        if (!valid) {
            reason = kFrames;
            return kRebuildIt;
        }
        if (culled != b.culled) {
            reason = kConfig;
            return kRebuildIt;
        }
    }
    for (const ShareGate& g : tape.gates) {
        if (!on(g.condition)) continue;
        if (g.frame >= frames) {
            reason = kFrames;
            return kRebuildIt;
        }
        if (frontSide(g_eyes[g.frame], g.plane) != g.front) {
            reason = g.config ? kConfig : kGate;
            return kRebuildIt;
        }
    }
    for (const ShareSelector& s : tape.selectors) {
        if (!on(s.condition)) continue;
        float value = 0.0f;
        if (s.kind == 0) {
            if (s.frame >= frames) {
                reason = kFrames;
                return kRebuildIt;
            }
            float v[3];
            vertexAt(s.param, v);
            value = clipW(g_clips[s.frame], v) * call.lodScale;
            if (value < 0.0f) value = 0.0f;
        } else {
            value = s.kind == 1 ? call.dlong : call.dlat;
        }
        if (selectorChoice(s.node, value) != s.chosen) {
            reason = s.kind == 0 ? kLod : kCamera;
            return kRebuildIt;
        }
    }
    return kAgrees;
}

// The replay reads GPL's graph where the build read it, but a fault ends the replay, not the game.
Verdict replayGuarded(const Program& p, const ShareCall& call, int& reason) {
    __try {
        return replay(p, call, reason);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reason = kFault;
        return kRebuildIt;
    }
}

// -- assembly ------------------------------------------------------------------------------------

// This eye's list from the program, with the header `fresh` was begun with. A list to draw holds
// the program's draws and this eye's by index (drawOrder, report 105 section 4.29); unless
// `whole`, the draws and materials are left out when the car renderer holds the program's rows
// (section 4.27), and a whole one with rows held is marked for the renderer to check them
// against. A list for a verify walk (`verify`) holds this eye's draws themselves, as a walk's.
// The drawOrder comes from the program's memo when these condition bits made it before (section
// 4.29.1), else is patched from the nearest memo or emitted whole (CarOrder.h, section 4.53); a
// verify walk's list is always emitted whole.
void assemble(Program& p, const baked::CarDrawList& fresh, baked::CarDrawList& out, bool whole,
              bool verify) {
    if (&out != &fresh) {
        out.clear();
        out.driver = fresh.driver;
        out.cockpit = fresh.cockpit;
        out.overlay = fresh.overlay;
        out.mirror = fresh.mirror;
        std::memcpy(out.rootModelView, fresh.rootModelView, sizeof(out.rootModelView));
        std::memcpy(out.projection, fresh.projection, sizeof(out.projection));
        std::memcpy(out.light, fresh.light, sizeof(out.light));
        out.walkSlot = fresh.walkSlot;
        out.flagman = false;
    }
    size_t frames = p.tape.frames.size() + 1;
    out.frames.resize(frames);
    for (size_t k = 0; k < frames; ++k) {
        baked::CarFrame& f = out.frames[k];
        std::memcpy(f.rootFromLocal, g_roots[k], sizeof(f.rootFromLocal));
        std::memcpy(f.eye, g_eyes[k], sizeof(f.eye));
        f.halfFrom = g_half[k];
    }
    OrderSource source{p.tape.orders.data(),       static_cast<uint32_t>(p.tape.orders.size()),
                       p.drawConditions.data(),    static_cast<uint32_t>(p.drawConditions.size()),
                       p.drawPolygons.data(),      g_on.data(),
                       static_cast<uint32_t>(g_on.size())};
    OrderMemo* memo = orderTake(source, p.order, verify, g_reuses, out.drawOrder);
    // An overlay list (the cockpit, report 105 section 4.50) keeps its draws and materials: the car
    // renderer counts its leaves by material as it draws them (the glass decides the mirror copy).
    uint32_t heldDraws = 0;
    bool     held = !verify && !fresh.overlay &&
                bakedCarRowsPinned(p.serial, static_cast<uint32_t>(frames), heldDraws);
    out.drawsElided = held && !whole;
    out.elidedDraws = out.drawsElided ? heldDraws : 0;
    out.contentCheck = held && whole;
    if (out.drawsElided) {
        out.draws.clear();
        out.materials.clear();
    } else if (verify) {
        out.draws.clear();
        for (uint32_t index : out.drawOrder) {
            out.draws.push_back(p.list.draws[index]);
            out.draws.back().condition = 0;
        }
        out.materials = p.list.materials;
    } else {
        out.draws = p.list.draws;
        out.materials = p.list.materials;
    }
    out.dynamicIndices = p.list.dynamicIndices;
    out.dynamicVertices = p.list.dynamicVertices;
    // emitSkinned's corners, carried through the re-made frames.
    const baked::CarScene& scene = carBakeScene();
    for (const ShareSkin& skin : p.tape.skins) {
        const baked::CarObject& object = scene.objects[skin.object];
        for (uint32_t i = 0; i < skin.count; ++i) {
            baked::CarVertex vertex = object.vertices[skin.base + i];
            uint16_t         corner = p.tape.skinFrames[skin.firstCorner + 2 * i];
            uint16_t         normalFrame = p.tape.skinFrames[skin.firstCorner + 2 * i + 1];
            float            rooted[3];
            if (corner != skin.frame) {
                localToRoot(g_roots[corner], vertex.position, true, rooted);
                rootToLocal(g_roots[skin.frame], rooted, true, vertex.position);
            }
            if (skin.lit && normalFrame != skin.frame) {
                float normal[3] = {vertex.normal[0] / 32767.0f, vertex.normal[1] / 32767.0f,
                                   vertex.normal[2] / 32767.0f};
                float local[3];
                localToRoot(g_roots[normalFrame], normal, false, rooted);
                rootToLocal(g_roots[skin.frame], rooted, false, local);
                for (int k = 0; k < 3; ++k) vertex.normal[k] = unitToShort(local[k]);
            }
            out.dynamicVertices[skin.firstVertex + i] = vertex;
        }
    }
    // The cockpit's glass with the UVs MirrorUv keeps in GPL's PRIM now (report 105 section 4.50);
    // the replay found every node's corner count the listed one.
    for (const ShareGlass& g : p.tape.glass) {
        const auto*       uv = reinterpret_cast<const float*>(g.node + 16);
        baked::CarVertex* corners = &out.dynamicVertices[g.firstVertex];
        for (uint32_t i = 0; i < g.count; ++i) {
            corners[i].uv[0] = uv[2 * i];
            corners[i].uv[1] = uv[2 * i + 1];
        }
    }
    out.conditionOn.clear();
    out.sealed = 0;
    out.polygons = 0;
    out.emptyLeaves = 0;
    out.dynamicLeaves = 0;
    if (memo) {
        out.polygons = memo->polygons;
        out.dynamicLeaves = memo->dynamicLeaves;
    } else {
        for (const baked::CarDraw& draw : out.draws) {
            out.polygons += draw.polygons;
            if (draw.object == baked::kDynamicCarObject) out.dynamicLeaves += draw.polygons;
        }
    }
    // The draws are the program's; which of them this eye draws, and in what order, is drawOrder.
    out.contentKey = verify ? 0 : p.serial;
    out.orderKey = memo ? memo->key : 0;
    if (verify) out.drawOrder.clear();
}

// Whether every skinned leaf's recipe names corners the scene and the list hold, and every glass
// corners the list holds.
bool skinsValid(const ShareTape& tape, const baked::CarDrawList& list) {
    const baked::CarScene& scene = carBakeScene();
    size_t                 frames = tape.frames.size() + 1;
    for (const ShareSkin& skin : tape.skins) {
        if (skin.object >= scene.objects.size() ||
            skin.base + static_cast<size_t>(skin.count) >
                scene.objects[skin.object].vertices.size() ||
            skin.firstVertex + static_cast<size_t>(skin.count) > list.dynamicVertices.size() ||
            skin.firstCorner + 2 * static_cast<size_t>(skin.count) > tape.skinFrames.size() ||
            skin.frame >= frames)
            return false;
        for (uint32_t i = 0; i < 2 * skin.count; ++i)
            if (tape.skinFrames[skin.firstCorner + i] >= frames) return false;
    }
    // And every cockpit glass's corners (report 105 section 4.50).
    for (const ShareGlass& g : tape.glass)
        if (g.count < 3 ||
            g.firstVertex + static_cast<size_t>(g.count) > list.dynamicVertices.size())
            return false;
    return true;
}

// A plane's four floats from the model; false when the read faults.
bool readPlane(uintptr_t address, float out[4]) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), 4 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The program's plane tests from its tape (report 105 section 4.28): the side conditions'
// distinct (plane, frame), each with the plane as the model holds it now -- the walk that made the
// tape has just read it; a box or a wheel model has none (kNoTest). False when a plane cannot be
// read.
bool compileTests(Program& p) {
    const std::vector<ShareCondition>& conditions = p.tape.conditions;
    p.tests.clear();
    p.conditionTest.assign(conditions.size(), kNoTest);
    std::unordered_map<uint64_t, uint32_t> byPlane;
    byPlane.reserve(conditions.size());
    for (size_t k = 0; k < conditions.size(); ++k) {
        const ShareCondition& c = conditions[k];
        if (c.kind != kConditionSide) continue;
        uint64_t key = (static_cast<uint64_t>(c.plane) << 16) | c.frame;
        auto     found = byPlane.find(key);
        if (found != byPlane.end()) {
            p.conditionTest[k] = found->second;
            continue;
        }
        PlaneTest test;
        if (!readPlane(c.plane, test.plane)) return false;
        test.address = c.plane;
        test.frame = c.frame;
        auto index = static_cast<uint32_t>(p.tests.size());
        p.tests.push_back(test);
        byPlane.emplace(key, index);
        p.conditionTest[k] = index;
    }
    return true;
}

// The program's skip runs from its tape (report 105 section 4.37). A condition's descendants are
// the conditions created while the walk was under it, so they are normally one run of indices,
// though not the one right after it (a gate's front and back conditions come before either
// side's subtree); a run is recorded at its first index only where the indices are contiguous.
// Two conditions cannot share a first index: one would be the other's ancestor and have that
// other as a descendant at a lower index. A tape with a parent at or after its child gets no runs;
// the replay refuses it.
void compileSkips(Program& p) {
    const std::vector<ShareCondition>& conditions = p.tape.conditions;
    size_t n = conditions.size();
    p.skipOwner.assign(n, 0);
    p.skipEnd.assign(n, 0);
    for (size_t k = 0; k < n; ++k)
        if (conditions[k].parent > k) return;
    // Per condition id, over its descendants: the lowest and highest index, and their count. A
    // child's id is above its parent's, so walking the ids down completes a child before it is
    // folded into its parent; a condition's own index is below its descendants'.
    std::vector<uint32_t> first(n + 1, 0), last(n + 1, 0), count(n + 1, 0);
    for (size_t id = n; id >= 1; --id) {
        auto     index = static_cast<uint32_t>(id - 1);
        uint16_t parent = conditions[index].parent;
        uint32_t high = count[id] ? last[id] : index;
        if (count[parent]) {
            first[parent] = std::min(first[parent], index);
            last[parent] = std::max(last[parent], high);
        } else {
            first[parent] = index;
            last[parent] = high;
        }
        count[parent] += count[id] + 1;
    }
    for (size_t id = 1; id <= n; ++id) {
        if (!count[id]) continue;
        uint32_t start = first[id];
        if (last[id] - start + 1 != count[id] || p.skipOwner[start]) continue;
        p.skipOwner[start] = static_cast<uint16_t>(id);
        p.skipEnd[start] = last[id] + 1;
    }
}

size_t vectorBytes(const Program& p);

// -- the program pool ----------------------------------------------------------------------------

// Takes program `index` out of its car's list and frees its vectors.
void dropProgram(int index) {
    Program& p = g_programs[static_cast<size_t>(index)];
    if (p.car >= 0 && static_cast<size_t>(p.car) < g_cars.size()) {
        Car&     car = g_cars[static_cast<size_t>(p.car)];
        uint32_t kept = 0;
        for (uint32_t i = 0; i < car.count; ++i)
            if (car.programs[i] != index) car.programs[kept++] = car.programs[i];
        car.count = kept;
    }
    if (p.valid) g_bytes -= std::min(g_bytes, p.bytes);
    p = Program{};
}

// The valid program taken least recently, other than `except`; -1 when there is none.
int leastRecent(int except) {
    int best = -1;
    for (size_t i = 0; i < g_programs.size(); ++i) {
        const Program& p = g_programs[i];
        if (!p.valid || static_cast<int>(i) == except) continue;
        uint32_t age = g_pass - p.lastTaken;
        if (best < 0 || age > g_pass - g_programs[static_cast<size_t>(best)].lastTaken)
            best = static_cast<int>(i);
    }
    return best;
}

// The bytes the programs may hold once a kept build of `added` bytes is counted: kBudgetBytes, or
// from the first kept build that finds less than kFreeFloorBytes of address space free (or
// GlobalMemoryStatusEx failing) on, what they held before that build, at least kFloorCapMin: the
// heap keeps what dropped programs free, so new programs reuse it instead of taking address
// space, and free address space does not come back to lift the cap (report 105 section 4.55.1).
constexpr size_t kFloorCapMin = size_t{32} << 20;
size_t           g_floorCap = 0;  // 0 until the floor is met

size_t budgetFor(size_t added) {
    if (!g_floorCap) {
        MEMORYSTATUSEX status = {};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status) && status.ullAvailVirtual >= kFreeFloorBytes)
            return kBudgetBytes;
        size_t before = g_bytes > added ? g_bytes - added : 0;
        g_floorCap = std::min(kBudgetBytes, std::max(kFloorCapMin, before));
        logf("baked cars: kept lists capped at %zu KiB -- the process has under %u MiB of address "
             "space free (report 105 section 4.55.1)",
             g_floorCap >> 10, static_cast<unsigned>(kFreeFloorBytes >> 20));
    }
    return g_floorCap;
}

// A slot for a new program of `car`: the car's least recent one when it holds kConfigsPerCar,
// else a free slot, else the program of any car taken least recently. -1 when there is none.
int programSlot(Car& car) {
    if (car.count >= kConfigsPerCar) {
        int index = car.programs[car.count - 1];
        dropProgram(index);
        return index;
    }
    if (g_programs.capacity() < kMaxPrograms) g_programs.reserve(kMaxPrograms);
    for (size_t i = 0; i < g_programs.size(); ++i)
        if (g_programs[i].car < 0) return static_cast<int>(i);
    if (g_programs.size() < kMaxPrograms) {
        g_programs.emplace_back();
        return static_cast<int>(g_programs.size() - 1);
    }
    int index = leastRecent(-1);
    if (index < 0) return -1;
    dropProgram(index);
    return index;
}

// Which build to ask for, or why not.
ProgramOutcome askBuild(Car& car, int why) {
    if (g_pass < car.buildAfter) return kProgramWalk;
    if (g_pass >= car.everyGateAfter)
        car.nextMargin = 0.0f;
    else if (g_pass >= car.marginAfter)
        car.nextMargin = kProgramMargin;
    else
        return kProgramWalk;
    // A car rebuilt on nearly every pass costs more than the walk its programs save. A mirror car's
    // run of builds also ends at a reuse (carProgramTake).
    if (why != kNoProgram && g_pass - car.built <= kChurnPasses) {
        if (++car.churn >= kChurnLimit) {
            car.churn = 0;
            car.buildAfter = g_pass + kChurnBackoff;
            return kProgramWalk;
        }
    } else if (why != kNoProgram) {
        car.churn = 0;
    }
    int slot = programSlot(car);
    if (slot < 0) return kProgramWalk;
    Program& p = g_programs[static_cast<size_t>(slot)];
    p.car = carIndex(car);
    p.valid = false;
    g_building = slot;
    return kProgramBuild;
}

size_t vectorBytes(const Program& p) {
    const baked::CarDrawList& l = p.list;
    const ShareTape&          t = p.tape;
    return l.frames.capacity() * sizeof(baked::CarFrame) +
           l.materials.capacity() * sizeof(baked::CarMaterial) +
           l.draws.capacity() * sizeof(baked::CarDraw) +
           l.dynamicVertices.capacity() * sizeof(baked::CarVertex) +
           l.dynamicIndices.capacity() * sizeof(uint32_t) + l.conditionOn.capacity() +
           t.frames.capacity() * sizeof(ShareFrame) + t.gates.capacity() * sizeof(ShareGate) +
           t.boxes.capacity() * sizeof(ShareBox) +
           t.selectors.capacity() * sizeof(ShareSelector) +
           t.registers.capacity() * sizeof(ShareRegister) +
           t.sprites.capacity() * sizeof(ShareSprite) +
           t.conditions.capacity() * sizeof(ShareCondition) +
           t.orders.capacity() * sizeof(ShareOrder) + t.skins.capacity() * sizeof(ShareSkin) +
           t.skinFrames.capacity() * sizeof(uint16_t) + t.vertices.capacity() * sizeof(uint32_t) +
           t.glass.capacity() * sizeof(ShareGlass) +
           p.tests.capacity() * sizeof(PlaneTest) +
           p.conditionTest.capacity() * sizeof(uint32_t) +
           p.drawConditions.capacity() * sizeof(uint16_t) +
           p.drawPolygons.capacity() * sizeof(uint32_t) +
           p.skipOwner.capacity() * sizeof(uint16_t) + p.skipEnd.capacity() * sizeof(uint32_t) +
           orderIndexBytes(p.order.index);
}

}  // namespace

void carProgramInstall() {
    if (g_installed) return;
    g_installed = true;
    logf("baked cars: lists kept across frames (report 105 sections 4.23, 4.24) -- in every baked "
         "pass a car's list is walked once with every gate both ways (or, where its memo cannot be "
         "followed so, every gate within %.2f m of the eye and every gate in the wheels), deciding "
         "for its eye every configuration gate, then re-posed and assembled for each eye and frame "
         "until its LOD, a register, a child pointer, the helmet colour, the pass's inputs or a "
         "decided gate changes; up to %u lists a car, %zu lists and %zu KiB for all cars, up to "
         "%zu Cars, dropped after %u main passes unused",
         kProgramMargin, kConfigsPerCar, kMaxPrograms, kBudgetBytes >> 10, kMaxCars, kEvictPasses);
}

void carProgramPassBegin(bool mirror, bool enabled, int pane) {
    carPassViewBegin(mirror, pane);
    g_building = -1;
    g_verifyPending = false;
    g_active = enabled;
    g_mirror = mirror;
    if (!enabled) return;
    // The passes a program's age and a car's retries count are the main passes; a mirror pass
    // takes and builds programs only (report 105 section 4.42).
    if (mirror) return;
    ++g_pass;
    for (size_t i = 0; i < g_programs.size(); ++i) {
        Program& p = g_programs[i];
        if (p.car < 0 || g_pass - p.lastTaken <= kEvictPasses) continue;
        dropProgram(static_cast<int>(i));
    }
    for (Car& car : g_cars)
        if (car.record && !car.count && g_pass - car.lastTaken > kEvictPasses) car = Car{};
}

void carProgramDropAll() {
    for (size_t i = 0; i < g_programs.size(); ++i)
        if (g_programs[i].car >= 0) dropProgram(static_cast<int>(i));
    for (Car& car : g_cars) car = Car{};
}

namespace {

// carProgramTake past the mirror cull; `taken` the program a reuse or verify took.
ProgramOutcome takeProgram(const ShareCall& call, baked::CarDrawList& list) {
    // The cockpit gplvr draws (report 105 section 4.48) keeps programs as a car does (section
    // 4.50), under a Car of its own, in the passes whose list holds the glass (step 2, section
    // 4.49): the glass's UVs are taken from GPL's PRIM at each assembly. A step-1 pass leaves the
    // glass to GPL through CockpitGlass.h's set, which only walks fill, so it walks as before, as
    // does every cockpit call after a verify walk's MISMATCH until the car set changes.
    if (list.cockpit) {
        if (g_cockpitOff && g_cockpitOffGeneration != carBakeGeneration()) g_cockpitOff = false;
        if (!list.overlay || !g_active || g_mirror || g_cockpitOff) return kProgramWalk;
    }
    if (!g_active) return kProgramWalk;
    // A mirror car's programs are per glass (report 105 section 4.54), or per car where the
    // pass's glass is not known.
    int pane = g_mirror ? carPassView().pane : -1;
    Car* car = findCar(call.record, g_mirror, list.cockpit, pane);
    if (!car) car = allocateCar(call.record, call.handle, g_mirror, list.cockpit, pane);
    if (!car) {
        if (!g_saidNoCar) {
            g_saidNoCar = true;
            logf("baked cars: more than %zu kept-list Cars -- the others are walked every pass "
                 "(logged once)",
                 kMaxCars);
        }
        return kProgramWalk;
    }
    car->lastTaken = g_pass;
    car->handle = call.handle;
    if (!car->count) return askBuild(*car, kNoProgram);
    // The car's programs, most recently taken first: the first whose inputs, reads and decisions
    // all hold for this call is taken. One that cannot hold again (the pass's inputs or the bake
    // moved, its tape names frames it does not hold, its replay faulted) is dropped.
    int      why = -1;
    Program* p = nullptr;
    uint32_t position = 0;
    while (position < car->count) {
        int      index = car->programs[position];
        Program& candidate = g_programs[static_cast<size_t>(index)];
        int      reason = kInputs;
        Verdict  verdict = kRebuildIt;
        if (sameInputs(candidate, call, list)) verdict = replayGuarded(candidate, call, reason);
        if (verdict == kWalkIt) return kProgramWalk;
        if (verdict == kAgrees) {
            p = &candidate;
            break;
        }
        if (why < 0) why = reason;
        if (reason == kInputs || reason == kFrames || reason == kFault) {
            dropProgram(index);
            continue;
        }
        ++position;
    }
    if (!p) return askBuild(*car, why < 0 ? kNoProgram : why);
    if (position > 0) {
        int index = car->programs[position];
        for (uint32_t i = position; i > 0; --i) car->programs[i] = car->programs[i - 1];
        car->programs[0] = static_cast<int16_t>(index);
    }
    p->lastTaken = g_pass;
    // A mirror car serves up to six cameras a frame (three glasses, two eyes) and builds a program
    // for each on consecutive passes; a reuse shows its builds are being taken (section 4.51).
    if (car->mirror) car->churn = 0;
    // The cockpit's reuses are verified now and then: a wrong list shows over the eye image.
    bool verify = list.cockpit && ++g_cockpitReuses % kCockpitVerifyEvery == 0;
    if (verify) g_verifyCar = carIndex(*car);
    // A verify walk compares the whole assembly; one reuse in kWholeEvery comes whole for the car
    // renderer to check the rows it holds.
    assemble(*p, list, verify ? g_verifyList : list,
             verify || (!list.cockpit && ++g_reuses % kWholeEvery == 0), verify);
    if (verify) {
        g_verifyPending = true;
        return kProgramVerify;
    }
    return kProgramReused;
}

}  // namespace

ProgramOutcome carProgramTake(const ShareCall& call, baked::CarDrawList& list) {
    g_building = -1;
    g_verifyPending = false;
    g_verifyCar = -1;
    g_cockpitCall = list.cockpit;
    carCullTakeBegin();
    // A mirror car whose root box the view culls draws nothing; it is not listed, and no kept list
    // is tried (report 105 section 4.54). The check of the skip walks the car as before: a culled
    // walk ends at the root box, and builds no empty program.
    CullVerdict cull = g_mirror && !list.cockpit ? carCullTest(call) : kCullGoOn;
    if (cull != kCullGoOn) return cull == kCullSkip ? kProgramEmpty : kProgramWalk;
    return takeProgram(call, list);
}

void carProgramListed(const baked::CarDrawList& list) { carCullListed(list); }

ShareTape* carProgramTapeBegin(const ShareCall& call, float& margin) {
    if (g_building < 0 || static_cast<size_t>(g_building) >= g_programs.size() ||
        g_programs[static_cast<size_t>(g_building)].car < 0 ||
        static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car) >= g_cars.size() ||
        g_cars[static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car)].record !=
            call.record) {
        g_building = -1;
        return nullptr;
    }
    Program&   p = g_programs[static_cast<size_t>(g_building)];
    ShareTape& tape = p.tape;
    margin = g_cars[static_cast<size_t>(p.car)].nextMargin;
    tape.record = call.record;
    tape.root = call.root;
    tape.handle = call.handle;
    tape.driver = call.driver;
    tape.classVisible = call.classVisible;
    tape.textureList = call.textureList;
    tape.texturedMask = call.texturedMask;
    tape.lightingOn = call.lightingOn;
    tape.generation = carBakeGeneration();
    tape.rebaked = carBakeTotals().objectsRebaked;
    tape.low = ~static_cast<uintptr_t>(0);
    tape.high = 0;
    tape.vertices.clear();
    tape.refusal = call.planeMask || call.cellPlanes ? kSharePlanes : kShareOk;
    std::memset(tape.written, 0, sizeof(tape.written));
    tape.frames.clear();
    tape.gates.clear();
    tape.boxes.clear();
    tape.selectors.clear();
    tape.registers.clear();
    tape.sprites.clear();
    tape.margin = margin;
    tape.conditions.clear();
    tape.orders.clear();
    tape.skins.clear();
    tape.skinFrames.clear();
    tape.glass.clear();
    std::memcpy(tape.rootEye, call.eye, sizeof(tape.rootEye));
    p.valid = false;
    return &tape;
}

bool carProgramTapeEnd(ShareTape& tape, int hazard, ShareRefusal refusal, baked::CarDrawList& list,
                       uint32_t learned) {
    if (g_building < 0 || static_cast<size_t>(g_building) >= g_programs.size() ||
        &g_programs[static_cast<size_t>(g_building)].tape != &tape ||
        g_programs[static_cast<size_t>(g_building)].car < 0 ||
        static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car) >= g_cars.size()) {
        g_building = -1;
        return false;
    }
    int      index = g_building;
    Program& p = g_programs[static_cast<size_t>(index)];
    Car&     car = g_cars[static_cast<size_t>(p.car)];
    g_building = -1;
    if (tape.refusal == kShareOk) tape.refusal = refusal;
    bool frames = tape.refusal == kShareFrames || list.frames.size() != tape.frames.size() + 1;
    if (hazard >= 0 || frames) {
        // Gates the hazards named are decided from the next build on (CarConfig.h): try again
        // then, at the same margin. Otherwise what the union cannot follow is a fact of the model
        // at this selection: step down.
        if (learned && !frames && ++car.learning <= kLearnRetries) {
            car.buildAfter = g_pass + 1;
        } else {
            car.learning = 0;
            if (car.nextMargin <= 0.0f)
                car.everyGateAfter = g_pass + kEveryGateRetry;
            else
                car.marginAfter = g_pass + kMarginRetry;
        }
        dropProgram(index);
        return false;
    }
    if (tape.refusal != kShareOk ||
        !ordersNest(tape.orders, 0, static_cast<uint32_t>(list.draws.size()), 0,
                    static_cast<uint32_t>(tape.orders.size()), 0) ||
        !skinsValid(tape, list) || !compileTests(p)) {
        car.buildAfter = g_pass + kRefusedRetry;
        dropProgram(index);
        return false;
    }
    shareSealVertices(tape);
    p.list = list;
    p.drawConditions.resize(list.draws.size());
    p.drawPolygons.resize(list.draws.size());
    for (size_t i = 0; i < list.draws.size(); ++i) {
        const baked::CarDraw& draw = list.draws[i];
        p.drawConditions[i] = draw.condition;
        p.drawPolygons[i] = (draw.polygons & ~kDynamicDraw) |
                            (draw.object == baked::kDynamicCarObject ? kDynamicDraw : 0u);
    }
    compileSkips(p);
    p.margin = car.nextMargin;
    p.valid = true;
    if (++g_serial == 0) g_serial = 1;
    p.serial = g_serial;
    // The order patch's index (report 105 section 4.53) over the draws and order nodes as kept;
    // ordersNest held above. A refused index leaves the program's misses to whole emissions.
    p.order = OrderState{};
    OrderSource source{tape.orders.data(),      static_cast<uint32_t>(tape.orders.size()),
                       p.drawConditions.data(), static_cast<uint32_t>(p.drawConditions.size()),
                       p.drawPolygons.data(),   nullptr,
                       static_cast<uint32_t>(tape.conditions.size() + 1)};
    orderIndexBuild(p.order.index, source);
    p.built = g_pass;
    p.lastTaken = g_pass;
    p.bytes = vectorBytes(p);
    g_bytes += p.bytes;
    car.built = g_pass;
    car.learning = 0;
    uint32_t count = std::min(car.count, kConfigsPerCar - 1);
    for (uint32_t i = count; i > 0; --i) car.programs[i] = car.programs[i - 1];
    car.programs[0] = static_cast<int16_t>(index);
    car.count = count + 1;
    size_t budget = budgetFor(p.bytes);
    while (g_bytes > budget) {
        int oldest = leastRecent(index);
        if (oldest < 0) break;
        dropProgram(oldest);
    }
    // The walking eye draws the sides it is on and its own order.
    list.conditionOn.assign(tape.conditions.size() + 1, 0);
    list.conditionOn[0] = 1;
    for (size_t k = 0; k < tape.conditions.size(); ++k)
        list.conditionOn[k + 1] = tape.conditions[k].holds ? 1 : 0;
    return true;
}

void carProgramVerify(const baked::CarDrawList& walked, bool walkedWhole) {
    if (!g_verifyPending) return;
    g_verifyPending = false;
    if (!walkedWhole) return;
    char why[256];
    if (carUnionSameDrawn(g_verifyList, walked, why, sizeof(why))) return;
    // The cockpit is drawn over the eye in painter's order, where a wrong list shows: its programs
    // are dropped and it walks every pass until the car set changes (report 105 section 4.50).
    logf("baked cars: the cockpit's kept list differs from a fresh walk (%s) -- it is walked every "
         "pass until the car set changes",
         why);
    g_cockpitOff = true;
    g_cockpitOffGeneration = carBakeGeneration();
    if (g_verifyCar < 0 || static_cast<size_t>(g_verifyCar) >= g_cars.size()) return;
    Car& car = g_cars[static_cast<size_t>(g_verifyCar)];
    for (uint32_t n = car.count; n > 0 && car.count; --n) dropProgram(car.programs[0]);
}

}  // namespace gplvr
