#include "baked_gpl/CarProgram.h"

#include "Hooks.h"
#include "Log.h"

#include "baked/CarScene.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarConfig.h"
#include "baked_gpl/CarOrder.h"
#include "baked_gpl/CarPassView.h"
#include "baked_gpl/CarUnion.h"

#include <windows.h>

#include <intrin.h>

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
// (report 105 section 4.42), or one per glass (bakedMirrorCarKey 1) or per glass and eye (2,
// section 4.54): a field of 26 with the cockpit and three glasses needs 27 + 78 (+ 78 more at 2).
constexpr size_t   kMaxCars = 256;
// Every car's configurations together (report 105 section 4.24); bakedCarConfigs 0 keeps one a
// car, as section 4.23 did. The vectors they hold stay under kBudgetBytes: past it the program
// taken least recently is dropped. The pool is above what the budget holds of the 2020 grid's
// programs (~380 KiB each, section 4.55), so the budget is the limit (section 4.54).
constexpr size_t   kMaxPrograms = 1024;
constexpr uint32_t kConfigsPerCar = 8;
// About 700 programs of the 2020 grid's (~380 KiB each, section 4.55): the mirror passes' lists by
// glass and eye at a full-field start. The process has 2 GiB of address space (gplc67.exe is not
// /LARGEADDRESSAWARE), ~920 MiB of it free after the track's bake; while less than
// kFreeFloorBytes is free, a kept build only replaces what it displaces (budgetFor).
constexpr size_t   kBudgetBytes = size_t{256} << 20;
constexpr uint64_t kFreeFloorBytes = uint64_t{384} << 20;
// The configurations a car's kept builds were, newest last, so a build can be told apart as new
// or as one the car held before (report 105 section 4.31); and the items whose turning refused a
// held program, counted per second, the most frequent logged.
constexpr size_t   kSeenConfigs = 32;
constexpr size_t   kTurnedItems = 16;
constexpr size_t   kTurnedLogged = 4;
constexpr size_t   kMaxFrames = 257;       // the root and CarWalk's kMaxWalkFrames
constexpr int32_t  kMaxSelectorCases = 4096;
constexpr int32_t  kMaxPoolScan = 2048;    // as CarShare's
constexpr size_t   kMaxTeamsLogged = 12;
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
// One replay in kReplayProfileEvery is timed by part; one in kPlaneCheckEvery compares the plane
// tests' floats with the model's (report 105 section 4.28).
constexpr uint32_t kReplayProfileEvery = 16;
constexpr uint32_t kPlaneCheckEvery = 64;
// The cockpit's kept list (report 105 section 4.50) is verified on its own count of reuses, one a
// main pass, so its walks against the program do not depend on how the cars' reuses fall.
constexpr uint32_t kCockpitVerifyEvery = 32;

// The replay's parts, as timed (Second::replayPart).
enum ReplayPart : int {
    kPartChecks,      // registers, child pointers, sprite colours
    kPartPool,        // GPL's vertex memo
    kPartFrames,      // the frames re-made from the XFORM words
    kPartConditions,  // this eye's conditions
    kPartBoxes,       // configuration boxes
    kPartGates,       // decided gates
    kPartSelectors,   // selectors
    kReplayPartCount
};

// An assembly's parts, timed on every reuse (report 105 section 4.35, Second::assemblePart).
enum AssemblePart : int {
    kAssembleFrames,   // the header and the re-made frames
    kAssembleOrder,    // the drawOrder: the memo's look-up, its copy or a fresh emission
    kAssembleCopies,   // the draws and materials (unless elided), the dynamic vertices and indices
    kAssembleSkin,     // the skinned corners carried through the frames
    kAssembleTail,     // the counts
    kAssemblePartCount
};

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

// Why the cockpit's first call walked without asking for a kept list (report 105 section 4.50).
enum CockpitWalked : int {
    kCockpitOptionOff,  // bakedCockpitProgram = 0
    kCockpitStepOne,    // a step-1 pass: the glass is GPL's, and CockpitGlass.h's set needs walks
    kCockpitInactive,   // kept lists are off in this pass (bakedCarProgram 0, debug view 18)
    kCockpitDisabled,   // a verify walk found a MISMATCH since the car set was bound
    kCockpitWalkedCount
};

// Why a program was dropped (report 105 section 4.31); kHeld: it was not.
enum Dropped : uint8_t {
    kHeld,
    kDropLimit,   // the car held configsPerCar()
    kDropPool,    // kMaxPrograms were held
    kDropBudget,  // kBudgetBytes were held
    kDropUnused,  // not taken for kEvictPasses
    kDropStale,   // the pass's inputs, its frames or a fault
    kDroppedCount
};

// A configuration a car's kept build was: the sides of its configuration gates and boxes, and
// its LOD (the kind-0 selections), each hashed.
struct SeenConfig {
    uint32_t config = 0;
    uint32_t lod = 0;
    uint32_t droppedAt = 0;  // g_pass
    uint8_t  dropped = kHeld;
    bool     used = false;
};

// What a held program's replay found turned: a configuration gate's plane or box node, a decided
// gate's plane, or a selector node.
enum TurnedKind : uint8_t {
    kTurnedConfigGate,
    kTurnedConfigBox,
    kTurnedGate,
    kTurnedLod,
    kTurnedCamera,
    kTurnedOther,  // inputs, registers, child pointers, the helmet colour, frames, a fault
    kTurnedCount
};

struct TurnedItem {
    uintptr_t address = 0;
    uint32_t  count = 0;
    uint8_t   kind = kTurnedOther;
};

