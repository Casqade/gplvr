#include "baked_gpl/CarProgram.h"

#include "Log.h"

#include "baked/CarScene.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarBuildWorker.h"
#include "baked_gpl/CarConfig.h"
#include "baked_gpl/CarOrder.h"
#include "baked_gpl/CarPassView.h"
#include "baked_gpl/CarUnion.h"
#include "baked_gpl/CarWalk.h"
#include "baked_gpl/ProgramArena.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gplvr {
namespace {

// CarWalk's arena holds 64 a pass; a car has a Car for the main passes and one per mirror glass
// (report 105 sections 4.42, 4.54): a field of 26 with the cockpit and three glasses needs 27 + 78.
constexpr size_t   kMaxCars = 256;
// Every car's configurations together (report 105 section 4.24). Their vectors lie in the program
// region (ProgramArena.h, section 4.59): a kept build takes a run of its pages, and while none is
// free the program taken least recently is dropped. The pool is above what the region holds of the
// 2020 grid's programs (~380 KiB each, section 4.55), so the region is the limit (section 4.54).
constexpr size_t   kMaxPrograms = 1024;
constexpr uint32_t kConfigsPerCar = 8;
// Where no region could be reserved, the vectors are the heap's and stay under kBudgetBytes, about
// 700 programs of the 2020 grid's: the mirror passes' lists by glass at a full-field start. While
// less than kFreeFloorBytes of address space is free, a kept build there only replaces what it
// displaces (budgetFor). The region is not reserved where it would leave less than the floor free.
constexpr size_t   kBudgetBytes = size_t{256} << 20;
constexpr uint64_t kFreeFloorBytes = uint64_t{384} << 20;
constexpr size_t   kMaxFrames = 257;       // the root and CarWalk's kMaxWalkFrames
constexpr int32_t  kMaxSelectorCases = 4096;
constexpr int32_t  kMaxPoolScan = 2048;    // as CarShare's
// Main passes. Programs are dropped only for the budget and the pool, least recently taken first:
// a car out of view keeps its programs for when it comes back. A Car holding no program is freed
// after kCarIdlePasses unused. After a build that could not be kept, the every-gate walk is tried
// again after kEveryGateRetry, the margin's after kMarginRetry, and a build refused for another
// reason (a fault, a refused leaf) after kRefusedRetry. A build whose hazards taught CarConfig new
// gates is tried again on the next pass, at most kLearnRetries times running. A car rebuilt within
// kChurnPasses of its last build kChurnLimit times running is walked as before for kChurnBackoff.
constexpr uint32_t kCarIdlePasses = 600;
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

// -- the pose cache (report 105 section 4.64 (b1), kCarPoseCache) --------------------------------

// poseMatrix reads the XFORM's words 1-6 and nothing else, and computes from them alone, so the
// matrix it made from six words is the matrix of every later call that finds the same six words,
// bit for bit. A replay re-makes every frame of its program (remakeFrames), and a call tries
// several programs of one car whose XFORMs GPL's pose has just written; the frame's other passes
// (the other eye, each glass) find the same words again while the car stands where the tick put
// it. The team's model is shared, so one node holds each team-mate's pose in turn: the key is the
// node and its six words. kPoseWays entries a set, the set by a hash of the key, the entry filled
// least recently replaced. One hit in kPoseCheckEvery is made again by poseMatrix and compared bit
// for bit; a difference turns the cache off until the car set changes. Emptied at a change of the
// car set (carBakeGeneration) and with every program, though no entry can go stale: its words are
// its key.
constexpr uint32_t kPoseSets = 1024;  // a power of two: the hash's top 10 bits
constexpr uint32_t kPoseWays = 4;
constexpr uint32_t kPoseCheckEvery = 64;

struct PoseEntry {
    uintptr_t node = 0;  // 0: empty
    uint32_t  words[6] = {};
    uint32_t  filled = 0;
    float     m[12] = {};
};

PoseEntry g_poses[kPoseSets * kPoseWays];  // 4,096 x 80 bytes
uint32_t  g_poseClock = 0;
uint32_t  g_poseGeneration = 0;
bool      g_poseOff = false;
uint32_t  g_poseLookups = 0;
uint32_t  g_poseHits = 0;
uint32_t  g_poseChecks = 0;
uint32_t  g_poseMismatches = 0;

void posesClear() {
    for (PoseEntry& e : g_poses) e.node = 0;
}

// poseMatrix's matrix of `node`, from the cache where its six words are an entry's.
void posedMatrix(uintptr_t node, float m[12]) {
    if constexpr (kCarPoseCache) {
        if (!g_poseOff) {
            uint32_t words[6];
            std::memcpy(words, reinterpret_cast<const void*>(node + 4), sizeof(words));
            uint32_t hash = static_cast<uint32_t>(node) * 0x9E3779B1u;
            for (uint32_t w : words) hash = (hash ^ w) * 0x9E3779B1u;
            PoseEntry* set = &g_poses[(hash >> 22) * kPoseWays];
            PoseEntry* victim = nullptr;
            ++g_poseLookups;
            for (uint32_t way = 0; way < kPoseWays; ++way) {
                PoseEntry& e = set[way];
                if (e.node == node && std::memcmp(e.words, words, sizeof(words)) == 0) {
                    if (++g_poseHits % kPoseCheckEvery != 0) {
                        std::memcpy(m, e.m, sizeof(e.m));
                        return;
                    }
                    ++g_poseChecks;
                    poseMatrix(node, m);
                    if (std::memcmp(m, e.m, sizeof(e.m)) == 0) return;
                    if (g_poseMismatches++ == 0)
                        logf("baked cars: the pose cache's matrix of XFORM %08X differs from "
                             "poseMatrix's (report 105 section 4.64) -- the cache is off until "
                             "the car set changes",
                             static_cast<unsigned>(node));
                    g_poseOff = true;
                    return;
                }
                if (!victim || (victim->node && (!e.node || e.filled < victim->filled)))
                    victim = &e;
            }
            poseMatrix(node, m);
            victim->node = node;
            std::memcpy(victim->words, words, sizeof(words));
            victim->filled = ++g_poseClock;
            std::memcpy(victim->m, m, sizeof(victim->m));
            return;
        }
    }
    poseMatrix(node, m);
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

// What a program keeps of its walk's list (baked::CarDrawList): its kinds, its frame count, and
// what an assembly copies.
struct KeptList {
    int    driver = 0;
    bool   cockpit = false;
    bool   overlay = false;
    bool   mirror = false;
    bool   flagman = false;
    size_t frames = 0;
    ArenaVector<baked::CarMaterial> materials;
    ArenaVector<baked::CarDraw>     draws;
    ArenaVector<baked::CarVertex>   dynamicVertices;
    ArenaVector<uint32_t>           dynamicIndices;
};

// One car's list for one configuration: the sides of the configuration gates its walk met, and
// one LOD (its kind-0 selections). Built in g_build, then copied into its slot: into one run of the
// program region (ProgramArena.h, report 105 section 4.59), or onto the heap where there is none.
struct Program {
    int32_t   car = -1;  // its g_cars index; -1: a free slot
    bool      valid = false;
    ShareTape tape;
    // The walk's list: every condition's draws, the order nodes' halves sealed.
    KeptList  list;
    float     margin = 0.0f;  // of the kept list; 0: every gate both ways
    uint32_t  lastTaken = 0;  // main passes
    uint32_t  built = 0;
    size_t    bytes = 0;         // its vectors, counted against kBudgetBytes on the heap
    uint32_t  serial = 0;        // this kept build's own: its assemblies' contentKey
    // The tape's side conditions as plane tests (report 105 section 4.28): each distinct
    // (plane, frame) once, and per condition its test (kNoTest for a box).
    ArenaVector<PlaneTest> tests;
    ArenaVector<uint32_t>  conditionTest;
    // Its assemblies' last drawOrders, by condition bits (on the heap, not counted in `bytes`), and
    // the order patch's index (counted; CarOrder.h, report 105 section 4.53).
    OrderState order;
    // list.draws[i].condition, packed for emitRange (report 105 section 4.30).
    ArenaVector<uint16_t> drawConditions;
    // list.draws[i].polygons, with kDynamicDraw where its object is kDynamicCarObject, packed for
    // an order memo's counts (report 105 section 4.38).
    ArenaVector<uint32_t> drawPolygons;
    // Report 105 section 4.37, per condition index: the condition whose descendants are the
    // conditions [index, skipEnd[index]) (0: none), which the replay passes over while it is off.
    ArenaVector<uint16_t> skipOwner;
    ArenaVector<uint32_t> skipEnd;
    // Its pages in the program region; none on the heap.
    ArenaRun  run;
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
    // Report 105 section 4.61: this Car's own number (a job names its Car by index and serial),
    // its job in flight on the worker (-1 none), and what the worker last could not do for it (a
    // CarDetachedNeed; its next build is the render thread's).
    uint32_t  serial = 0;
    int16_t   job = -1;
    int8_t    buildHere = kDetachedOk;
    // Section 4.63: the main pass its job was queued in, and while it is drawn from its previous
    // program, the main pass of the first such draw.
    uint32_t  jobPass = 0;
    bool      staleRun = false;
    uint32_t  staleFrom = 0;
    // Section 4.64 (a1): the program each stereo eye last took or built (-1 none); one no longer
    // among `programs` is passed over.
    int16_t   eyeLast[2] = {-1, -1};
};

std::vector<Program> g_programs;  // reserved to kMaxPrograms once: never reallocated
std::vector<Car>     g_cars;      // reserved to kMaxCars once
// The build under way: the walk records into its tape, and the keep compiles into it; all on the
// heap, its capacity kept from build to build.
Program              g_build;
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
uint32_t             g_carSerial = 0;  // the last Car::serial handed out

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
    if (++g_carSerial == 0) g_carSerial = 1;
    car->serial = g_carSerial;
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

// Whether every XFORM's child is the one the build walked. A wheel XFORM the build took every
// model of may point at any of them (report 105 section 4.30); any other child is a new one.
bool childrenHold(const ShareTape& tape) {
    for (const ShareFrame& f : tape.frames) {
        uint32_t child = word(f.node, 8);
        if (child == f.words[6]) continue;
        bool known = false;
        for (uint32_t k = f.switchFirst; k < f.switchFirst + f.switchCount; ++k)
            known = known || (k >= 1 && k <= tape.conditions.size() &&
                              tape.conditions[k - 1].plane == child);
        if (!known) return false;
    }
    return true;
}

// The frames re-made from the XFORM words GPL's pose wrote for this car call, into g_clips,
// g_eyes, g_roots and g_half; then the sizes of the per-condition arrays checked.
Verdict remakeFrames(const Program& p, const ShareCall& call, int& reason) {
    const ShareTape& tape = p.tape;
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
        posedMatrix(f.node, pose);
        enterPose(g_clips[f.parent], g_eyes[f.parent], pose, g_clips[k + 1], g_eyes[k + 1]);
        enterRoot(g_roots[f.parent], pose, g_roots[k + 1]);
        // GPL rebuilds the half vector only where a rotation ran (enterFrame).
        bool rotates = wordFloat(f.node, 4) != 0.0f || wordFloat(f.node, 5) != 0.0f ||
                       wordFloat(f.node, 6) != 0.0f;
        g_half[k + 1] = rotates ? static_cast<uint16_t>(k + 1) : g_half[f.parent];
    }
    if (p.conditionTest.size() != tape.conditions.size() ||
        p.skipOwner.size() != tape.conditions.size() ||
        p.skipEnd.size() != tape.conditions.size()) {
        reason = kFrames;
        return kRebuildIt;
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
    if (frames > kMaxFrames || p.list.frames != frames) {
        reason = kFrames;
        return kRebuildIt;
    }
    if (!childrenHold(tape)) {
        reason = kChild;
        return kRebuildIt;
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
    Verdict remade = remakeFrames(p, call, reason);
    if (remade != kAgrees) return remade;
    // The plane tests hold the planes as the build read them; now and then they are compared
    // with the model, and a plane that moved drops the program.
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

// A stale take's replay (report 105 section 4.63): the frames and conditions of `p` for this call,
// made as a reuse makes them, with its child pointers and GPL's vertex memo checked as a reuse
// checks them. The registers, sprite colours and decisions keep the build's values: the program's
// draws are those of its build's selection.
Verdict staleReplay(const Program& p, const ShareCall& call, int& reason) {
    size_t frames = p.tape.frames.size() + 1;
    if (frames > kMaxFrames || p.list.frames != frames) {
        reason = kFrames;
        return kRebuildIt;
    }
    if (!childrenHold(p.tape)) {
        reason = kChild;
        return kRebuildIt;
    }
    Verdict pool = scanPool(p.tape, call, reason);
    if (pool != kAgrees) return pool;
    Verdict remade = remakeFrames(p, call, reason);
    if (remade != kAgrees) return remade;
    return evaluateConditions<true>(p, frames, g_on, g_side, reason);
}

Verdict staleReplayGuarded(const Program& p, const ShareCall& call, int& reason) {
    __try {
        return staleReplay(p, call, reason);
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
        out.materials.assign(p.list.materials.begin(), p.list.materials.end());
    } else {
        out.draws.assign(p.list.draws.begin(), p.list.draws.end());
        out.materials.assign(p.list.materials.begin(), p.list.materials.end());
    }
    out.dynamicIndices.assign(p.list.dynamicIndices.begin(), p.list.dynamicIndices.end());
    out.dynamicVertices.assign(p.list.dynamicVertices.begin(), p.list.dynamicVertices.end());
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
    const ArenaVector<ShareCondition>& conditions = p.tape.conditions;
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
    const ArenaVector<ShareCondition>& conditions = p.tape.conditions;
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

// -- the program pool ----------------------------------------------------------------------------

// Every vector a kept program copies from g_build (the list's come from the walk's list).
template <typename F>
void forEachKept(const Program& p, F&& f) {
    const ShareTape& t = p.tape;
    f(t.vertices);
    f(t.frames);
    f(t.gates);
    f(t.boxes);
    f(t.selectors);
    f(t.registers);
    f(t.sprites);
    f(t.conditions);
    f(t.orders);
    f(t.skins);
    f(t.skinFrames);
    f(t.glass);
    f(p.tests);
    f(p.conditionTest);
    f(p.drawConditions);
    f(p.drawPolygons);
    f(p.skipOwner);
    f(p.skipEnd);
    const OrderIndex& index = p.order.index;
    f(index.drawsFirst);
    f(index.draws);
    f(index.nodesFirst);
    f(index.nodes);
    f(index.drawNode);
    f(index.nodeParent);
}

// The vectors a program keeps of a list: a KeptList's, or the walk's baked::CarDrawList's.
template <typename List, typename F>
void forEachKeptOfList(const List& l, F&& f) {
    f(l.materials);
    f(l.draws);
    f(l.dynamicVertices);
    f(l.dynamicIndices);
}

// What a kept program's vectors hold, counted against kBudgetBytes on the heap.
size_t vectorBytes(const Program& p) {
    size_t bytes = 0;
    auto   add = [&bytes](const auto& v) {
        using Vector = std::decay_t<decltype(v)>;
        bytes += v.capacity() * sizeof(typename Vector::value_type);
    };
    forEachKept(p, add);
    forEachKeptOfList(p.list, add);
    return bytes;
}

// The bytes the copy of `build` and the walk's `list` takes in a run of the program region, block
// by block as ArenaAllocator packs them.
size_t keptBytes(const Program& build, const baked::CarDrawList& list) {
    size_t bytes = 0;
    auto   add = [&bytes](const auto& v) {
        using Vector = std::decay_t<decltype(v)>;
        bytes += programArenaBlockBytes(v.size() * sizeof(typename Vector::value_type));
    };
    forEachKept(build, add);
    forEachKeptOfList(list, add);
    return bytes;
}

// Takes program `index` out of its car's list and frees its vectors, then its run.
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
    // Its rows in the car renderer, CPU and GPU, go with it (report 105 section 4.66).
    if (p.serial) bakedCarRowsDropped(p.serial);
    ArenaRun run = p.run;
    p = Program{};
    programArenaGive(run);
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

// On the heap (no program region): the bytes the programs may hold once a kept build of `added`
// bytes is counted: kBudgetBytes, or from the first kept build that finds less than
// kFreeFloorBytes of address space free (or GlobalMemoryStatusEx failing) on, what they held
// before that build, at least kFloorCapMin: the heap keeps what dropped programs free, so new
// programs reuse it instead of taking address space, and free address space does not come back to
// lift the cap (report 105 section 4.55.1).
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

// A build's tape begun from its call, under the car bake it is asked under.
void tapeBegin(ShareTape& tape, const ShareCall& call, float margin, uint32_t generation,
               uint32_t rebaked) {
    tape.record = call.record;
    tape.root = call.root;
    tape.handle = call.handle;
    tape.driver = call.driver;
    tape.classVisible = call.classVisible;
    tape.textureList = call.textureList;
    tape.texturedMask = call.texturedMask;
    tape.lightingOn = call.lightingOn;
    tape.generation = generation;
    tape.rebaked = rebaked;
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
}

// -- builds on the worker (CarBuildWorker.h, report 105 section 4.61) ----------------------------

// Why a build stays on the render thread. kHereObject..kHereScan follow CarDetachedNeed's order.
enum BuiltHere : int {
    kHereOff,        // kCarBuildWorker is false
    kHereNoWorker,   // the worker did not start
    kHereCockpit,    // the cockpit's glass reads GPL's live UVs and the glass set (CockpitGlass.h)
    kHereObject,     // the worker met an object not baked yet; the render thread bakes it
    kHereTexture,    // a texture handle not resolved yet
    kHereScan,       // a gate or box CarConfig's scan had not answered
    kHereQueueFull,  // kCarBuildQueue jobs in flight
    kHereSnapshot,   // the posed nodes could not be read
    kHereReasons
};
static_assert(kHereTexture - kHereObject == kDetachedTexture - kDetachedObject &&
                  kHereScan - kHereObject == kDetachedScan - kDetachedObject,
              "a worker's need names its render-thread build");

// What the worker made of a job; at its keep the render thread adds kLandStale and kLandNoRoom.
// kJobObject..kJobScan follow CarDetachedNeed's order.
enum JobStatus : int {
    kJobBuilt,      // walked and compiled (kLandKept once kept)
    kJobHazard,     // a hazard, or a frame the tape could not place: retried as a build here is
    kJobRefused,    // the walk faulted, or its list, orders, skins or planes refused it
    kJobObject,
    kJobTexture,
    kJobScan,
    kJobCancelled,  // the epoch moved or the car bake was dropped, before or while it ran
    kJobAged,       // queued more than kJobMaxAge main passes ago: not built
    kJobThrew,      // an exception (an allocation) ended it
    kLandStale,     // landed, its Car gone or the car bake moved since it was asked
    kLandNoRoom,    // built, but no slot or run of the region took it
    kLandOutcomes
};
constexpr int kLandKept = kJobBuilt;
static_assert(kJobTexture - kJobObject == kDetachedTexture - kDetachedObject &&
                  kJobScan - kJobObject == kDetachedScan - kDetachedObject,
              "a worker's need names its job status");
// A job queued this many main passes before the worker reaches it is not built: the car has moved
// on (builds are predictable a few frames ahead, section 4.60).
constexpr uint32_t kJobMaxAge = 16;

// One build on the worker: the request, then the result in heap scratch of its own, its capacity
// kept from job to job. The render thread fills it and hands it over; the worker owns it from
// carBuildWorkerSubmit to carBuildWorkerTakeDone.
struct BuildJob {
    CarBuildSnapshot snapshot;
    int32_t   car = -1;
    uint32_t  carSerial = 0;
    uintptr_t record = 0;
    int       why = kNoProgram;
    int8_t    eye = -1;  // the stereo eye of the call that queued it
    uint32_t  pass = 0;  // g_pass when queued
    int       status = kJobThrew;
    bool      frames = false;  // of a kJobHazard
    uint32_t  learned = 0;     // configuration gates the walk's hazards named that were new
    std::vector<uintptr_t> learnedGates;  // learned at the keep (CarConfig is the render thread's)
    Program   build;
    baked::CarDrawList list;
};

std::vector<BuildJob> g_jobs;      // kCarBuildQueue, made at install: never reallocated
std::vector<uint16_t> g_freeJobs;  // render thread
bool                  g_worker = false;
std::atomic<uint32_t> g_passClock{0};  // g_pass, for the worker's kJobMaxAge
uint32_t              g_here[kHereReasons] = {};
uint32_t              g_landed[kLandOutcomes] = {};

// Queues a build of `car` for this call: -1, or the BuiltHere reason it is the render thread's.
int queueBuild(Car& car, int why, const ShareCall& call, const baked::CarDrawList& list) {
    if (g_freeJobs.empty()) return kHereQueueFull;
    uint16_t          slot = g_freeJobs.back();
    BuildJob&         job = g_jobs[slot];
    CarBuildSnapshot& s = job.snapshot;
    if (!call.registers || list.materials.empty()) return kHereSnapshot;
    if (!carWalkSnapshotPose(call.instance, s)) return kHereSnapshot;
    s.record = call.record;
    s.root = call.root;
    s.handle = call.handle;
    s.instance = call.instance;
    s.driver = call.driver;
    std::memcpy(s.clip, call.clip, sizeof(s.clip));
    std::memcpy(s.eye, call.eye, sizeof(s.eye));
    s.lodScale = call.lodScale;
    s.dlong = call.dlong;
    s.dlat = call.dlat;
    s.classVisible = call.classVisible;
    s.lightingOn = call.lightingOn;
    s.textureList = call.textureList;
    s.texturedMask = call.texturedMask;
    std::memcpy(s.registers, call.registers, sizeof(s.registers));
    std::memcpy(s.rootModelView, list.rootModelView, sizeof(s.rootModelView));
    std::memcpy(s.projection, list.projection, sizeof(s.projection));
    std::memcpy(s.light, list.light, sizeof(s.light));
    s.mirror = list.mirror;
    s.walkSlot = list.walkSlot;
    const baked::CarMaterial& material = list.materials[0];
    s.surfaceClass = material.surfaceClass;
    s.textured = material.textured;
    s.specularExponent = material.specularExponent;
    s.specular = material.specular;
    s.margin = car.nextMargin;
    s.generation = carBakeGeneration();
    s.rebaked = carBakeTotals().objectsRebaked;
    s.epoch = carBuildWorkerEpoch();
    job.car = carIndex(car);
    job.carSerial = car.serial;
    job.record = car.record;
    job.why = why;
    job.eye = static_cast<int8_t>(carPassView().eye);
    job.pass = g_pass;
    job.status = kJobThrew;
    if (!carBuildWorkerSubmit(slot)) return kHereQueueFull;
    g_freeJobs.pop_back();
    car.job = static_cast<int16_t>(slot);
    car.jobPass = g_pass;
    return -1;
}

// A build the render thread makes: counted, and each reason but the switch logged the first time.
void builtHere(int reason) {
    if (g_here[reason]++ || reason == kHereOff) return;
    static const char* const kWhy[kHereReasons] = {
        "",
        "the build worker did not start",
        "a cockpit's build reads GPL's live glass",
        "the worker met an object not baked yet (the render thread bakes it)",
        "the worker met a texture handle not resolved yet",
        "the worker met a gate or box CarConfig had not scanned",
        "the worker's queue was full",
        "the car's posed nodes could not be read for the worker"};
    logf("baked cars: a car program was built on the render thread -- %s (report 105 section "
         "4.61; logged once a reason)",
         kWhy[reason]);
}

// Which build to ask for, or why not. `call` and `list` (begun for it) are what a build on the
// worker is asked with.
ProgramOutcome askBuild(Car& car, int why, const ShareCall& call,
                        const baked::CarDrawList& list) {
    if (g_pass < car.buildAfter) return kProgramWalk;
    // The worker builds this car already; this call walks as before until the build lands.
    if (car.job >= 0) return kProgramWalk;
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
    // On the worker where it can be (report 105 section 4.61), else here as before.
    int here = !kCarBuildWorker ? kHereOff
               : !g_worker      ? kHereNoWorker
               : car.cockpit    ? kHereCockpit
               : car.buildHere  ? kHereObject + (car.buildHere - kDetachedObject)
                                : queueBuild(car, why, call, list);
    if (here < 0) return kProgramQueued;
    car.buildHere = kDetachedOk;
    builtHere(here);
    int slot = programSlot(car);
    if (slot < 0) return kProgramWalk;
    Program& p = g_programs[static_cast<size_t>(slot)];
    p.car = carIndex(car);
    p.valid = false;
    g_building = slot;
    return kProgramBuild;
}

// -- the program region (ProgramArena.h, report 105 section 4.59) -------------------------------

// Why a kept build got no run: it was not kept, and its car is walked as before.
enum RegionRefusal : int {
    kRegionTooBig,      // its run is longer than the region
    kRegionCommit,      // VirtualAlloc could not commit its run
    kRegionNoRun,       // no run free with every other program dropped
    kRegionRefusalCount
};

uint32_t g_regionRefused[kRegionRefusalCount] = {};
uint32_t g_regionDrops = 0;       // programs dropped for a run
uint32_t g_regionScattered = 0;   // of them, while the pages were free but not in one run
uint32_t g_regionSpilled = 0;     // kept builds that outgrew the run keptBytes asked for
bool     g_saidRegionFull = false;
bool     g_saidRegionScattered = false;
bool     g_saidRegionSpill = false;

void logRegionUse(const char* what) {
    ArenaUse use = programArenaUse();
    uint32_t held = 0;
    for (const Program& p : g_programs) held += p.valid ? 1u : 0u;
    logf("baked cars: %s -- %u programs hold %u of %u pages (%zu KiB each), %u committed, the "
         "largest free run %u pages; dropped for a run %u (pages free but scattered %u), refused "
         "too big %u / commit %u / no run %u, spilled to the heap %u (logged once)",
         what, held, use.used, use.pages, kArenaPageBytes >> 10, use.committed, use.largestFree,
         g_regionDrops, g_regionScattered, g_regionRefused[kRegionTooBig],
         g_regionRefused[kRegionCommit], g_regionRefused[kRegionNoRun], g_regionSpilled);
}

// A run of the program region for program `index` (not valid yet) to hold `bytes`: the programs
// taken least recently are dropped until one is free. False when none can be had.
bool regionRun(int index, size_t bytes, ArenaRun& run) {
    for (;;) {
        int taken = programArenaTake(bytes, run);
        if (taken == kArenaTaken) return true;
        int refusal = taken == kArenaTooBig        ? kRegionTooBig
                      : taken == kArenaCommitFailed ? kRegionCommit
                                                    : kRegionNoRun;
        int oldest = refusal == kRegionNoRun ? leastRecent(index) : -1;
        if (oldest < 0) {
            if (g_regionRefused[refusal]++ == 0) {
                static const char* const kWhy[kRegionRefusalCount] = {
                    "a kept list is longer than the program region",
                    "the program region could not commit a run",
                    "no run of the program region is free with every other program dropped"};
                logf("baked cars: %s (%zu KiB; the last commit error %lu) -- it is not kept and the "
                     "car is walked as before (logged once)",
                     kWhy[refusal], bytes >> 10, programArenaCommitError());
            }
            return false;
        }
        if (!g_saidRegionFull) {
            g_saidRegionFull = true;
            logRegionUse("the program region is full, the programs taken least recently are "
                         "dropped from now on");
        }
        ++g_regionDrops;
        if (taken == kArenaNoRun) {
            ++g_regionScattered;
            if (!g_saidRegionScattered) {
                g_saidRegionScattered = true;
                logRegionUse("a kept build found the region's free pages scattered and dropped a "
                             "program for a run");
            }
        }
        dropProgram(oldest);
    }
}

// Copies the build into program `p`: the build's vectors (g_build's, or a worker job's) and the
// walk's list, into `run` (none: the heap). The order memos start empty.
void keepBuild(Program& p, const Program& build, const baked::CarDrawList& list,
               const ArenaRun& run) {
    p.order = OrderState{};
    p.run = run;
    uint32_t spills = programArenaSpills();
    {
        ProgramArenaFill fill(run);
        p.tape = build.tape;
        p.list.materials.assign(list.materials.begin(), list.materials.end());
        p.list.draws.assign(list.draws.begin(), list.draws.end());
        p.list.dynamicVertices.assign(list.dynamicVertices.begin(), list.dynamicVertices.end());
        p.list.dynamicIndices.assign(list.dynamicIndices.begin(), list.dynamicIndices.end());
        p.tests = build.tests;
        p.conditionTest = build.conditionTest;
        p.drawConditions = build.drawConditions;
        p.drawPolygons = build.drawPolygons;
        p.skipOwner = build.skipOwner;
        p.skipEnd = build.skipEnd;
        p.order.index = build.order.index;
    }
    p.list.driver = list.driver;
    p.list.cockpit = list.cockpit;
    p.list.overlay = list.overlay;
    p.list.mirror = list.mirror;
    p.list.flagman = list.flagman;
    p.list.frames = list.frames.size();
    if (programArenaSpills() != spills) {
        ++g_regionSpilled;
        if (!g_saidRegionSpill) {
            g_saidRegionSpill = true;
            logRegionUse("a kept list outgrew the run its sizes asked for, the rest came from the "
                         "heap");
        }
    }
}

// -- a walked build: checked and compiled on either thread, kept on the render thread ------------

enum BuildVerdict { kBuildKeep, kBuildHazard, kBuildRefused };

// Whether `build`'s walk (its tape, `list`) can be kept: kBuildHazard for a hazard (`hazard`, the
// first UnionRedone, -1 none) or a frame the tape could not place (`frames`), kBuildRefused for
// what CarWalk's shareRefusal (`refusal`), the order nodes, the skins or the plane reads refuse.
// Reads the car bake and GPL's planes: on the worker, inside its gate.
BuildVerdict checkBuild(Program& build, int hazard, ShareRefusal refusal,
                        const baked::CarDrawList& list, bool& frames) {
    ShareTape& tape = build.tape;
    if (tape.refusal == kShareOk) tape.refusal = refusal;
    frames = tape.refusal == kShareFrames || list.frames.size() != tape.frames.size() + 1;
    if (hazard >= 0 || frames) return kBuildHazard;
    if (tape.refusal != kShareOk ||
        !ordersNest(tape.orders, 0, static_cast<uint32_t>(list.draws.size()), 0,
                    static_cast<uint32_t>(tape.orders.size()), 0) ||
        !skinsValid(tape, list) || !compileTests(build))
        return kBuildRefused;
    return kBuildKeep;
}

// What a kept build adds to its tape: its vertices sealed, the packed draw arrays, the skip runs
// and the order patch's index. Its own data only.
void compileBuild(Program& build, const baked::CarDrawList& list) {
    ShareTape& tape = build.tape;
    shareSealVertices(tape);
    build.drawConditions.resize(list.draws.size());
    build.drawPolygons.resize(list.draws.size());
    for (size_t i = 0; i < list.draws.size(); ++i) {
        const baked::CarDraw& draw = list.draws[i];
        build.drawConditions[i] = draw.condition;
        build.drawPolygons[i] = (draw.polygons & ~kDynamicDraw) |
                                (draw.object == baked::kDynamicCarObject ? kDynamicDraw : 0u);
    }
    compileSkips(build);
    // The order patch's index (report 105 section 4.53) over the draws and order nodes as kept;
    // ordersNest held in checkBuild. A refused index leaves the program's misses to whole
    // emissions.
    OrderSource source{tape.orders.data(),
                       static_cast<uint32_t>(tape.orders.size()),
                       build.drawConditions.data(),
                       static_cast<uint32_t>(build.drawConditions.size()),
                       build.drawPolygons.data(),
                       nullptr,
                       static_cast<uint32_t>(tape.conditions.size() + 1)};
    orderIndexBuild(build.order.index, source);
}

// After a build with a hazard: gates the hazards named (`learned` new ones) are decided from the
// next build on (CarConfig.h), so it is tried again then, at the same margin. Otherwise what the
// union cannot follow is a fact of the model at this selection: step down.
void afterHazard(Car& car, uint32_t learned, bool frames) {
    if (learned && !frames && ++car.learning <= kLearnRetries) {
        car.buildAfter = g_pass + 1;
        return;
    }
    car.learning = 0;
    if (car.nextMargin <= 0.0f)
        car.everyGateAfter = g_pass + kEveryGateRetry;
    else
        car.marginAfter = g_pass + kMarginRetry;
}

// Keeps a checked and compiled build in program slot `index` (its car set, not valid yet), first
// of `car`'s programs: a run of the program region, made free by dropping the programs taken least
// recently, or the heap under the byte budget where no region was reserved. False when no run
// could be had: the slot is dropped and the car retried later.
bool keepInto(int index, Car& car, const Program& build, const baked::CarDrawList& list,
              float margin) {
    Program& p = g_programs[static_cast<size_t>(index)];
    ArenaRun run;
    if (programArenaActive() && !regionRun(index, keptBytes(build, list), run)) {
        car.buildAfter = g_pass + kRefusedRetry;
        dropProgram(index);
        return false;
    }
    keepBuild(p, build, list, run);
    p.margin = margin;
    p.valid = true;
    if (++g_serial == 0) g_serial = 1;
    p.serial = g_serial;
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
    if (!programArenaActive()) {
        size_t budget = budgetFor(p.bytes);
        while (g_bytes > budget) {
            int oldest = leastRecent(index);
            if (oldest < 0) break;
            dropProgram(oldest);
        }
    }
    return true;
}

// The worker's job (CarBuildWorker.h): listOurs's build of the snapshot, checked and compiled.
// Inside the gate while it reads GPL or the car bake; the pure compile after it.
int buildJob(BuildJob& job) {
    const CarBuildSnapshot& s = job.snapshot;
    if (g_passClock.load(std::memory_order_relaxed) - job.pass > kJobMaxAge) return kJobAged;
    if (!carBuildEnter(s.epoch)) return kJobCancelled;
    if (carBakeGeneration() != s.generation) return kJobCancelled;
    ShareCall call;
    call.record = s.record;
    call.root = s.root;
    call.handle = s.handle;
    call.driver = s.driver;
    call.clip = s.clip;
    call.eye = s.eye;
    call.classVisible = s.classVisible;
    call.lightingOn = s.lightingOn;
    call.textureList = s.textureList;
    call.texturedMask = s.texturedMask;
    call.registers = s.registers;
    Program& build = job.build;
    tapeBegin(build.tape, call, s.margin, s.generation, s.rebaked);
    CarDetachedWalk walked;
    walked.learnedGates = &job.learnedGates;
    carWalkDetached(s, build.tape, job.list, walked);
    job.learned = walked.learned;
    if (walked.need == kDetachedCancelled) return kJobCancelled;
    if (walked.need != kDetachedOk) return kJobObject + (walked.need - kDetachedObject);
    BuildVerdict verdict =
        checkBuild(build, walked.walked ? walked.hazard : -1, walked.refusal, job.list, job.frames);
    carBuildLeave();
    if (verdict == kBuildHazard) return kJobHazard;
    if (verdict == kBuildRefused) return kJobRefused;
    compileBuild(build, job.list);
    return kJobBuilt;
}

void runJob(uint32_t slot) {
    BuildJob& job = g_jobs[slot];
    job.status = kJobThrew;
    job.frames = false;
    job.learned = 0;
    job.learnedGates.clear();
    try {
        job.status = buildJob(job);
    } catch (...) {
        job.status = kJobThrew;
    }
    carBuildLeave();
}

// -- drawn from the previous program while the build is on the worker (report 105 section 4.63) -

// Main passes after its job was queued from which a car walks as before until the job lands (the
// worker drops a job it reaches after kJobMaxAge anyway).
constexpr uint32_t kStaleMaxPasses = 16;

// Why a call whose build is on the worker walks as before after all.
enum StaleRefusal : int {
    kStaleNoProgram,  // no program of the car is left
    kStaleDropped,    // this call dropped a program of the car (inputs, bake, frames, a fault)
    kStaleNever,      // its most recent program differs where a stale draw is not safe (a child)
    kStalePast,       // its job was queued more than kStaleMaxPasses main passes ago
    kStaleRemake,     // that program's frames or conditions could not be made (GPL's memo, a fault)
    kStaleRefusals
};

// Main passes from a car's first stale draw to its job's landing: 0, 1, 2-3, 4-7, 8-16, more.
constexpr int kStaleSpans = 6;

uint32_t g_stale[kRebuildCount] = {};
uint32_t g_staleRefused[kStaleRefusals] = {};
uint32_t g_staleSpans[kStaleSpans] = {};
uint32_t g_staleTotal = 0;
uint32_t g_staleLogged = 0;         // g_staleTotal at the last totals line
uint32_t g_staleNextLog = 1000;     // the next tenfold of g_staleTotal that logs the totals
uint32_t g_staleGeneration = 0;     // the car set the totals were last looked at in

const char* const kRebuildName[kRebuildCount] = {
    "no program", "inputs", "a register", "a child pointer", "the helmet colour", "the LOD",
    "the selector camera", "a decided gate", "frames", "a fault", "a configuration gate or box",
    "the cockpit glass"};
static_assert(kRebuildCount == 12, "kRebuildName names every Rebuild reason");

// The reasons whose difference only the picture shows: the program's draws are its build's
// selection, its frames and live conditions this call's (staleReplay).
bool staleSafe(int reason) {
    return reason == kRegisters || reason == kSprite || reason == kLod || reason == kCamera ||
           reason == kGate || reason == kConfig;
}

void logStaleTotals(const char* when) {
    g_staleLogged = g_staleTotal;
    logf("baked cars: stale draws %s -- %u: register %u, helmet colour %u, LOD %u, selector camera "
         "%u, decided gate %u, configuration %u; walked instead: no program %u, a program dropped "
         "%u, a child pointer %u, past %u main passes %u, frames not made %u; jobs landed 0 / 1 / "
         "2-3 / 4-7 / 8-16 / more main passes after the first stale draw: %u / %u / %u / %u / %u / "
         "%u (report 105 section 4.63)",
         when, g_staleTotal, g_stale[kRegisters], g_stale[kSprite], g_stale[kLod],
         g_stale[kCamera], g_stale[kGate], g_stale[kConfig], g_staleRefused[kStaleNoProgram],
         g_staleRefused[kStaleDropped], g_staleRefused[kStaleNever], kStaleMaxPasses,
         g_staleRefused[kStalePast], g_staleRefused[kStaleRemake], g_staleSpans[0],
         g_staleSpans[1], g_staleSpans[2], g_staleSpans[3], g_staleSpans[4], g_staleSpans[5]);
}

void staleRefused(int why) {
    if (g_staleRefused[why]++) return;
    static const char* const kWhy[kStaleRefusals] = {
        "no program of the car is left (a new car, or every program dropped)",
        "this call dropped a program of the car (its inputs or the bake moved, frames, a fault)",
        "its most recent program's child pointer moved (a tyre swap, a lost wheel)",
        "its build was queued too many main passes ago",
        "its most recent program's frames or conditions could not be made for this call (GPL's "
        "vertex memo, a fault)"};
    logf("baked cars: a car whose build is on the worker was walked as before -- %s (report 105 "
         "section 4.63; logged once a reason)",
         kWhy[why]);
}

// A car's job landed: the main passes since its first stale draw, if it had one.
void staleEnded(Car& car) {
    if (!car.staleRun) return;
    car.staleRun = false;
    uint32_t passes = g_pass - car.staleFrom;
    int      span = 5;
    if (passes <= 1)
        span = static_cast<int>(passes);
    else if (passes <= 3)
        span = 2;
    else if (passes <= 7)
        span = 3;
    else if (passes <= kStaleMaxPasses)
        span = 4;
    ++g_staleSpans[span];
}

// While `car`'s build is queued or on the worker, this call takes its most recent program
// (car.programs[0]) instead of the walk: its frames and conditions made for this call as a reuse
// makes them, its decisions as its build took them (staleReplay), assembled as a reuse is.
// `reason`: why that program's replay disagreed this call; `dropped`: this call dropped a program
// of the car. False: the call walks as before.
bool staleTake(Car& car, int reason, bool dropped, const ShareCall& call,
               baked::CarDrawList& list) {
    if (car.job < 0 || car.cockpit || list.cockpit) return false;
    int refusal = -1;
    if (!car.count)
        refusal = kStaleNoProgram;
    else if (dropped)
        refusal = kStaleDropped;
    else if (!staleSafe(reason))
        refusal = kStaleNever;
    else if (g_pass - car.jobPass > kStaleMaxPasses)
        refusal = kStalePast;
    if (refusal >= 0) {
        staleRefused(refusal);
        return false;
    }
    Program& p = g_programs[static_cast<size_t>(car.programs[0])];
    int      why = 0;
    bool     remade = p.valid && staleReplayGuarded(p, call, why) == kAgrees;
    if (!remade) {
        staleRefused(kStaleRemake);
        return false;
    }
    p.lastTaken = g_pass;
    assemble(p, list, list, ++g_reuses % kWholeEvery == 0, false);
    if (!car.staleRun) {
        car.staleRun = true;
        car.staleFrom = g_pass;
    }
    ++g_staleTotal;
    if (g_stale[reason]++ == 0)
        logf("baked cars: a car was drawn from its previous program while its build was on the "
             "worker -- %s differed (report 105 section 4.63; logged once a reason)",
             kRebuildName[reason]);
    if (g_staleTotal == g_staleNextLog) {
        logStaleTotals("so far");
        g_staleNextLog = g_staleNextLog <= 100000000u ? g_staleNextLog * 10 : 0;
    }
    return true;
}

// A call no kept program agrees with: the build askBuild asks for, and while the car's build is
// on the worker its most recent program instead of the walk.
ProgramOutcome noAgreement(Car& car, int why, int staleReason, bool dropped,
                           const ShareCall& call, baked::CarDrawList& list) {
    ProgramOutcome asked = askBuild(car, why, call, list);
    if constexpr (kCarStaleWhileBuilding) {
        if ((asked == kProgramWalk || asked == kProgramQueued) &&
            staleTake(car, staleReason, dropped, call, list))
            return kProgramReused;
    }
    return asked;
}

// A job the worker handed back (report 105 section 4.61), on the render thread at a pass begin:
// kept when it was built for the Car, car bake and epoch that still hold, else what it found is
// what a build here would have left on its Car (carProgramTapeEnd). The slot is free again.
void landJob(uint32_t slot) {
    BuildJob& job = g_jobs[slot];
    Car*      car = nullptr;
    if (job.car >= 0 && static_cast<size_t>(job.car) < g_cars.size()) {
        Car& c = g_cars[static_cast<size_t>(job.car)];
        if (c.record == job.record && c.serial == job.carSerial) car = &c;
    }
    if (car && car->job == static_cast<int16_t>(slot)) {
        car->job = -1;
        staleEnded(*car);
    }
    bool holds = car && job.snapshot.epoch == carBuildWorkerEpoch() &&
                 job.snapshot.generation == carBakeGeneration() &&
                 job.snapshot.rebaked == carBakeTotals().objectsRebaked;
    int outcome = job.status;
    if (holds) {
        // The gates its hazards named, as a walk here learns them.
        for (uintptr_t node : job.learnedGates) carConfigLearn(node);
        if (outcome == kJobBuilt) {
            int index = programSlot(*car);
            outcome = kLandNoRoom;
            if (index >= 0) {
                Program& p = g_programs[static_cast<size_t>(index)];
                p.car = carIndex(*car);
                p.valid = false;
                if (keepInto(index, *car, job.build, job.list, job.snapshot.margin)) {
                    outcome = kLandKept;
                    if (job.eye == 0 || job.eye == 1)
                        car->eyeLast[job.eye] = static_cast<int16_t>(index);
                }
            }
        } else if (outcome == kJobHazard) {
            afterHazard(*car, job.learned, job.frames);
        } else if (outcome == kJobRefused || outcome == kJobThrew) {
            car->buildAfter = g_pass + kRefusedRetry;
        } else if (outcome >= kJobObject && outcome <= kJobScan) {
            car->buildHere = static_cast<int8_t>(kDetachedObject + (outcome - kJobObject));
        }
    } else if (outcome != kJobCancelled && outcome != kJobAged) {
        outcome = kLandStale;
    }
    if (g_landed[outcome]++ == 0 && (outcome == kJobThrew || outcome == kLandNoRoom))
        logf("baked cars: a build from the worker %s (report 105 section 4.61; logged once)",
             outcome == kJobThrew ? "threw (an allocation?); the car is built again later"
                                  : "found no program slot or region run; it is not kept");
    g_freeJobs.push_back(static_cast<uint16_t>(slot));
}

void landJobs() {
    uint32_t slot = 0;
    while (carBuildWorkerTakeDone(slot))
        if (slot < g_jobs.size()) landJob(slot);
}

}  // namespace

void carProgramInstall() {
    if (g_installed) return;
    g_installed = true;
    bool   region = programArenaInstall(kFreeFloorBytes);
    size_t budget = region ? programArenaUse().bytes : kBudgetBytes;
    logf("baked cars: lists kept across frames (report 105 sections 4.23, 4.24) -- in every baked "
         "pass a car's list is walked once with every gate both ways (or, where its memo cannot be "
         "followed so, every gate within %.2f m of the eye and every gate in the wheels), deciding "
         "for its eye every configuration gate, then re-posed and assembled for each eye and frame "
         "until its LOD, a register, a child pointer, the helmet colour, the pass's inputs or a "
         "decided gate changes; up to %u lists a car, %zu lists and %zu KiB for all cars (%s), up "
         "to %zu Cars; past the budget or the pool the list taken least recently is dropped",
         kProgramMargin, kConfigsPerCar, kMaxPrograms, budget >> 10,
         region ? "the program region" : "the heap", kMaxCars);
    // Report 105 section 4.61: after the region is placed, which the worker's frees compare with.
    const char* why = "kCarBuildWorker is false";
    if (kCarBuildWorker) {
        g_jobs.resize(kCarBuildQueue);
        g_freeJobs.clear();
        for (uint32_t i = kCarBuildQueue; i > 0; --i)
            g_freeJobs.push_back(static_cast<uint16_t>(i - 1));
        g_worker = carBuildWorkerStart(&runJob, why);
    }
    if (g_worker)
        logf("baked cars: program builds on a worker thread (report 105 section 4.61) -- a car "
             "call that needs a build walks as before and queues it with a snapshot of the call "
             "(%u jobs at most, one a Car; one queued over %u main passes before it starts is "
             "dropped); the worker reads GPL's graph only inside a baked pass and lets the render "
             "thread in at every node; a finished build is kept at the next pass begin and taken "
             "by the replay as any program. Cockpit builds, and what the worker cannot do (bake an "
             "object, resolve a texture, scan a gate), stay on the render thread",
             kCarBuildQueue, kJobMaxAge);
    else
        logf("baked cars: program builds on the render thread (report 105 section 4.61) -- the "
             "build worker is NOT used: %s",
             why);
    if (g_worker && kCarStaleWhileBuilding)
        logf("baked cars: while a car's build is on the worker it is drawn from its most recent "
             "program instead of walked (report 105 section 4.63) -- where that program differs "
             "only in its LOD, a selector camera, a decided or configuration gate, a register or "
             "the helmet colour, for at most %u main passes after the build was queued; a new "
             "car, a dropped program, a moved child pointer and the cockpit walk as before",
             kStaleMaxPasses);
    else if (g_worker)
        logf("baked cars: a car whose build is on the worker is walked as before (report 105 "
             "section 4.63) -- kCarStaleWhileBuilding is false");
    logf("baked cars: a call tries %s (kCarEyeFirst %d); a replay's pose matrices %s "
         "(kCarPoseCache %d, %u entries, 1 hit in %u checked against poseMatrix) (report 105 "
         "section 4.64)",
         kCarEyeFirst ? "first the program its eye took last, then the rest most recently taken "
                        "first"
                      : "a car's programs most recently taken first",
         kCarEyeFirst ? 1 : 0,
         kCarPoseCache ? "come from a cache keyed by the XFORM and its six pose words"
                       : "are computed in every replay",
         kCarPoseCache ? 1 : 0, kPoseSets * kPoseWays, kPoseCheckEvery);
}

void carProgramPassBegin(bool mirror, bool enabled, int pane) {
    carPassViewBegin(mirror, pane);
    g_building = -1;
    g_verifyPending = false;
    g_active = enabled;
    g_mirror = mirror;
    if (!enabled) return;
    // Section 4.64's pose cache starts again with each car set.
    if (carBakeGeneration() != g_poseGeneration) {
        posesClear();
        g_poseGeneration = carBakeGeneration();
        g_poseOff = false;
    }
    // The passes a program's age and a car's retries count are the main passes; a mirror pass
    // takes and builds programs only (report 105 section 4.42).
    if (!mirror) {
        ++g_pass;
        for (Car& car : g_cars)
            if (car.record && !car.count && car.job < 0 && g_pass - car.lastTaken > kCarIdlePasses)
                car = Car{};
        // Section 4.63's totals, when the car set changes after stale draws.
        uint32_t generation = carBakeGeneration();
        if (generation != g_staleGeneration) {
            if (g_staleTotal != g_staleLogged) logStaleTotals("up to a change of the car set");
            g_staleGeneration = generation;
        }
    }
    // The worker's finished builds are kept before this pass takes programs, then the worker may
    // read until the pass ends (CarBuildWorker.h).
    if (!g_worker) return;
    g_passClock.store(g_pass, std::memory_order_relaxed);
    landJobs();
    carBuildGateOpen();
}

void carProgramDropAll() {
    // Every job in flight comes back cancelled and finds no Car.
    carBuildWorkerCancel();
    if (g_staleTotal != g_staleLogged) logStaleTotals("up to a drop of every program");
    for (size_t i = 0; i < g_programs.size(); ++i)
        if (g_programs[i].car >= 0) dropProgram(static_cast<int>(i));
    for (Car& car : g_cars) car = Car{};
    programArenaTrim();
    posesClear();
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
    if (!car->count) return noAgreement(*car, kNoProgram, kNoProgram, false, call, list);
    // The car's programs, most recently taken first: the first whose inputs, reads and decisions
    // all hold for this call is taken. One that cannot hold again (the pass's inputs or the bake
    // moved, its tape names frames it does not hold, its replay faulted) is dropped. `first`: the
    // reason of the one left most recent, for a stale take (section 4.63).
    int      why = -1;
    int      first = kNoProgram;
    bool     dropped = false;
    Program* p = nullptr;
    uint32_t position = 0;
    int      eye = carPassView().eye == 0 || carPassView().eye == 1 ? carPassView().eye : -1;
    // A candidate's inputs, then its replay.
    auto test = [&](int index, int& reason) {
        Program& candidate = g_programs[static_cast<size_t>(index)];
        reason = kInputs;
        Verdict verdict = kRebuildIt;
        if (sameInputs(candidate, call, list)) {
            verdict = replayGuarded(candidate, call, reason);
        }
        return verdict;
    };
    // Section 4.64 (a1): the program this eye took last goes first where another is more recent;
    // the loop below passes over it (`skip`) when it was refused.
    uint32_t ownAt = 0;
    int      ownWhy = -1;
    int      skip = -1;
    if constexpr (kCarEyeFirst) {
        for (uint32_t i = 1; eye >= 0 && i < car->count && !ownAt; ++i)
            if (car->programs[i] == car->eyeLast[eye]) ownAt = i;
    }
    if (ownAt) {
        int     index = car->programs[ownAt];
        int     reason = kInputs;
        Verdict verdict = test(index, reason);
        if (verdict == kWalkIt) return kProgramWalk;
        if (verdict == kAgrees) {
            p = &g_programs[static_cast<size_t>(index)];
            position = ownAt;
        } else if (reason == kInputs || reason == kFrames || reason == kFault) {
            ownWhy = reason;
            dropProgram(index);
            dropped = true;
        } else {
            ownWhy = reason;
            skip = index;
        }
    }
    while (!p && position < car->count) {
        int index = car->programs[position];
        if (index == skip) {
            if (position == 0) first = ownWhy;
            ++position;
            continue;
        }
        int     reason = kInputs;
        Verdict verdict = test(index, reason);
        if (verdict == kWalkIt) return kProgramWalk;
        if (verdict == kAgrees) {
            p = &g_programs[static_cast<size_t>(index)];
            break;
        }
        if (why < 0) why = reason;
        if (reason == kInputs || reason == kFrames || reason == kFault) {
            dropProgram(index);
            dropped = true;
            continue;
        }
        if (position == 0) first = reason;
        ++position;
    }
    if (why < 0) why = ownWhy;
    if (!p) return noAgreement(*car, why < 0 ? kNoProgram : why, first, dropped, call, list);
    if (position > 0) {
        int index = car->programs[position];
        for (uint32_t i = position; i > 0; --i) car->programs[i] = car->programs[i - 1];
        car->programs[0] = static_cast<int16_t>(index);
    }
    if (eye >= 0) car->eyeLast[eye] = car->programs[0];
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
    ShareTape& tape = g_build.tape;
    margin = g_cars[static_cast<size_t>(p.car)].nextMargin;
    tapeBegin(tape, call, margin, carBakeGeneration(), carBakeTotals().objectsRebaked);
    p.valid = false;
    return &tape;
}

bool carProgramTapeEnd(ShareTape& tape, int hazard, ShareRefusal refusal, baked::CarDrawList& list,
                       uint32_t learned) {
    if (g_building < 0 || static_cast<size_t>(g_building) >= g_programs.size() ||
        &g_build.tape != &tape || g_programs[static_cast<size_t>(g_building)].car < 0 ||
        static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car) >= g_cars.size()) {
        g_building = -1;
        return false;
    }
    int      index = g_building;
    Program& p = g_programs[static_cast<size_t>(index)];
    Car&     car = g_cars[static_cast<size_t>(p.car)];
    g_building = -1;
    bool         frames = false;
    BuildVerdict verdict = checkBuild(g_build, hazard, refusal, list, frames);
    if (verdict == kBuildHazard) afterHazard(car, learned, frames);
    if (verdict == kBuildRefused) car.buildAfter = g_pass + kRefusedRetry;
    if (verdict != kBuildKeep) {
        dropProgram(index);
        return false;
    }
    compileBuild(g_build, list);
    if (!keepInto(index, car, g_build, list, car.nextMargin)) return false;
    int eye = carPassView().eye;
    if (eye == 0 || eye == 1) car.eyeLast[eye] = static_cast<int16_t>(index);
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
