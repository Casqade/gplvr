#include "baked_gpl/CarProgram.h"

#include "Hooks.h"
#include "Log.h"

#include "baked/CarScene.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarConfig.h"
#include "baked_gpl/CarUnion.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gplvr {
namespace {

// CarWalk's arena holds 64 a pass; a car has a Car for the main passes and one for the mirrors
// (report 105 section 4.42).
constexpr size_t   kMaxCars = 128;
// Every car's configurations together (report 105 section 4.24); bakedCarConfigs 0 keeps one a
// car, as section 4.23 did. The vectors they hold stay under kBudgetBytes: past it the program
// taken least recently is dropped.
constexpr size_t   kMaxPrograms = 256;
constexpr uint32_t kConfigsPerCar = 8;
// About 256 programs of the 2020 grid's largest (section 4.31); 1.2 GiB of the process's address
// space was free after the track's bake.
constexpr size_t   kBudgetBytes = size_t{128} << 20;
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
// section 4.27); not a divisor or multiple of kProgramVerifyEvery, whose reuses go to a verify walk.
constexpr uint32_t kWholeEvery = 61;
// One replay in kPlaneCheckEvery compares the plane tests' floats with the model's (report 105
// section 4.28).
constexpr uint32_t kPlaneCheckEvery = 64;

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
    kRebuildCount
};