// Why a car is walked as before (stage 1) instead.
enum Walked : int {
    kGplMemo,    // GPL's vertex memo holds one of the program's vertices
    kPoolFull,   // GPL's vertex memo too full to scan
    kNoPool,     // GPL's vertex memo was not placed at install
    kBackedOff,  // both kinds of build could not be kept; waiting to try again
    kWaiting,    // the last build was refused for a fault or a leaf, or taught CarConfig gates
    kChurned,    // rebuilt on nearly every pass: backed off
    kChurnWait,  // the passes of that backoff (kChurnBackoff)
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
    uint32_t  config = 0;        // SeenConfig's hashes of this program
    uint32_t  lod = 0;
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
    // A mirror Car's glass and eye (report 105 section 4.54, bakedMirrorCarKey): -1 where the key
    // leaves them out.
    int8_t    pane = -1;
    int8_t    eye = -1;
    // Its team model's `car` XFORM, which every car of the team shares, or the `cockpit` XFORM; and
    // that XFORM's root box (CarPassView.h), to name it in the churn line.
    uintptr_t handle = 0;
    uintptr_t rootBox = 0;
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
    bool      churnWait = false;  // buildAfter is the churn backoff's (kChurnWait)
    uint32_t  kept = 0;  // programs kept for it since it was placed: its configurations and LODs
    // Its kept builds' configurations, the oldest overwritten first (seenNext), and how many were
    // new to it.
    SeenConfig seen[kSeenConfigs] = {};
    uint32_t   seenNext = 0;
    uint32_t   distinct = 0;
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
    uint32_t mirrorChurned = 0;     // mirror cars backed off for rebuilding every pass
    uint32_t mirrorChurnReset = 0;  // mirror cars' runs of builds ended by a reuse (section 4.51)
    // Report 105 section 4.54: mirror first calls skipped off the view and those walked as a check
    // of the skip (neither in `calls`), and those keyed more coarsely than bakedMirrorCarKey asks,
    // the pass's glass or eye not known.
    uint32_t mirrorSkipped = 0;
    uint32_t mirrorChecked = 0;
    uint32_t keyNoPane = 0;
    uint32_t keyNoEye = 0;
    uint32_t calls = 0;
    uint32_t callsOff = 0;
    // The cockpit's first calls (section 4.48), which the cars' Second counts and leaves to the
    // cockpit's (report 105 section 4.50); in the cockpit's, those walked without asking for a
    // kept list, by CockpitWalked, and the glass polygons whose UVs the assemblies took from GPL.
    uint32_t cockpit = 0;
    uint32_t cockpitWalked[kCockpitWalkedCount] = {};
    uint64_t glassRefreshed = 0;
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
    // Kept builds while the free address space was under kFreeFloorBytes, or unknown (budgetFor).
    uint32_t keptAtFloor = 0;
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
    // Agreeing replays timed by part (TSC ticks), and what they walked.
    uint64_t replayPart[kReplayPartCount] = {};
    uint32_t replayProfiled = 0;
    // Every assembly by part (TSC ticks, report 105 section 4.35).
    uint64_t assemblePart[kAssemblePartCount] = {};
    uint32_t assembled = 0;
    // The drawOrders: from the memo, patched or emitted whole, with their ticks (CarOrder.h;
    // sections 4.29.1, 4.38, 4.53).
    OrderSecond order;
    uint64_t profiledGates = 0;
    uint64_t profiledConditions = 0;
    uint64_t profiledSelectors = 0;
    uint64_t profiledFrames = 0;
    uint64_t profiledTests = 0;
    uint64_t profiledSkipped = 0;  // conditions passed over under an off one (section 4.37)
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
    // Report 105 section 4.31. Kept builds: a configuration and LOD new to the car (of them, a
    // configuration it had at another LOD), one it held before and had dropped (by Dropped, with
    // the passes since the drop), and one it still held; recognition entries overwritten.
    uint32_t seenNew = 0;
    uint32_t seenNewLod = 0;
    uint32_t seenAgain[kDroppedCount] = {};
    uint64_t seenAgainAge = 0;
    uint32_t seenHeld = 0;
    uint32_t seenOverwritten = 0;
    // Builds of a car that held programs: their count by what the car held (1-3, 4-7, 8 or more)
    // and what refused each held program.
    uint32_t heldBuilds[3] = {};
    uint32_t heldRefused[kTurnedCount] = {};
    uint32_t rootBoxRefused = 0;  // of the configuration boxes' refusals, by the Car's root box
    TurnedItem turned[kTurnedItems] = {};
    uint32_t   turnedMore = 0;  // refusals whose item found no row
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
int                g_building = -1;   // the program carProgramTapeBegin handed out
baked::CarDrawList   g_verifyList;      // the assembled list a verify walk is compared with
bool                 g_verifyPending = false;
Second               g_second;
// The cockpit's kept list (report 105 section 4.50): its counts apart from the cars', whether the
// call being served is the cockpit's (from carProgramTake to the build or verify it asked for),
// its own reuse count for kCockpitVerifyEvery, the Car a verify walk is for, and the car set a
// MISMATCH turned the cockpit's programs off in.
Second               g_cockpitSecond;
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
uint32_t             g_swapped = 0;    // the last assembly's order nodes put the other way round
int64_t              g_skinTicks = 0;  // the last assembly's skinned corners
uint32_t             g_replays = 0;    // replays run, for kReplayProfileEvery and kPlaneCheckEvery
// Per plane test of the program being replayed: 2 not tested yet, else the eye's side (1 front).
std::vector<uint8_t> g_side;
// g_on and g_side as the skip check evaluates them, every condition visited (section 4.37).
std::vector<uint8_t> g_onCheck;
std::vector<uint8_t> g_sideCheck;
// What the last replay that refused found turned (report 105 section 4.31).
uint8_t              g_turnedKind = kTurnedOther;
uintptr_t            g_turnedAddress = 0;

bool on(uint16_t condition) { return condition < g_on.size() && g_on[condition] != 0; }

uint32_t configsPerCar() { return g_options.bakedCarConfigs ? kConfigsPerCar : 1u; }

// The counts of the call being served: the cockpit's or the cars'.
Second& stats() { return g_cockpitCall ? g_cockpitSecond : g_second; }

// The glass and eye a call's Car is keyed by (report 105 section 4.54): none in a main pass, as
// bakedMirrorCarKey asks in a mirror pass -- 1 the glass, 2 the glass and the eye -- and coarser,
// counted, where the pass's glass or eye is not known.
void carKey(int& pane, int& eye, Second& t) {
    pane = -1;
    eye = -1;
    if (!g_mirror || g_options.bakedMirrorCarKey <= 0) return;
    const PassView& view = carPassView();
    if (view.pane < 0) {
        ++t.keyNoPane;
        return;
    }
    pane = view.pane;
    if (g_options.bakedMirrorCarKey < 2) return;
    if (view.eye < 0) {
        ++t.keyNoEye;
        return;
    }
    eye = view.eye;
}

Car* findCar(uintptr_t record, bool mirror, bool cockpit, int pane, int eye) {
    for (Car& c : g_cars)
        if (c.record == record && c.mirror == mirror && c.cockpit == cockpit && c.pane == pane &&
            c.eye == eye)
            return &c;
    return nullptr;
}

Car* allocateCar(uintptr_t record, uintptr_t handle, bool mirror, bool cockpit, int pane,
                 int eye) {
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
    car->eye = static_cast<int8_t>(eye);
    car->handle = handle;
    car->rootBox = carRootBox(handle);
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
// off, its entries left 0 as the parent checks would leave them; `skipped` counts them. The frames
// re-made into g_clips and g_eyes; `frames` their count.
template <bool kSkip>
Verdict evaluateConditions(const Program& p, size_t frames, std::vector<uint8_t>& on,
                           std::vector<uint8_t>& side, uint32_t& skipped, int& reason) {
    const ShareTape& tape = p.tape;
    on.assign(tape.conditions.size() + 1, 0);
    on[0] = 1;
    side.assign(p.tests.size(), 2);
    uint8_t* bits = on.data();
    for (size_t k = 0; k < tape.conditions.size(); ++k) {
        if constexpr (kSkip) {
            uint16_t owner = p.skipOwner[k];
            if (owner && !bits[owner]) {
                skipped += p.skipEnd[k] - static_cast<uint32_t>(k);
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
    g_turnedKind = kTurnedOther;
    g_turnedAddress = 0;
    // One replay in kReplayProfileEvery is timed part by part, counted only when it agrees.
    bool     profile = ++g_replays % kReplayProfileEvery == 0;
    uint64_t part[kReplayPartCount] = {};
    uint64_t mark = profile ? __rdtsc() : 0;
    auto     lap = [&](int which) {
        if (!profile) return;
        uint64_t now = __rdtsc();
        part[which] += now - mark;
        mark = now;
    };
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
    lap(kPartChecks);
    Verdict pool = scanPool(tape, call, t, reason);
    if (pool != kAgrees) return pool;
    lap(kPartPool);

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
    lap(kPartFrames);
    // The plane tests hold the planes as the build read them; now and then they are compared
    // with the model, and a plane that moved drops the program.
    if (p.conditionTest.size() != tape.conditions.size() ||
        p.skipOwner.size() != tape.conditions.size() ||
        p.skipEnd.size() != tape.conditions.size()) {
        reason = kFrames;
        return kRebuildIt;
    }
    // Offset from the timed replays (kReplayProfileEvery), so the checks are not timed as
    // conditions.
    bool check = (g_replays + kReplayProfileEvery / 2) % kPlaneCheckEvery == 0;
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
    uint32_t skipped = 0;
    Verdict  conditions = evaluateConditions<true>(p, frames, g_on, g_side, skipped, reason);
    if (conditions != kAgrees) return conditions;
    lap(kPartConditions);
    // On the plane checks' replays the conditions are evaluated again with every one visited; a
    // difference drops the program (report 105 section 4.37).
    if (check) {
        ++t.skipChecks;
        uint32_t none = 0;
        int      why = 0;
        if (evaluateConditions<false>(p, frames, g_onCheck, g_sideCheck, none, why) != kAgrees ||
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
            g_turnedKind = kTurnedConfigBox;
            g_turnedAddress = b.node;
            return kRebuildIt;
        }
    }
    lap(kPartBoxes);
    for (const ShareGate& g : tape.gates) {
        if (!on(g.condition)) continue;
        if (g.frame >= frames) {
            reason = kFrames;
            return kRebuildIt;
        }
        if (frontSide(g_eyes[g.frame], g.plane) != g.front) {
            reason = g.config ? kConfig : kGate;
            g_turnedKind = g.config ? kTurnedConfigGate : kTurnedGate;
            g_turnedAddress = g.plane;
            return kRebuildIt;
        }
    }
    lap(kPartGates);
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
            g_turnedKind = s.kind == 0 ? kTurnedLod : kTurnedCamera;
            g_turnedAddress = s.node;
            return kRebuildIt;
        }
    }
    lap(kPartSelectors);
    if (profile) {
        for (int i = 0; i < kReplayPartCount; ++i) t.replayPart[i] += part[i];
        ++t.replayProfiled;
        t.profiledGates += tape.gates.size();
        t.profiledConditions += tape.conditions.size();
        t.profiledSkipped += skipped;
        t.profiledSelectors += tape.selectors.size();
        t.profiledFrames += tape.frames.size();
        t.profiledTests += p.tests.size();
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
    Second&  t = stats();
    uint64_t mark = __rdtsc();
    auto     lap = [&](int part) {
        uint64_t now = __rdtsc();
        t.assemblePart[part] += now - mark;
        mark = now;
    };
    ++t.assembled;
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
    lap(kAssembleFrames);
    OrderSource source{p.tape.orders.data(),       static_cast<uint32_t>(p.tape.orders.size()),
                       p.drawConditions.data(),    static_cast<uint32_t>(p.drawConditions.size()),
                       p.drawPolygons.data(),      g_on.data(),
                       static_cast<uint32_t>(g_on.size())};
    OrderTaken taken = orderTake(source, p.order, verify, g_reuses, out.drawOrder, t.order);
    OrderMemo* memo = taken.memo;
    g_swapped = taken.swapped;
    // A patch check's whole emission is kept out of the assembly's parts (section 4.53).
    mark += taken.checkTicks;
    lap(kAssembleOrder);
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
    lap(kAssembleCopies);
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
    t.glassRefreshed += p.tape.glass.size();
    QueryPerformanceCounter(&skinEnd);
    g_skinTicks = skinEnd.QuadPart - skinStart.QuadPart;
    lap(kAssembleSkin);
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
    lap(kAssembleTail);
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

// -- configuration churn (report 105 section 4.31) ----------------------------------------------

uint32_t mixWord(uint32_t hash, uint32_t value) { return (hash ^ value) * 16777619u; }

// A kept build's configuration: the sides of its configuration gates (by plane) and boxes (by
// node); and its LOD: the child each kind-0 selector took.
void configHashes(const ShareTape& tape, uint32_t& config, uint32_t& lod) {
    config = 2166136261u;
    lod = 2166136261u;
    for (const ShareGate& g : tape.gates)
        if (g.config) config = mixWord(mixWord(config, static_cast<uint32_t>(g.plane)), g.front);
    for (const ShareBox& b : tape.boxes)
        if (b.config) config = mixWord(mixWord(config, static_cast<uint32_t>(b.node)), b.culled);
    for (const ShareSelector& s : tape.selectors)
        if (s.kind == 0)
            lod = mixWord(mixWord(lod, static_cast<uint32_t>(s.node)),
                          static_cast<uint32_t>(s.chosen));
}

// Marks the car's record of a kept program's configuration as dropped `how`.
void markDropped(Car& car, const Program& p, uint8_t how) {
    for (SeenConfig& s : car.seen)
        if (s.used && s.dropped == kHeld && s.config == p.config && s.lod == p.lod) {
            s.dropped = how;
            s.droppedAt = g_pass;
            return;
        }
}

// A kept build of `car`: new to it, or one it held before (dropped, or still held).
void recordSeen(Car& car, const Program& p, Second& t) {
    bool knownConfig = false;
    for (SeenConfig& s : car.seen) {
        if (!s.used) continue;
        if (s.config == p.config && s.lod == p.lod) {
            if (s.dropped == kHeld) {
                ++t.seenHeld;
            } else {
                ++t.seenAgain[s.dropped];
                t.seenAgainAge += g_pass - s.droppedAt;
                s.dropped = kHeld;
            }
            return;
        }
        if (s.config == p.config) knownConfig = true;
    }
    ++t.seenNew;
    ++car.distinct;
    if (knownConfig) ++t.seenNewLod;
    SeenConfig& slot = car.seen[car.seenNext];
    if (slot.used) ++t.seenOverwritten;
    slot = SeenConfig{p.config, p.lod, 0, kHeld, true};
    car.seenNext = (car.seenNext + 1) % kSeenConfigs;
}

// A call none of whose `n` held programs agreed: what refused each; `rootBox` the Car's.
void countRefusals(Second& t, uint32_t n, const uint8_t* kinds, const uintptr_t* addresses,
                   uintptr_t rootBox) {
    ++t.heldBuilds[n >= 8 ? 2 : n >= 4 ? 1 : 0];
    for (uint32_t i = 0; i < n; ++i) {
        ++t.heldRefused[kinds[i]];
        if (kinds[i] == kTurnedConfigBox && rootBox && addresses[i] == rootBox)
            ++t.rootBoxRefused;
        if (kinds[i] == kTurnedOther) continue;
        TurnedItem* row = nullptr;
        for (TurnedItem& item : t.turned)
            if (item.count == 0 || (item.address == addresses[i] && item.kind == kinds[i])) {
                row = &item;
                break;
            }
        if (!row) {
            ++t.turnedMore;
            continue;
        }
        row->address = addresses[i];
        row->kind = kinds[i];
        ++row->count;
    }
}

// Takes program `index` out of its car's list and frees its vectors.
void dropProgram(int index, uint8_t how) {
    Program& p = g_programs[static_cast<size_t>(index)];
    if (p.car >= 0 && static_cast<size_t>(p.car) < g_cars.size()) {
        Car&     car = g_cars[static_cast<size_t>(p.car)];
        uint32_t kept = 0;
        for (uint32_t i = 0; i < car.count; ++i)
            if (car.programs[i] != index) car.programs[kept++] = car.programs[i];
        car.count = kept;
        if (p.valid) markDropped(car, p, how);
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

size_t budgetFor(size_t added, Second& t) {
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
    ++t.keptAtFloor;
    return g_floorCap;
}

// A slot for a new program of `car`: the car's least recent one when it holds configsPerCar(),
// else a free slot, else the program of any car taken least recently. -1 when there is none.
int programSlot(Car& car, Second& t) {
    if (car.count >= configsPerCar()) {
        int index = car.programs[car.count - 1];
        dropProgram(index, kDropLimit);
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
    dropProgram(index, kDropPool);
    ++t.droppedPool;
    return index;
}

// Which build to ask for, or why not.
ProgramOutcome askBuild(Car& car, int why, Second& t) {
    if (g_pass < car.buildAfter) {
        ++t.walked[car.churnWait ? kChurnWait : kWaiting];
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
    // A car rebuilt on nearly every pass costs more than the walk its programs save. A mirror car's
    // run of builds also ends at a reuse (bakedMirrorChurnReset, carProgramTake).
    if (why != kNoProgram && g_pass - car.built <= kChurnPasses) {
        if (++car.churn >= kChurnLimit) {
            car.churn = 0;
            car.buildAfter = g_pass + kChurnBackoff;
            car.churnWait = true;
            ++t.walked[kChurned];
            if (g_mirror) ++t.mirrorChurned;
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
           t.glass.capacity() * sizeof(ShareGlass) +
           p.tests.capacity() * sizeof(PlaneTest) +
           p.conditionTest.capacity() * sizeof(uint32_t) +
           p.drawConditions.capacity() * sizeof(uint16_t) +
           p.drawPolygons.capacity() * sizeof(uint32_t) +
           p.skipOwner.capacity() * sizeof(uint16_t) + p.skipEnd.capacity() * sizeof(uint32_t) +
           orderIndexBytes(p.order.index);
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
    // Per team model: the cars of one team share their `car` XFORM.
    struct Team {
        uintptr_t handle;
        uint32_t  cars;
        uint32_t  programs;
        uint32_t  kept;
    };
    Team   teams[kMaxTeamsLogged] = {};
    size_t teamCount = 0, teamsMore = 0;
    for (const Car& car : g_cars) {
        if (!car.record) continue;
        size_t k = 0;
        while (k < teamCount && teams[k].handle != car.handle) ++k;
        if (k == teamCount) {
            if (teamCount == kMaxTeamsLogged) {
                ++teamsMore;
                continue;
            }
            teams[teamCount++] = Team{car.handle, 0, 0, 0};
        }
        ++teams[k].cars;
        teams[k].programs += car.count;
        teams[k].kept += car.kept;
    }
    char   perTeam[512] = {};
    size_t used = 0;
    for (size_t k = 0; k < teamCount && used < sizeof(perTeam); ++k) {
        int n = std::snprintf(perTeam + used, sizeof(perTeam) - used, "%s%u/%u/%u",
                              k ? " " : "", teams[k].cars, teams[k].programs, teams[k].kept);
        if (n < 0) break;
        used += static_cast<size_t>(n);
    }
    ConfigCounts c = carConfigCounts();
    double perReuse = t.reuses ? static_cast<double>(t.reuses) : 1.0;
    double perCall = t.calls ? static_cast<double>(t.calls) : 1.0;
    double perKept = t.keptEvery + t.keptMargin ? static_cast<double>(t.keptEvery + t.keptMargin)
                                                : 1.0;
    double replay = us * static_cast<double>(t.replayTicks) / perReuse;
    double skin = us * static_cast<double>(t.skinTicks) / perReuse;
    double total = us * static_cast<double>(t.reuseTicks) / perReuse;
    logf("baked cars: configurations (bakedCarConfigs %d) -- programs held %u for %u Cars (%.1f a "
         "Car, most %u of %u), %.0f of %zu KiB; per team model Cars/programs/kept so far: %s%s "
         "(%zu more); reuses taken from another program of the car than its last %u, replays %.2f "
         "a first call; dropped: unused %u, a car's limit %u, the pool of %zu %u, the budget %u "
         "(kept builds with under %u MiB of address space free %u, section 4.55), "
         "inputs or frames moved %u; configuration gates and boxes: %u scanned, %u with a "
         "register write below (%u control nodes memoised in %u KiB), %u learned from hazards (+%u "
         "this second; hazards naming none: memo %u, registers %u, order %u, conditions full %u, "
         "too big %u), decided a kept build %.1f gates and %.2f boxes; left both ways unanswered "
         "%u (scans faulted %u, too deep %u, memo full %u), learned set full %u; a reuse %.1f us "
         "= replay %.1f + draws %.1f + skinned corners %.1f",
         g_options.bakedCarConfigs, held, cars, cars ? static_cast<double>(held) / cars : 0.0, most,
         configsPerCar(), static_cast<double>(g_bytes) / 1024.0, kBudgetBytes >> 10, perTeam,
         teamsMore ? " ..." : "", teamsMore, t.switched,
         static_cast<double>(t.replays) / perCall, t.evicted, t.droppedLimit, kMaxPrograms,
         t.droppedPool, t.droppedBudget, static_cast<unsigned>(kFreeFloorBytes >> 20),
         t.keptAtFloor, t.droppedStale, c.scanned, c.writers, c.nodes,
         c.tableKiB, c.learned, c.learnedNow, t.unlearned[kUnionMemo],
         t.unlearned[kUnionRegister], t.unlearned[kUnionOrder], t.unlearned[kUnionConditions],
         t.unlearned[kUnionTooBig], t.keptConfigGates / perKept, t.keptConfigBoxes / perKept,
         c.unanswered, c.scanFaults, c.scanDeep, c.scanFull, c.learnFull, total, replay,
         std::max(0.0, total - replay - skin), skin);
    if (t.replayProfiled) {
        uint64_t sum = 0;
        for (uint64_t ticks : t.replayPart) sum += ticks;
        double share[kReplayPartCount] = {};
        for (int i = 0; i < kReplayPartCount; ++i)
            share[i] = sum ? 100.0 * static_cast<double>(t.replayPart[i]) / sum : 0.0;
        double n = t.replayProfiled;
        logf("baked cars: replay by parts, %u agreeing replays timed (1 in %u) -- checks "
             "(registers, child pointers, sprite colours) %.0f%%, GPL's vertex memo %.0f%%, frames "
             "%.0f%%, conditions %.0f%%, configuration boxes %.0f%%, gates %.0f%%, selectors %.0f%%; "
             "per replay %.1f frames, %.0f conditions (visited %.0f, skipped under an off "
             "condition %.0f, section 4.37) in %.0f plane tests (section 4.28), %.0f gates, %.1f "
             "selectors; plane tests compared with the model in %u replays, a plane moved %u "
             "(must be 0), builds not kept for a plane that faulted %u; condition skips (section "
             "4.37): %.1f runs a kept build, runs not contiguous %u (must be 0), conditions "
             "evaluated again without skips in %u replays, differing %u (must be 0); elided "
             "draws: %u of %u reuses (report 105 section 4.27), whole for the renderer's check "
             "%u; draw orders from the program's memo %u (section 4.29.1)",
             t.replayProfiled, kReplayProfileEvery, share[kPartChecks], share[kPartPool],
             share[kPartFrames], share[kPartConditions], share[kPartBoxes], share[kPartGates],
             share[kPartSelectors], static_cast<double>(t.profiledFrames) / n,
             static_cast<double>(t.profiledConditions) / n,
             static_cast<double>(t.profiledConditions - std::min(t.profiledConditions,
                                                                 t.profiledSkipped)) / n,
             static_cast<double>(t.profiledSkipped) / n,
             static_cast<double>(t.profiledTests) / n, static_cast<double>(t.profiledGates) / n,
             static_cast<double>(t.profiledSelectors) / n, t.planeChecks, t.planesMoved,
             t.planeFaults, static_cast<double>(t.keptSkipRuns) / perKept, t.skipRunsRefused,
             t.skipChecks, t.skipsDiffered, t.elided, t.reuses, t.wholeChecks,
             t.order.memoised);
    }
    carConfigSecond();
}

// Configuration churn (report 105 section 4.31), once a second after the configurations line.
void logChurn(const Second& t) {
    uint32_t cars = 0, distinct = 0, most = 0;
    for (const Car& car : g_cars) {
        if (!car.record) continue;
        ++cars;
        distinct += car.distinct;
        most = std::max(most, car.distinct);
    }
    uint32_t again = 0;
    for (uint32_t n : t.seenAgain) again += n;
    // The items refused most often, most first.
    const TurnedItem* top[kTurnedLogged] = {};
    for (const TurnedItem& item : t.turned) {
        if (!item.count) continue;
        for (size_t k = 0; k < kTurnedLogged; ++k)
            if (!top[k] || item.count > top[k]->count) {
                for (size_t j = kTurnedLogged - 1; j > k; --j) top[j] = top[j - 1];
                top[k] = &item;
                break;
            }
    }
    static const char* const kTurnedNames[kTurnedCount] = {
        "configuration gate", "configuration box", "decided gate", "LOD selector",
        "camera selector", "other"};
    // A box item that is some Car's root box (CarPassView.h, report 105 section 4.54) says so.
    auto rootBox = [](const TurnedItem& item) {
        if (item.kind != kTurnedConfigBox) return false;
        for (const Car& car : g_cars)
            if (car.record && car.rootBox && car.rootBox == item.address) return true;
        return false;
    };
    char   items[320] = {};
    size_t used = 0;
    for (size_t k = 0; k < kTurnedLogged && top[k] && used < sizeof(items); ++k) {
        int n = std::snprintf(items + used, sizeof(items) - used, "%s%s %08X%s x%u",
                              k ? ", " : "", kTurnedNames[top[k]->kind],
                              static_cast<uint32_t>(top[k]->address),
                              rootBox(*top[k]) ? " (a car's root box)" : "", top[k]->count);
        if (n < 0) break;
        used += static_cast<size_t>(n);
    }
    logf("baked cars: configuration churn (report 105 section 4.31) -- kept builds: new to the car "
         "%u (a configuration it had at another LOD %u), held before and dropped for %u (a car's "
         "limit %u, the pool %u, the budget %u, unused %u, inputs or frames moved %u; %.0f main "
         "passes after the drop), identical to one it still held %u, records overwritten %u; "
         "distinct configurations a Car so far %.1f, most %u (%u Cars); calls no held program "
         "agreed with, by programs held 1-3 %u, 4-7 %u, %u or more %u; what refused the held "
         "programs: configuration gate %u, configuration box %u (of them the Car's root box %u, "
         "section 4.54), decided gate %u, LOD %u, camera %u, other %u; refused most by: %s%s",
         t.seenNew, t.seenNewLod, again, t.seenAgain[kDropLimit], t.seenAgain[kDropPool],
         t.seenAgain[kDropBudget], t.seenAgain[kDropUnused], t.seenAgain[kDropStale],
         again ? static_cast<double>(t.seenAgainAge) / again : 0.0, t.seenHeld, t.seenOverwritten,
         cars ? static_cast<double>(distinct) / cars : 0.0, most, cars, t.heldBuilds[0],
         t.heldBuilds[1], kConfigsPerCar, t.heldBuilds[2], t.heldRefused[kTurnedConfigGate],
         t.heldRefused[kTurnedConfigBox], t.rootBoxRefused, t.heldRefused[kTurnedGate],
         t.heldRefused[kTurnedLod],
         t.heldRefused[kTurnedCamera], t.heldRefused[kTurnedOther], used ? items : "none",
         t.turnedMore ? " (more items than rows)" : "");
}

// A reuse by parts in microseconds (report 105 section 4.35), once a second: the replay's parts
// from its timed replays (1 in kReplayProfileEvery), the assembly's from every reuse. The TSC is
// calibrated against QueryPerformanceCounter over the second; the first second has no rate.
void logReuseParts(const Second& t, double us) {
    static uint64_t lastTsc = 0;
    static int64_t  lastQpc = 0;
    LARGE_INTEGER   qpc;
    QueryPerformanceCounter(&qpc);
    uint64_t tsc = __rdtsc();
    double   perUs = 0.0;
    if (lastQpc && qpc.QuadPart > lastQpc)
        perUs = static_cast<double>(tsc - lastTsc) /
                (us * static_cast<double>(qpc.QuadPart - lastQpc));
    lastTsc = tsc;
    lastQpc = qpc.QuadPart;
    if (perUs <= 0.0 || !t.assembled) return;
    double replay[kReplayPartCount] = {};
    double replayed = t.replayProfiled ? static_cast<double>(t.replayProfiled) : 1.0;
    for (int i = 0; i < kReplayPartCount; ++i)
        replay[i] = static_cast<double>(t.replayPart[i]) / replayed / perUs;
    double part[kAssemblePartCount] = {};
    double assembled = t.assembled;
    for (int i = 0; i < kAssemblePartCount; ++i)
        part[i] = static_cast<double>(t.assemblePart[i]) / assembled / perUs;
    const OrderSecond& o = t.order;
    uint32_t hits = t.assembled - std::min(t.assembled, o.misses);
    double   missUs = o.misses ? static_cast<double>(o.missTicks) / o.misses / perUs : 0.0;
    double hitUs = hits ? (static_cast<double>(t.assemblePart[kAssembleOrder]) -
                           static_cast<double>(o.missTicks)) / hits / perUs
                        : 0.0;
    double total = t.reuses ? us * static_cast<double>(t.reuseTicks) / t.reuses : 0.0;
    double replayTotal = t.reuses ? us * static_cast<double>(t.replayTicks) / t.reuses : 0.0;
    double timed = o.timed ? static_cast<double>(o.timed) : 1.0;
    logf("baked cars: a reuse by parts (report 105 section 4.35) -- %u reuses, a reuse %.1f us = "
         "finding the program and its replay %.1f + the assembly %.1f; a timed replay (%u): checks "
         "%.2f, GPL's vertex memo %.2f, frames %.2f, conditions %.2f, configuration boxes %.2f, "
         "gates %.2f, selectors %.2f us; an assembly: frames %.2f, draw order %.2f (from the memo "
         "%.2f us x %u, emitted %.2f us x %u), copies %.2f (draws, materials, dynamic vertices "
         "and indices), skinned corners %.2f, the rest %.2f us; an order emitted for a memo "
         "(section 4.38, %u): emitRange %.2f us over %.0f draws and %.0f order nodes to %.0f "
         "drawn, its copy into the list %.2f, the memo's fill %.2f us; memos refilled %u, of them "
         "never used since their fill %u; TSC %.0f MHz",
         t.reuses, total, replayTotal,
         part[kAssembleFrames] + part[kAssembleOrder] + part[kAssembleCopies] +
             part[kAssembleSkin] + part[kAssembleTail],
         t.replayProfiled, replay[kPartChecks], replay[kPartPool], replay[kPartFrames],
         replay[kPartConditions], replay[kPartBoxes], replay[kPartGates], replay[kPartSelectors],
         part[kAssembleFrames], part[kAssembleOrder], hitUs, hits, missUs, o.misses,
         part[kAssembleCopies], part[kAssembleSkin], part[kAssembleTail], o.timed,
         static_cast<double>(o.emitTicks) / timed / perUs,
         static_cast<double>(o.draws) / timed, static_cast<double>(o.nodes) / timed,
         static_cast<double>(o.emitted) / timed,
         static_cast<double>(o.copyTicks) / timed / perUs,
         static_cast<double>(o.fillTicks) / timed / perUs, o.refilled, o.unused, perUs);
    orderLogPatch(o, perUs);
}

// The cockpit's kept list (report 105 section 4.50), once a second after the cars' lines: `c` its
// own counts, `firstCalls` every first call of the cockpit in a baked main pass.
void logCockpitProgram(const Second& c, uint32_t firstCalls, double us) {
    uint32_t hazards = 0, walked = 0, without = 0;
    for (uint32_t n : c.failed) hazards += n;
    for (uint32_t n : c.walked) walked += n;
    for (uint32_t n : c.cockpitWalked) without += n;
    double reuse = c.reuses ? us * static_cast<double>(c.reuseTicks) / c.reuses : 0.0;
    double build = c.builds ? us * static_cast<double>(c.buildTicks) / c.builds : 0.0;
    double plain = c.plainWalks ? us * static_cast<double>(c.plainTicks) / c.plainWalks : 0.0;
    double glass = c.reuses ? static_cast<double>(c.glassRefreshed) / c.reuses : 0.0;
    logf("baked cars: the cockpit's kept list (report 105 section 4.50, bakedCockpitProgram %d) "
         "-- first calls %u, %u of them asking for a kept list: REUSED %u, verified %u of %u (1 "
         "in %u reuses), MISMATCH %u (must be 0; one turns the cockpit's kept lists off until the "
         "car set changes: %s), verify walks faulted %u; builds %u (no kept list %u, pass inputs "
         "moved %u, registers moved %u, a child pointer moved %u, helmet colour moved %u, LOD "
         "changed %u, selector camera moved %u, a decided gate turned %u, a configuration gate "
         "turned %u, frames %u, a glass's corner count moved %u, replay faulted %u): kept %u "
         "with every gate, %u with the margin; not kept: hazards %u, frames %u, a fault or "
         "refused leaf %u, a plane that faulted %u; walked as before %u (GPL's vertex memo holds "
         "a vertex %u, too full to scan %u, not placed %u, backed off %u, waiting %u, rebuilt "
         "every pass %u, backing off %u, no slot %u); walked without asking %u (bakedCockpitProgram 0 %u, a step-1 "
         "pass %u, kept lists off in the pass %u, after a MISMATCH %u); a reuse %.1f us, a build "
         "%.1f us, a verify walk %.1f us; glass polygons given GPL's live UVs %.1f a reuse; "
         "programs dropped as stale %u; hand-overs lost %u (must be 0)",
         g_options.bakedCockpitProgram, firstCalls, c.calls, c.reused, c.verified, c.verifies,
         kCockpitVerifyEvery, c.mismatched, g_cockpitOff ? "OFF now" : "on", c.verifyFaulted,
         c.builds, c.rebuilt[kNoProgram], c.rebuilt[kInputs], c.rebuilt[kRegisters],
         c.rebuilt[kChild], c.rebuilt[kSprite], c.rebuilt[kLod], c.rebuilt[kCamera],
         c.rebuilt[kGate], c.rebuilt[kConfig], c.rebuilt[kFrames], c.rebuilt[kGlass],
         c.rebuilt[kFault], c.keptEvery, c.keptMargin, hazards, c.failedFrames, c.failedOther,
         c.planeFaults, walked, c.walked[kGplMemo], c.walked[kPoolFull], c.walked[kNoPool],
         c.walked[kBackedOff], c.walked[kWaiting], c.walked[kChurned], c.walked[kChurnWait],
         c.walked[kNoSlot], without,
         c.cockpitWalked[kCockpitOptionOff], c.cockpitWalked[kCockpitStepOne],
         c.cockpitWalked[kCockpitInactive], c.cockpitWalked[kCockpitDisabled], reuse, build,
         plain, glass, c.droppedStale, c.lost);
    if (c.mismatched)
        logf("baked cars: the cockpit's kept list, first MISMATCH this second -- %s",
             c.firstMismatch);
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
         "changes; one reuse in %u is walked anyway and compared run by run; up to %zu Cars, "
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

void carProgramPassBegin(bool mirror, bool enabled, int pane) {
    Second& t = g_second;
    carPassViewBegin(mirror, pane);
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
        dropProgram(static_cast<int>(i), kDropUnused);
        ++t.evicted;
    }
    for (Car& car : g_cars)
        if (car.record && !car.count && g_pass - car.lastTaken > kEvictPasses) car = Car{};
}

namespace {

// carProgramTake past the mirror cull; `taken` the program a reuse or verify took.
ProgramOutcome takeProgram(const ShareCall& call, baked::CarDrawList& list,
                           const Program*& taken) {
    Second& t = stats();
    // The cockpit gplvr draws (report 105 section 4.48) keeps programs as a car does (section
    // 4.50), under a Car of its own, in the passes whose list holds the glass (step 2, section
    // 4.49): the glass's UVs are taken from GPL's PRIM at each assembly. A step-1 pass leaves the
    // glass to GPL through CockpitGlass.h's set, which only walks fill, so it walks as before.
    if (list.cockpit) {
        ++g_second.cockpit;
        if (g_cockpitOff && g_cockpitOffGeneration != carBakeGeneration()) g_cockpitOff = false;
        int walked = !g_options.bakedCockpitProgram ? kCockpitOptionOff
                     : !list.overlay               ? kCockpitStepOne
                     : !g_active || g_mirror       ? kCockpitInactive
                     : g_cockpitOff                ? kCockpitDisabled
                                                   : -1;
        if (walked >= 0) {
            ++t.cockpitWalked[walked];
            return kProgramWalk;
        }
    }
    if (!g_active) {
        if (!g_mirror) ++t.callsOff;
        return kProgramWalk;
    }
    ++t.calls;
    if (g_mirror) ++t.mirrorCalls;
    int pane = -1, eye = -1;
    carKey(pane, eye, t);
    Car* car = findCar(call.record, g_mirror, list.cockpit, pane, eye);
    if (!car) car = allocateCar(call.record, call.handle, g_mirror, list.cockpit, pane, eye);
    if (!car) {
        ++t.walked[kNoSlot];
        return kProgramWalk;
    }
    car->lastTaken = g_pass;
    if (car->handle != call.handle) car->rootBox = carRootBox(call.handle);
    car->handle = call.handle;
    if (!car->count) return askBuild(*car, kNoProgram, t);
    // The car's programs, most recently taken first: the first whose inputs, reads and decisions
    // all hold for this call is taken. One that cannot hold again (the pass's inputs or the bake
    // moved, its tape names frames it does not hold, its replay faulted) is dropped.
    LARGE_INTEGER start, replayed, end;
    QueryPerformanceCounter(&start);
    int       why = -1;
    Program*  p = nullptr;
    uint32_t  position = 0;
    uint32_t  refused = 0;
    uint8_t   refusedKinds[kConfigsPerCar];
    uintptr_t refusedAddresses[kConfigsPerCar];
    while (position < car->count) {
        int      index = car->programs[position];
        Program& candidate = g_programs[static_cast<size_t>(index)];
        int      reason = kInputs;
        Verdict  verdict = kRebuildIt;
        g_turnedKind = kTurnedOther;
        g_turnedAddress = 0;
        if (sameInputs(candidate, call, list)) {
            ++t.replays;
            verdict = replayGuarded(candidate, call, t, reason);
        }
        if (verdict == kRebuildIt && refused < kConfigsPerCar) {
            refusedKinds[refused] = reason == kFault ? kTurnedOther : g_turnedKind;
            refusedAddresses[refused] = g_turnedAddress;
            ++refused;
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
            dropProgram(index, kDropStale);
            ++t.droppedStale;
            continue;
        }
        ++position;
    }
    if (!p) {
        if (refused) countRefusals(t, refused, refusedKinds, refusedAddresses, car->rootBox);
        return askBuild(*car, why < 0 ? kNoProgram : why, t);
    }
    taken = p;
    if (position > 0) {
        int index = car->programs[position];
        for (uint32_t i = position; i > 0; --i) car->programs[i] = car->programs[i - 1];
        car->programs[0] = static_cast<int16_t>(index);
        ++t.switched;
    }
    p->lastTaken = g_pass;
    // A mirror car keyed by record serves up to six cameras a frame (three glasses, two eyes), one
    // keyed by glass two (section 4.54), and builds a program for each on consecutive passes; a
    // reuse shows its builds are being taken.
    if (car->mirror && g_options.bakedMirrorChurnReset && car->churn) {
        car->churn = 0;
        ++t.mirrorChurnReset;
    }
    QueryPerformanceCounter(&replayed);
    bool verify = list.cockpit ? ++g_cockpitReuses % kCockpitVerifyEvery == 0
                               : ++g_reuses % kProgramVerifyEvery == 0;
    if (verify) g_verifyCar = carIndex(*car);
    // A verify walk compares the whole assembly; one reuse in kWholeEvery comes whole for the car
    // renderer to check the rows it holds.
    assemble(*p, list, verify ? g_verifyList : list,
             verify || (!list.cockpit && g_reuses % kWholeEvery == 0), verify);
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

}  // namespace

ProgramOutcome carProgramTake(const ShareCall& call, baked::CarDrawList& list) {
    g_building = -1;
    g_verifyPending = false;
    g_verifyCar = -1;
    g_cockpitCall = list.cockpit;
    carCullTakeBegin();
    // A mirror car whose root box the view culls draws nothing; it is not listed, and no kept list
    // is tried (report 105 section 4.54, bakedMirrorCarCull). The check of the skip walks the car
    // as before: a culled walk ends at the root box, and builds no empty program.
    CullVerdict cull = g_mirror && !list.cockpit ? carCullTest(call) : kCullGoOn;
    if (cull != kCullGoOn) {
        if (cull == kCullSkip)
            ++g_second.mirrorSkipped;
        else
            ++g_second.mirrorChecked;
        carEyePairNote(call.record, kEyeSkipped, 0, 0);
        return cull == kCullSkip ? kProgramEmpty : kProgramWalk;
    }
    const Program* taken = nullptr;
    ProgramOutcome outcome = takeProgram(call, list, taken);
    if (!list.cockpit)
        carEyePairNote(call.record,
                       outcome == kProgramBuild ? kEyeBuilt : taken ? kEyeTook : kEyeWalked,
                       taken ? taken->config : 0u, taken ? taken->lod : 0u);
    return outcome;
}

void carProgramListed(const baked::CarDrawList& list) { carCullListed(list); }

ShareTape* carProgramTapeBegin(const ShareCall& call, float& margin) {
    if (g_building < 0 || static_cast<size_t>(g_building) >= g_programs.size() ||
        g_programs[static_cast<size_t>(g_building)].car < 0 ||
        static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car) >= g_cars.size() ||
        g_cars[static_cast<size_t>(g_programs[static_cast<size_t>(g_building)].car)].record !=
            call.record) {
        ++stats().lost;
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
    tape.glass.clear();
    std::memcpy(tape.rootEye, call.eye, sizeof(tape.rootEye));
    p.valid = false;
    return &tape;
}

bool carProgramTapeEnd(ShareTape& tape, int hazard, ShareRefusal refusal, baked::CarDrawList& list,
                       int64_t ticks, uint32_t learned, const uint32_t* unlearned) {
    Second& t = stats();
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
            car.churnWait = false;
        } else {
            car.learning = 0;
            if (car.nextMargin <= 0.0f)
                car.everyGateAfter = g_pass + kEveryGateRetry;
            else
                car.marginAfter = g_pass + kMarginRetry;
        }
        dropProgram(index, kDropStale);
        return false;
    }
    if (tape.refusal != kShareOk ||
        !ordersNest(tape.orders, 0, static_cast<uint32_t>(list.draws.size()), 0,
                    static_cast<uint32_t>(tape.orders.size()), 0) ||
        !skinsValid(tape, list)) {
        ++t.failedOther;
        car.buildAfter = g_pass + kRefusedRetry;
        car.churnWait = false;
        dropProgram(index, kDropStale);
        return false;
    }
    if (!compileTests(p)) {
        ++t.planeFaults;
        car.buildAfter = g_pass + kRefusedRetry;
        car.churnWait = false;
        dropProgram(index, kDropStale);
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
    configHashes(tape, p.config, p.lod);
    recordSeen(car, p, t);
    // The order patch's index (report 105 section 4.53) over the draws and order nodes as kept;
    // ordersNest held above.
    p.order = OrderState{};
    if (g_options.bakedOrderPatch) {
        OrderSource source{tape.orders.data(),      static_cast<uint32_t>(tape.orders.size()),
                           p.drawConditions.data(), static_cast<uint32_t>(p.drawConditions.size()),
                           p.drawPolygons.data(),   nullptr,
                           static_cast<uint32_t>(tape.conditions.size() + 1)};
        if (!orderIndexBuild(p.order.index, source)) ++t.order.indexRefused;
    }
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
    ++car.kept;
    uint32_t count = std::min(car.count, configsPerCar() - 1);
    for (uint32_t i = count; i > 0; --i) car.programs[i] = car.programs[i - 1];
    car.programs[0] = static_cast<int16_t>(index);
    car.count = count + 1;
    size_t budget = budgetFor(p.bytes, t);
    while (g_bytes > budget) {
        int oldest = leastRecent(index);
        if (oldest < 0) break;
        dropProgram(oldest, kDropBudget);
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
    Second& t = stats();
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
    // The cockpit is drawn over the eye in painter's order, where a wrong list shows: its programs
    // are dropped and it walks every pass until the car set changes (report 105 section 4.50).
    if (!g_cockpitCall) return;
    g_cockpitOff = true;
    g_cockpitOffGeneration = carBakeGeneration();
    if (g_verifyCar < 0 || static_cast<size_t>(g_verifyCar) >= g_cars.size()) return;
    Car& car = g_cars[static_cast<size_t>(g_verifyCar)];
    for (uint32_t n = car.count; n > 0 && car.count; --n) {
        dropProgram(car.programs[0], kDropStale);
        ++t.droppedStale;
    }
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
             "kept %u, waiting after a refused or teaching build %u, rebuilt every pass %u and "
             "then backing off %u (section 4.51), past "
             "%zu Cars or no program slot %u); GPL's memo scans whose slots in a program's address "
             "range were none of its vertices (section 4.43) %u; hand-overs lost %u (must be 0); "
             "the cockpit's first calls, counted on their own line (section 4.50) %u; "
             "of these, in mirror "
             "passes (report 105 section 4.42, bakedCarProgramMirrors %d): passes %u (off %u), "
             "first calls %u, REUSED %u, verified walks %u, builds %u, rebuilt every pass %u, "
             "runs of builds ended by a reuse %u (bakedMirrorChurnReset %d); first calls skipped "
             "off the view before these %u, walked as a check of the skip %u (section 4.54, "
             "bakedMirrorCarCull %d)",
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
             t.walked[kChurnWait], kMaxCars, t.walked[kNoSlot], t.poolNarrowed, t.lost, t.cockpit,
             g_options.bakedCarProgramMirrors,
             t.mirrorPasses, t.mirrorPassesOff, t.mirrorCalls, t.mirrorReused, t.mirrorVerifies,
             t.mirrorBuilds, t.mirrorChurned, t.mirrorChurnReset, g_options.bakedMirrorChurnReset,
             t.mirrorSkipped, t.mirrorChecked, g_options.bakedMirrorCarCull);
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
        logChurn(t);
        logReuseParts(t, us);
        if (t.mismatched)
            logf("baked cars: lists kept across frames, first MISMATCH this second -- %s",
                 t.firstMismatch);
        if (t.cockpit) logCockpitProgram(g_cockpitSecond, t.cockpit, us);
        CarKeyCensus census;
        census.option = g_options.bakedMirrorCarKey;
        census.maxCars = static_cast<uint32_t>(kMaxCars);
        census.noPane = t.keyNoPane;
        census.noEye = t.keyNoEye;
        for (const Car& car : g_cars) {
            if (!car.record) continue;
            if (car.cockpit)
                ++census.cockpitCars;
            else if (!car.mirror)
                ++census.mainCars;
            else if (car.pane < 0)
                ++census.mirrorRecord;
            else if (car.eye < 0)
                ++census.mirrorGlass;
            else
                ++census.mirrorEye;
        }
        carPassViewSecond(census);
    }
    g_second = Second{};
    g_cockpitSecond = Second{};
}

}  // namespace gplvr