// Why a car is walked as before (stage 1) instead.
enum Walked : int {
    kGplMemo,    // GPL's vertex memo holds one of the program's vertices
    kPoolFull,   // GPL's vertex memo too full to scan
    kNoPool,     // GPL's vertex memo was not placed at install
    kBackedOff,  // both kinds of build could not be kept; waiting to try again
    kWaiting,    // the last build was refused for a fault or a leaf, or taught CarConfig gates
    kChurned,    // rebuilt on nearly every pass: backed off
    kNoSlot,     // past kMaxCars cars, or no program slot to build into
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

// The draws an eye draws (CarDrawList::drawOrder) under one set of condition bits, as an
// assembly emitted them (report 105 section 4.29.1).
struct OrderMemo {
    std::vector<uint8_t>  on;  // g_on
    std::vector<uint32_t> order;
    uint32_t              polygons = 0;
    uint32_t              dynamicLeaves = 0;
    uint32_t              swapped = 0;
    uint32_t              key = 0;  // CarDrawList::orderKey; 0: empty
    uint32_t              lastUsed = 0;  // g_reuses
};
// Section 4.35.1: with 2, a third of reuses emitted their order afresh (section 4.36).
constexpr int kOrderMemos = 4;
constexpr uint32_t kDynamicDraw = 0x80000000u;  // in Program::drawPolygons

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
    uint32_t  configGates = 0;   // configuration gates its walk decided
    uint32_t  serial = 0;        // this kept build's own: its assemblies' contentKey
    // The tape's side conditions as plane tests (report 105 section 4.28): each distinct
    // (plane, frame) once, and per condition its test (kNoTest for a box).
    std::vector<PlaneTest> tests;
    std::vector<uint32_t>  conditionTest;
    // Its assemblies' last drawOrders, by condition bits (not counted in `bytes`).
    OrderMemo orders[kOrderMemos];
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
    uintptr_t handle = 0;  // its team model's `car` XFORM, which every car of the team shares
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

struct Second {
    uint32_t passes = 0;  // main passes with the program on
    uint32_t passesOff = 0;
    // Report 105 section 4.42: mirror passes with the program on and off, and of the counts below
    // (calls, reused, builds, verifies) those in mirror passes.
    uint32_t mirrorPasses = 0;
    uint32_t mirrorPassesOff = 0;
    uint32_t mirrorCalls = 0;
    uint32_t mirrorReused = 0;
    uint32_t mirrorBuilds = 0;
    uint32_t mirrorVerifies = 0;
    uint32_t calls = 0;
    uint32_t callsOff = 0;
    uint32_t reused = 0;
    uint32_t verifies = 0;
    uint32_t verified = 0;
    uint32_t mismatched = 0;
    uint32_t verifyFaulted = 0;
    uint32_t builds = 0;
    uint32_t rebuilt[kRebuildCount] = {};
    uint32_t keptEvery = 0;
    uint32_t keptMargin = 0;
    uint32_t failed[kUnionRedoneCount] = {};
    uint32_t failedFrames = 0;
    uint32_t failedOther = 0;
    uint32_t failedLearned = 0;  // of the failed, those that taught CarConfig new gates
    uint32_t learnedGates = 0;
    // Hazards that named no gate or box CarConfig could take, by UnionRedone.
    uint32_t unlearned[kUnionRedoneCount] = {};
    uint32_t walked[kWalkedCount] = {};
    uint32_t evicted = 0;
    // Programs dropped for a car's kConfigsPerCar, for a full pool, for kBudgetBytes, and because
    // the pass's inputs, the tape's frames or a fault made them unusable.
    uint32_t droppedLimit = 0;
    uint32_t droppedPool = 0;
    uint32_t droppedBudget = 0;
    uint32_t droppedStale = 0;
    // Replays run (one a candidate program), and reuses taken from a program other than the one
    // the car's last reuse took.
    uint32_t replays = 0;
    uint32_t switched = 0;
    // A build or verify CarWalk asked for that found no program waiting for it: must be 0.
    uint32_t lost = 0;
    int64_t  reuseTicks = 0;
    int64_t  replayTicks = 0;
    int64_t  skinTicks = 0;
    uint32_t reuses = 0;
    // Reuses assembled without their draws (report 105 section 4.27), and whole ones marked for the
    // car renderer's check.
    uint32_t elided = 0;
    uint32_t wholeChecks = 0;
    // Reuses whose drawOrder came from the program's memo (report 105 section 4.29.1).
    uint32_t ordersMemoised = 0;
    // Report 105 section 4.37. Kept builds: skip runs, and conditions whose descendants are not
    // one run of indices or share a run's first index with another's (must be 0). Replays whose
    // conditions were evaluated again without skips, and those that differed (must be 0; the
    // program dropped).
    uint64_t keptSkipRuns = 0;
    uint32_t skipRunsRefused = 0;
    uint32_t skipChecks = 0;
    uint32_t skipsDiffered = 0;
    // Report 105 section 4.28: replays whose plane tests were compared with the model, those that
    // found a plane moved (the program dropped), and builds not kept for a plane that faulted.
    uint32_t planeChecks = 0;
    uint32_t planesMoved = 0;
    uint32_t planeFaults = 0;
    int64_t  buildTicks = 0;
    int64_t  plainTicks = 0;
    uint32_t plainWalks = 0;
    uint64_t poolEntries = 0;
    uint32_t poolScans = 0;
    // Scans that met a slot in the program's address range whose source is none of its vertices,
    // which the range alone refused (report 105 section 4.43).
    uint32_t poolNarrowed = 0;
    // Over the kept builds: the program's size. Over the reuses: draws drawn of the program's, and
    // order nodes whose halves this eye put the other way round from the build's.
    uint64_t keptLeaves = 0;
    uint64_t keptDraws = 0;
    uint64_t keptConditions = 0;
    uint64_t keptBoxes = 0;
    uint64_t keptOrders = 0;
    uint64_t keptGates = 0;
    uint64_t keptConfigGates = 0;
    uint64_t keptConfigBoxes = 0;
    uint64_t keptSkins = 0;
    uint64_t keptWheelsSwitched = 0;  // report 105 section 4.30
    uint64_t keptWheelsAsPosed = 0;
    uint64_t drawnDraws = 0;
    uint64_t programDraws = 0;
    uint64_t swapped = 0;
    char     firstMismatch[256] = {};
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
uint32_t             g_orderKeys = 0;   // the last OrderMemo::key handed out
int                  g_building = -1;   // the program carProgramTapeBegin handed out
baked::CarDrawList   g_verifyList;      // the assembled list a verify walk is compared with
bool                 g_verifyPending = false;
Second               g_second;

// The re-made frames, by walk frame id, and this eye's conditions (CarDrawList::conditionOn's
// layout).
float                g_clips[kMaxFrames][16];
float                g_eyes[kMaxFrames][3];
float                g_roots[kMaxFrames][12];
uint16_t             g_half[kMaxFrames];
std::vector<uint8_t> g_on;
uint32_t             g_swapped = 0;
// emitRange's output, one index a draw of the largest program met; it only grows (report 105
// section 4.36).
std::vector<uint32_t> g_orderScratch;
int64_t              g_skinTicks = 0;  // the last assembly's skinned corners
uint32_t             g_replays = 0;    // replays run, for kPlaneCheckEvery
// Per plane test of the program being replayed: 2 not tested yet, else the eye's side (1 front).
std::vector<uint8_t> g_side;
// g_on and g_side as the skip check evaluates them, every condition visited (section 4.37).
std::vector<uint8_t> g_onCheck;
std::vector<uint8_t> g_sideCheck;

bool on(uint16_t condition) { return condition < g_on.size() && g_on[condition] != 0; }

uint32_t configsPerCar() { return g_options.bakedCarConfigs ? kConfigsPerCar : 1u; }

Car* findCar(uintptr_t record, bool mirror) {
    for (Car& c : g_cars)
        if (c.record == record && c.mirror == mirror) return &c;
    return nullptr;
}

Car* allocateCar(uintptr_t record, uintptr_t handle, bool mirror) {
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
// a live slot. A source in [low, high] is looked up among them (report 105 section 4.43); one that
// is not among them is counted.
Verdict scanPool(const ShareTape& tape, const ShareCall& call, Second& t, int& reason) {
    if (!call.poolIndex || !call.poolSources) {
        reason = kNoPool;
        return kWalkIt;
    }
    if (tape.high < tape.low) return kAgrees;
    int32_t used = at<int32_t>(call.poolIndex);
    if (used <= 0) return kAgrees;
    t.poolEntries += static_cast<uint32_t>(used);
    ++t.poolScans;
    if (used > kMaxPoolScan) {
        reason = kPoolFull;
        return kWalkIt;
    }
    bool inRange = false;
    for (int32_t i = 0; i < used; ++i) {
        uint32_t source = at<uint32_t>(call.poolSources + 4u * static_cast<uint32_t>(i));
        if (source < tape.low || source > tape.high) continue;
        if (shareReadsVertex(tape, source)) {
            reason = kGplMemo;
            return kWalkIt;
        }
        inRange = true;
    }
    if (inRange) ++t.poolNarrowed;
    return kAgrees;
}

// What the call reads at the car's dispatch besides the camera and the decisions, against the
// build's: the car's root, the pass's masks and lighting, the root MATERIAL state and the bake.
bool sameInputs(const Program& p, const ShareCall& call, const baked::CarDrawList& fresh) {
    const ShareTape& tape = p.tape;
    return tape.root == call.root && tape.handle == call.handle && tape.driver == call.driver &&
           tape.classVisible == call.classVisible && tape.lightingOn == call.lightingOn &&
           tape.textureList == call.textureList && tape.texturedMask == call.texturedMask &&
           call.planeMask == 0 && !call.cellPlanes && tape.generation == carBakeGeneration() &&
           tape.rebaked == carBakeTotals().objectsRebaked && fresh.driver == p.list.driver &&
           !fresh.cockpit && !p.list.cockpit && fresh.mirror == p.list.mirror &&
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
Verdict replay(const Program& p, const ShareCall& call, Second& t, int& reason) {
    const ShareTape& tape = p.tape;
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
    Verdict pool = scanPool(tape, call, t, reason);
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
    bool check = ++g_replays % kPlaneCheckEvery == 0;
    if (check) {
        ++t.planeChecks;
        for (const PlaneTest& test : p.tests)
            if (std::memcmp(test.plane, reinterpret_cast<const void*>(test.address),
                            sizeof(test.plane)) != 0) {
                ++t.planesMoved;
                reason = kInputs;
                return kRebuildIt;
            }
    }
    Verdict conditions = evaluateConditions<true>(p, frames, g_on, g_side, reason);
    if (conditions != kAgrees) return conditions;
    // On the plane checks' replays the conditions are evaluated again with every one visited; a
    // difference drops the program (report 105 section 4.37).
    if (check) {
        ++t.skipChecks;
        int why = 0;
        if (evaluateConditions<false>(p, frames, g_onCheck, g_sideCheck, why) != kAgrees ||
            g_onCheck != g_on) {
            ++t.skipsDiffered;
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
Verdict replayGuarded(const Program& p, const ShareCall& call, Second& t, int& reason) {
    __try {
        return replay(p, call, t, reason);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reason = kFault;
        return kRebuildIt;
    }
}

// -- assembly ------------------------------------------------------------------------------------

// The draws [from, to) this eye draws, written by index at `out`, which returns past the last:
// every index is written and the end moves on by the draw's condition bit, so an unpredictable
// condition costs no branch. Each draw is visited once over an emission, so a buffer of the
// program's draw count holds every write (report 105 section 4.36).
uint32_t* copyDraws(const Program& p, uint32_t from, uint32_t to, uint32_t* out) {
    const std::vector<uint16_t>& conditions = p.drawConditions;
    if (to > conditions.size()) to = static_cast<uint32_t>(conditions.size());
    const uint8_t* bits = g_on.data();
    auto           count = static_cast<uint32_t>(g_on.size());
    for (uint32_t i = from; i < to; ++i) {
        uint16_t condition = conditions[i];
        *out = i;
        out += condition < count ? bits[condition] : 0u;
    }
    return out;
}

// The indices of the draws [begin, end) this eye draws, with the order nodes [orderBegin,
// orderEnd) inside them, each node's halves in this eye's order (ShareOrder); written at `out`,
// which returns past the last.
uint32_t* emitRange(const Program& p, uint32_t begin, uint32_t end, uint32_t orderBegin,
                    uint32_t orderEnd, uint32_t* out) {
    const std::vector<ShareOrder>& orders = p.tape.orders;
    uint32_t                       from = begin;
    uint32_t                       k = orderBegin;
    while (k < orderEnd) {
        const ShareOrder& o = orders[k];
        out = copyDraws(p, from, o.begin, out);
        if (on(o.condition)) {
            if (on(o.onFront) == o.front) {
                out = emitRange(p, o.begin, o.split, k + 1, o.splitOrder, out);
                out = emitRange(p, o.split, o.end, o.splitOrder, o.nested, out);
            } else {
                out = emitRange(p, o.split, o.end, o.splitOrder, o.nested, out);
                out = emitRange(p, o.begin, o.split, k + 1, o.splitOrder, out);
                ++g_swapped;
            }
        }
        from = o.end;
        k = o.nested;
    }
    return copyDraws(p, from, end, out);
}

// Whether the order nodes nest as emitRange walks them: each inside its parent's half, in order.
bool ordersNest(const std::vector<ShareOrder>& orders, uint32_t begin, uint32_t end,
                uint32_t orderBegin, uint32_t orderEnd, int depth) {
    if (depth > 4096) return false;
    uint32_t from = begin;
    uint32_t k = orderBegin;
    while (k < orderEnd) {
        const ShareOrder& o = orders[k];
        if (o.begin < from || o.split < o.begin || o.end < o.split || o.end > end ||
            o.splitOrder <= k || o.nested < o.splitOrder || o.nested > orderEnd)
            return false;
        if (!ordersNest(orders, o.begin, o.split, k + 1, o.splitOrder, depth + 1) ||
            !ordersNest(orders, o.split, o.end, o.splitOrder, o.nested, depth + 1))
            return false;
        from = o.end;
        k = o.nested;
    }
    return true;
}

// This eye's list from the program, with the header `fresh` was begun with. A list to draw holds
// the program's draws and this eye's by index (drawOrder, report 105 section 4.29); unless
// `whole`, the draws and materials are left out when the car renderer holds the program's rows
// (section 4.27), and a whole one with rows held is marked for the renderer to check them
// against. A list for a verify walk (`verify`) holds this eye's draws themselves, as a walk's.
// The drawOrder comes from the program's memo when these condition bits made it before (section
// 4.29.1); a verify walk's list is always emitted afresh.
void assemble(Program& p, const baked::CarDrawList& fresh, baked::CarDrawList& out, bool whole,
              bool verify) {
    if (&out != &fresh) {
        out.clear();
        out.driver = fresh.driver;
        out.cockpit = fresh.cockpit;
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
    OrderMemo* memo = nullptr;
    if (!verify)
        for (OrderMemo& m : p.orders)
            if (m.key && m.on == g_on) memo = &m;
    if (memo) {
        ++g_second.ordersMemoised;
        out.drawOrder = memo->order;
    } else {
        g_swapped = 0;
        if (g_orderScratch.size() < p.drawConditions.size() + 1)
            g_orderScratch.resize(p.drawConditions.size() + 1);
        uint32_t* first = g_orderScratch.data();
        uint32_t* last = emitRange(p, 0, static_cast<uint32_t>(p.list.draws.size()), 0,
                                   static_cast<uint32_t>(p.tape.orders.size()), first);
        out.drawOrder.assign(first, last);
        if (!verify) {
            memo = &p.orders[0];
            for (OrderMemo& m : p.orders)
                if (m.lastUsed < memo->lastUsed) memo = &m;
            memo->on = g_on;
            memo->order = out.drawOrder;
            memo->swapped = g_swapped;
            if (++g_orderKeys == 0) ++g_orderKeys;
            memo->key = g_orderKeys;
            uint32_t polygons = 0;
            uint32_t dynamicLeaves = 0;
            const std::vector<uint32_t>& packed = p.drawPolygons;
            for (uint32_t index : out.drawOrder) {
                uint32_t entry = index < packed.size() ? packed[index] : 0u;
                uint32_t count = entry & ~kDynamicDraw;
                polygons += count;
                dynamicLeaves += entry & kDynamicDraw ? count : 0u;
            }
            memo->polygons = polygons;
            memo->dynamicLeaves = dynamicLeaves;
        }
    }
    if (memo) {
        memo->lastUsed = g_reuses;
        g_swapped = memo->swapped;
    }
    uint32_t heldDraws = 0;
    bool     held =
        !verify && bakedCarRowsPinned(p.serial, static_cast<uint32_t>(frames), heldDraws);
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
    LARGE_INTEGER skinStart, skinEnd;
    QueryPerformanceCounter(&skinStart);
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
    QueryPerformanceCounter(&skinEnd);
    g_skinTicks = skinEnd.QuadPart - skinStart.QuadPart;
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

// Whether every skinned leaf's recipe names corners the scene and the list hold.
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
// side's subtree); a run is recorded at its first index only where the indices are contiguous,
// and the others are counted in `refused`. Two conditions cannot share a first index: one would
// be the other's ancestor and have that other as a descendant at a lower index. A tape with a
// parent at or after its child gets no runs; the replay refuses it.
void compileSkips(Program& p, uint32_t& runs, uint32_t& refused) {
    const std::vector<ShareCondition>& conditions = p.tape.conditions;
    size_t n = conditions.size();
    p.skipOwner.assign(n, 0);
    p.skipEnd.assign(n, 0);
    runs = 0;
    refused = 0;
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
        if (last[id] - start + 1 != count[id] || p.skipOwner[start]) {
            ++refused;
            continue;
        }
        p.skipOwner[start] = static_cast<uint16_t>(id);
        p.skipEnd[start] = last[id] + 1;
        ++runs;
    }
}

size_t vectorBytes(const Program& p);

// -- the program table ---------------------------------------------------------------------------

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

// A slot for a new program of `car`: the car's least recent one when it holds configsPerCar(),
// else a free slot, else the program of any car taken least recently. -1 when there is none.
int programSlot(Car& car, Second& t) {
    if (car.count >= configsPerCar()) {
        int index = car.programs[car.count - 1];
        dropProgram(index);
        ++t.droppedLimit;
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
    ++t.droppedPool;
    return index;
}

// Which build to ask for, or why not.
ProgramOutcome askBuild(Car& car, int why, Second& t) {
    if (g_pass < car.buildAfter) {
        ++t.walked[kWaiting];
        return kProgramWalk;
    }
    if (g_pass >= car.everyGateAfter)
        car.nextMargin = 0.0f;
    else if (g_pass >= car.marginAfter)
        car.nextMargin = kProgramMargin;
    else {
        ++t.walked[kBackedOff];
        return kProgramWalk;
    }
    // A car rebuilt on nearly every pass costs more than the walk its programs save.
    if (why != kNoProgram && g_pass - car.built <= kChurnPasses) {
        if (++car.churn >= kChurnLimit) {
            car.churn = 0;
            car.buildAfter = g_pass + kChurnBackoff;
            ++t.walked[kChurned];
            return kProgramWalk;
        }
    } else if (why != kNoProgram) {
        car.churn = 0;
    }
    int slot = programSlot(car, t);
    if (slot < 0) {
        ++t.walked[kNoSlot];
        return kProgramWalk;
    }
    Program& p = g_programs[static_cast<size_t>(slot)];
    p.car = carIndex(car);
    p.valid = false;
    ++t.rebuilt[why];
    ++t.builds;
    if (g_mirror) ++t.mirrorBuilds;
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
           p.tests.capacity() * sizeof(PlaneTest) +
           p.conditionTest.capacity() * sizeof(uint32_t) +
           p.drawConditions.capacity() * sizeof(uint16_t) +
           p.drawPolygons.capacity() * sizeof(uint32_t) +
           p.skipOwner.capacity() * sizeof(uint16_t) + p.skipEnd.capacity() * sizeof(uint32_t);
}

// The per-configuration cache (report 105 section 4.24), once a second after the cost line.
void logConfigurations(const Second& t, double us) {
    uint32_t held = 0, cars = 0, most = 0;
    for (const Car& car : g_cars) {
        if (!car.record || !car.count) continue;
        ++cars;
        held += car.count;
        most = std::max(most, car.count);
    }
    ConfigCounts c = carConfigCounts();
    double perReuse = t.reuses ? static_cast<double>(t.reuses) : 1.0;
    double perCall = t.calls ? static_cast<double>(t.calls) : 1.0;
    double perKept = t.keptEvery + t.keptMargin ? static_cast<double>(t.keptEvery + t.keptMargin)
                                                : 1.0;
    double replay = us * static_cast<double>(t.replayTicks) / perReuse;
    double skin = us * static_cast<double>(t.skinTicks) / perReuse;
    double total = us * static_cast<double>(t.reuseTicks) / perReuse;
    logf("baked cars: configurations (bakedCarConfigs %d) -- programs held %u for %u cars (%.1f a "
         "car, most %u of %u), %.0f of %zu KiB; reuses taken from another program of the car than "
         "its last %u, replays %.2f a first call; dropped: unused %u, a car's limit %u, the pool "
         "of %zu %u, the budget %u, "
         "inputs or frames moved %u; configuration gates and boxes: %u scanned, %u with a "
         "register write below (%u control nodes memoised in %u KiB), %u learned from hazards (+%u "
         "this second; hazards naming none: memo %u, registers %u, order %u, conditions full %u, "
         "too big %u), decided a kept build %.1f gates and %.2f boxes; left both ways unanswered "
         "%u (scans faulted %u, too deep %u, memo full %u), learned set full %u; a reuse %.1f us "
         "= replay %.1f + draws %.1f + skinned corners %.1f",
         g_options.bakedCarConfigs, held, cars, cars ? static_cast<double>(held) / cars : 0.0, most,
         configsPerCar(), static_cast<double>(g_bytes) / 1024.0, kBudgetBytes >> 10, t.switched,
         static_cast<double>(t.replays) / perCall, t.evicted, t.droppedLimit, kMaxPrograms,
         t.droppedPool, t.droppedBudget, t.droppedStale, c.scanned, c.writers, c.nodes,
         c.tableKiB, c.learned, c.learnedNow, t.unlearned[kUnionMemo],
         t.unlearned[kUnionRegister], t.unlearned[kUnionOrder], t.unlearned[kUnionConditions],
         t.unlearned[kUnionTooBig], t.keptConfigGates / perKept, t.keptConfigBoxes / perKept,
         c.unanswered, c.scanFaults, c.scanDeep, c.scanFull, c.learnFull, total, replay,
         std::max(0.0, total - replay - skin), skin);
    if (t.reuses || t.planeChecks)
        logf("baked cars: replay checks -- plane tests compared with the model in %u replays, a "
             "plane moved %u (must be 0), builds not kept for a plane that faulted %u (report 105 "
             "section 4.28); condition skips (section 4.37): %.1f runs a kept build, runs not "
             "contiguous %u (must be 0), conditions evaluated again without skips in %u replays, "
             "differing %u (must be 0); elided draws: %u of %u reuses (section 4.27), whole for "
             "the renderer's check %u; draw orders from the program's memo %u (section 4.29.1)",
             t.planeChecks, t.planesMoved, t.planeFaults,
             static_cast<double>(t.keptSkipRuns) / perKept, t.skipRunsRefused, t.skipChecks,
             t.skipsDiffered, t.elided, t.reuses, t.wholeChecks, t.ordersMemoised);
    carConfigSecond();
}

}  // namespace

void carProgramInstall() {
    if (g_installed) return;
    g_installed = true;
    if (!g_options.bakedCarProgram) {
        logf("baked cars: lists kept across frames OFF (bakedCarProgram = 0) -- the first eye walks "
             "every car every frame, as in report 105 section 4.22");
        return;
    }
    logf("baked cars: lists kept across frames ON (bakedCarProgram = 1, report 105 section 4.23) "
         "-- in every baked main pass a car's list is walked once with every gate both ways (or, "
         "where its memo cannot be followed so, every gate within %.2f m of the eye and every gate "
         "in the wheels), then re-posed and assembled for each eye and frame until its LOD, a "
         "register, a child pointer, the helmet colour, the pass's inputs or a decided gate "
         "changes; one reuse in %u is walked anyway and compared run by run; up to %zu cars, "
         "dropped after %u main passes unused; debug view 18 turns it off. Watch `baked cars: "
         "lists kept across frames` (MISMATCH 0)",
         kProgramMargin, kProgramVerifyEvery, kMaxCars, kEvictPasses);
    if (!g_options.bakedCarConfigs)
        logf("baked cars: configurations OFF (bakedCarConfigs = 0) -- one kept list a car, every "
             "gate both ways, as in report 105 section 4.23");
    else
        logf("baked cars: configurations ON (bakedCarConfigs = 1, report 105 section 4.24) -- a "
             "kept list's walk decides for its eye every gate with a register write below it and "
             "every gate a hazard of an earlier build named, so a car keeps up to %u lists, one a "
             "configuration of those gates and LOD, and each call takes the first whose decisions "
             "hold for its eye; at most %zu lists and %zu KiB for all cars. Watch `baked cars: "
             "configurations`",
             kConfigsPerCar, kMaxPrograms, kBudgetBytes >> 10);
}

void carProgramPassBegin(bool mirror, bool enabled) {
    Second& t = g_second;
    g_building = -1;
    g_verifyPending = false;
    g_active = false;
    g_mirror = mirror;
    if (!enabled) return;
    if (!g_options.bakedCarProgram || bakedPassCarsUnkept() ||
        (mirror && !g_options.bakedCarProgramMirrors)) {
        if (mirror)
            ++t.mirrorPassesOff;
        else
            ++t.passesOff;
        return;
    }
    g_active = true;
    // The passes a program's age and a car's retries count are the main passes; a mirror pass
    // takes and builds programs only (report 105 section 4.42).
    if (mirror) {
        ++t.mirrorPasses;
        return;
    }
    ++g_pass;
    ++t.passes;
    for (size_t i = 0; i < g_programs.size(); ++i) {
        Program& p = g_programs[i];
        if (p.car < 0 || g_pass - p.lastTaken <= kEvictPasses) continue;
        dropProgram(static_cast<int>(i));
        ++t.evicted;
    }
    for (Car& car : g_cars)
        if (car.record && !car.count && g_pass - car.lastTaken > kEvictPasses) car = Car{};
}

ProgramOutcome carProgramTake(const ShareCall& call, baked::CarDrawList& list) {
    Second& t = g_second;
    g_building = -1;
    g_verifyPending = false;
    if (!g_active) {
        if (!g_mirror) ++t.callsOff;
        return kProgramWalk;
    }
    ++t.calls;
    if (g_mirror) ++t.mirrorCalls;
    Car* car = findCar(call.record, g_mirror);
    if (!car) car = allocateCar(call.record, call.handle, g_mirror);
    if (!car) {
        ++t.walked[kNoSlot];
        return kProgramWalk;
    }
    car->lastTaken = g_pass;
    car->handle = call.handle;
    if (!car->count) return askBuild(*car, kNoProgram, t);
    // The car's programs, most recently taken first: the first whose inputs, reads and decisions
    // all hold for this call is taken. One that cannot hold again (the pass's inputs or the bake
    // moved, its tape names frames it does not hold, its replay faulted) is dropped.
    LARGE_INTEGER start, replayed, end;
    QueryPerformanceCounter(&start);
    int      why = -1;
    Program* p = nullptr;
    uint32_t position = 0;
    while (position < car->count) {
        int      index = car->programs[position];
        Program& candidate = g_programs[static_cast<size_t>(index)];
        int      reason = kInputs;
        Verdict  verdict = kRebuildIt;
        if (sameInputs(candidate, call, list)) {
            ++t.replays;
            verdict = replayGuarded(candidate, call, t, reason);
        }
        if (verdict == kWalkIt) {
            ++t.walked[reason];
            return kProgramWalk;
        }
        if (verdict == kAgrees) {
            p = &candidate;
            break;
        }
        if (why < 0) why = reason;
        if (reason == kInputs || reason == kFrames || reason == kFault) {
            dropProgram(index);
            ++t.droppedStale;
            continue;
        }
        ++position;
    }
    if (!p) return askBuild(*car, why < 0 ? kNoProgram : why, t);
    if (position > 0) {
        int index = car->programs[position];
        for (uint32_t i = position; i > 0; --i) car->programs[i] = car->programs[i - 1];
        car->programs[0] = static_cast<int16_t>(index);
        ++t.switched;
    }
    p->lastTaken = g_pass;
    QueryPerformanceCounter(&replayed);
    bool verify = ++g_reuses % kProgramVerifyEvery == 0;
    // A verify walk compares the whole assembly; one reuse in kWholeEvery comes whole for the car
    // renderer to check the rows it holds.
    assemble(*p, list, verify ? g_verifyList : list, verify || g_reuses % kWholeEvery == 0,
             verify);
    QueryPerformanceCounter(&end);
    t.reuseTicks += end.QuadPart - start.QuadPart;
    t.replayTicks += replayed.QuadPart - start.QuadPart;
    t.skinTicks += g_skinTicks;
    ++t.reuses;
    baked::CarDrawList& drawn = verify ? g_verifyList : list;
    t.drawnDraws += drawn.contentKey ? drawn.drawOrder.size() : drawn.draws.size();
    if (drawn.drawsElided) ++t.elided;
    if (drawn.contentCheck) ++t.wholeChecks;
    t.programDraws += p->list.draws.size();
    t.swapped += g_swapped;
    if (verify) {
        ++t.verifies;
        if (g_mirror) ++t.mirrorVerifies;
        g_verifyPending = true;
        return kProgramVerify;
    }
    ++t.reused;
    if (g_mirror) ++t.mirrorReused;
    return kProgramReused;
}

ShareTape* carProgramTapeBegin(const ShareCall& call, float& margin) {
    if (g_building < 0 || static_cast<size_t>(g_building) >= g_programs.size() ||
        g_programs[static_cast<size_t>(g_building)].car < 0 ||
        static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car) >= g_cars.size() ||
        g_cars[static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car)].record !=
            call.record) {
        ++g_second.lost;
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
    tape.wheelsSwitched = 0;
    tape.wheelsAsPosed = 0;
    std::memcpy(tape.rootEye, call.eye, sizeof(tape.rootEye));
    p.valid = false;
    return &tape;
}

bool carProgramTapeEnd(ShareTape& tape, int hazard, ShareRefusal refusal, baked::CarDrawList& list,
                       int64_t ticks, uint32_t learned, const uint32_t* unlearned) {
    Second& t = g_second;
    if (g_building < 0 || static_cast<size_t>(g_building) >= g_programs.size() ||
        &g_programs[static_cast<size_t>(g_building)].tape != &tape ||
        g_programs[static_cast<size_t>(g_building)].car < 0 ||
        static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car) >= g_cars.size()) {
        ++t.lost;
        g_building = -1;
        return false;
    }
    int      index = g_building;
    Program& p = g_programs[static_cast<size_t>(index)];
    Car&     car = g_cars[static_cast<size_t>(p.car)];
    g_building = -1;
    t.buildTicks += ticks;
    t.learnedGates += learned;
    for (int reason = 0; reason < kUnionRedoneCount; ++reason)
        t.unlearned[reason] += unlearned[reason];
    if (tape.refusal == kShareOk) tape.refusal = refusal;
    bool frames = tape.refusal == kShareFrames || list.frames.size() != tape.frames.size() + 1;
    if (hazard >= 0 || frames) {
        if (hazard >= 0 && hazard < kUnionRedoneCount)
            ++t.failed[hazard];
        else
            ++t.failedFrames;
        // Gates the hazards named are decided from the next build on (CarConfig.h): try again
        // then, at the same margin. Otherwise what the union cannot follow is a fact of the model
        // at this selection: step down.
        if (learned && !frames && ++car.learning <= kLearnRetries) {
            ++t.failedLearned;
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
        !skinsValid(tape, list)) {
        ++t.failedOther;
        car.buildAfter = g_pass + kRefusedRetry;
        dropProgram(index);
        return false;
    }
    if (!compileTests(p)) {
        ++t.planeFaults;
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
    uint32_t skipRuns = 0, skipRefused = 0;
    compileSkips(p, skipRuns, skipRefused);
    t.keptSkipRuns += skipRuns;
    t.skipRunsRefused += skipRefused;
    p.margin = car.nextMargin;
    p.valid = true;
    if (++g_serial == 0) g_serial = 1;
    p.serial = g_serial;
    for (OrderMemo& m : p.orders) m = OrderMemo{};
    p.built = g_pass;
    p.lastTaken = g_pass;
    p.configGates = 0;
    for (const ShareGate& g : tape.gates)
        if (g.config) ++p.configGates;
    for (const ShareBox& b : tape.boxes)
        if (b.config) ++t.keptConfigBoxes;
    p.bytes = vectorBytes(p);
    g_bytes += p.bytes;
    car.built = g_pass;
    car.learning = 0;
    uint32_t count = std::min(car.count, configsPerCar() - 1);
    for (uint32_t i = count; i > 0; --i) car.programs[i] = car.programs[i - 1];
    car.programs[0] = static_cast<int16_t>(index);
    car.count = count + 1;
    while (g_bytes > kBudgetBytes) {
        int oldest = leastRecent(index);
        if (oldest < 0) break;
        dropProgram(oldest);
        ++t.droppedBudget;
    }
    if (p.margin <= 0.0f)
        ++t.keptEvery;
    else
        ++t.keptMargin;
    t.keptLeaves += list.polygons;
    t.keptDraws += list.draws.size();
    t.keptConditions += tape.conditions.size();
    for (const ShareCondition& c : tape.conditions)
        if (c.kind == kConditionBox) ++t.keptBoxes;
    t.keptOrders += tape.orders.size();
    t.keptGates += tape.gates.size();
    t.keptConfigGates += p.configGates;
    t.keptSkins += tape.skins.size();
    t.keptWheelsSwitched += tape.wheelsSwitched;
    t.keptWheelsAsPosed += tape.wheelsAsPosed;
    // The walking eye draws the sides it is on and its own order.
    list.conditionOn.assign(tape.conditions.size() + 1, 0);
    list.conditionOn[0] = 1;
    for (size_t k = 0; k < tape.conditions.size(); ++k)
        list.conditionOn[k + 1] = tape.conditions[k].holds ? 1 : 0;
    return true;
}

void carProgramVerify(const baked::CarDrawList& walked, bool walkedWhole, int64_t ticks) {
    Second& t = g_second;
    if (!g_verifyPending) {
        ++t.lost;
        return;
    }
    g_verifyPending = false;
    if (!walkedWhole) {
        ++t.verifyFaulted;
        return;
    }
    t.plainTicks += ticks;
    ++t.plainWalks;
    char why[sizeof(t.firstMismatch)];
    if (carUnionSameDrawn(g_verifyList, walked, why, sizeof(why))) {
        ++t.verified;
        return;
    }
    if (!t.mismatched) std::memcpy(t.firstMismatch, why, sizeof(why));
    ++t.mismatched;
}

void carProgramSecond() {
    Second& t = g_second;
    if (t.passes || t.passesOff || t.mirrorPasses) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        double   us = 1e6 / static_cast<double>(frequency.QuadPart);
        uint32_t asked = 0, walked = 0, kept = t.keptEvery + t.keptMargin;
        for (uint32_t n : t.rebuilt) asked += n;
        for (uint32_t n : t.walked) walked += n;
        double reuse = t.reuses ? us * static_cast<double>(t.reuseTicks) / t.reuses : 0.0;
        double build = t.builds ? us * static_cast<double>(t.buildTicks) / t.builds : 0.0;
        double plain = t.plainWalks ? us * static_cast<double>(t.plainTicks) / t.plainWalks : 0.0;
        // The plain walks the reuses stood in for, less their own cost, the assembly a verify
        // adds to its walk and what a build costs beyond a plain walk.
        double saved = t.plainWalks
                           ? (static_cast<double>(t.reused) * (plain - reuse) -
                              static_cast<double>(t.verifies) * reuse -
                              static_cast<double>(t.builds) * std::max(0.0, build - plain)) /
                                 1000.0
                           : 0.0;
        logf("baked cars: lists kept across frames -- %u first calls (the mirror passes' with them, "
             "below) in %u main passes (%u calls "
             "in %u passes with it off: bakedCarProgram %d, debug view 18): REUSED %u, verified "
             "%u of %u, MISMATCH %u (must be 0), verify walks faulted %u; builds %u (no kept list "
             "%u, pass inputs moved %u, registers moved %u, a child pointer moved %u, helmet "
             "colour moved %u, LOD changed %u, selector camera moved %u, a decided gate turned "
             "%u, a configuration gate turned with no program of the car on this side %u, frames "
             "%u, replay faulted %u): kept %u with every gate, %u with the %.2f m margin; not "
             "kept: memo %u, registers %u, order %u, conditions full %u, too big %u, frames %u, a "
             "fault or refused leaf %u (of the hazard builds, %u taught configuration gates: tried "
             "again next pass); walked as before %u (GPL's vertex memo holds a vertex %u, too "
             "full to scan %u (%.0f entries a scan), not placed %u, backed off after builds not "
             "kept %u, waiting after a refused or teaching build %u, rebuilt every pass %u, past "
             "%zu cars or no program slot %u); GPL's memo scans whose slots in a program's address "
             "range were none of its vertices (section 4.43) %u; hand-overs lost %u (must be 0); "
             "of these, in mirror "
             "passes (report 105 section 4.42, bakedCarProgramMirrors %d): passes %u (off %u), "
             "first calls %u, REUSED %u, verified walks %u, builds %u",
             t.calls, t.passes, t.callsOff, t.passesOff, g_options.bakedCarProgram, t.reused,
             t.verified, t.verifies, t.mismatched, t.verifyFaulted, t.builds,
             t.rebuilt[kNoProgram], t.rebuilt[kInputs], t.rebuilt[kRegisters], t.rebuilt[kChild],
             t.rebuilt[kSprite], t.rebuilt[kLod], t.rebuilt[kCamera], t.rebuilt[kGate],
             t.rebuilt[kConfig], t.rebuilt[kFrames], t.rebuilt[kFault], t.keptEvery,
             t.keptMargin, kProgramMargin, t.failed[kUnionMemo], t.failed[kUnionRegister],
             t.failed[kUnionOrder], t.failed[kUnionConditions], t.failed[kUnionTooBig],
             t.failedFrames, t.failedOther, t.failedLearned, walked, t.walked[kGplMemo],
             t.walked[kPoolFull],
             t.poolScans ? static_cast<double>(t.poolEntries) / t.poolScans : 0.0,
             t.walked[kNoPool], t.walked[kBackedOff], t.walked[kWaiting], t.walked[kChurned],
             kMaxCars, t.walked[kNoSlot], t.poolNarrowed, t.lost, g_options.bakedCarProgramMirrors,
             t.mirrorPasses, t.mirrorPassesOff, t.mirrorCalls, t.mirrorReused, t.mirrorVerifies,
             t.mirrorBuilds);
        uint32_t cached = 0, every = 0;
        size_t   bytes = 0;
        for (const Program& p : g_programs) {
            bytes += vectorBytes(p);
            if (!p.valid) continue;
            ++cached;
            if (p.margin <= 0.0f) ++every;
        }
        double perKept = kept ? static_cast<double>(kept) : 1.0;
        double perReuse = t.reuses ? static_cast<double>(t.reuses) : 1.0;
        logf("baked cars: lists kept across frames, cost -- a reuse %.1f us (%u), a build %.1f us "
             "(%u), a plain walk %.1f us (%u verify walks); saved ~%.1f ms of CPU this second; "
             "programs held %u (every gate %u, margin %u), %.0f KiB, dropped unused %u; per kept "
             "build %.0f leaves, %.0f draws, %.0f conditions (%.1f boxes), %.0f order nodes, %.0f "
             "decided gates, %.1f skinned leaves, wheels with every model %.1f and as posed %.1f "
             "(section 4.30); per reuse %.0f of %.0f draws drawn, %.1f order nodes put the other "
             "way round from the build",
             reuse, t.reuses, build, t.builds, plain, t.plainWalks, saved, cached, every,
             cached - every, static_cast<double>(bytes) / 1024.0, t.evicted,
             t.keptLeaves / perKept, t.keptDraws / perKept, t.keptConditions / perKept,
             t.keptBoxes / perKept, t.keptOrders / perKept, t.keptGates / perKept,
             t.keptSkins / perKept, t.keptWheelsSwitched / perKept,
             t.keptWheelsAsPosed / perKept, t.drawnDraws / perReuse, t.programDraws / perReuse,
             t.swapped / perReuse);
        logConfigurations(t, us);
        if (t.mismatched)
            logf("baked cars: lists kept across frames, first MISMATCH this second -- %s",
                 t.firstMismatch);
    }
    g_second = Second{};
}

}  // namespace gplvr
