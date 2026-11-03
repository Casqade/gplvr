#include "baked_gpl/CarWalk.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"

#include "baked/CarScene.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarBuildWorker.h"
#include "baked_gpl/CarConfig.h"
#include "baked_gpl/CarMemo.h"
#include "baked_gpl/CarPoseWrapper.h"
#include "baked_gpl/CarProgram.h"
#include "baked_gpl/CarShare.h"
#include "baked_gpl/CarWalkAddresses.h"
#include "baked_gpl/CarWalkStats.h"
#include "baked_gpl/CockpitDraw.h"
#include "baked_gpl/CockpitGlass.h"
#include "baked_gpl/ResidualDepth.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

namespace gplvr {
namespace {

using namespace carwalk;

// The game addresses are in CarWalkAddresses.h.

// Render record and team instance fields (report 103 section 1).
constexpr unsigned kRecordDriver = 32;       // R+0x20, the index gpl_driverGetTeamModelInstance takes
constexpr unsigned kRecordCockpit = 48;      // R+0x30, non-zero for the player's cockpit view
constexpr unsigned kRecordDriverObject = 52;  // R+0x34, the per-driver object, else the instance's
constexpr unsigned kObjectRoot = 56;         // Obj3d+0x38, the root node gpl_objDrawRootNode dispatches
constexpr unsigned kInstanceCar = 148;       // I+148, the `car` handle's XFORM
constexpr unsigned kInstanceCockpit = 152;   // I+152, the `cockpit` handle's XFORM
// The four wheel XFORMs I+116, 124, 132, 140, and their models: gpl_carModelSetWheelSpinLod
// 0x46D850 points wheel w's child (w8) at I[13 + 3w + gpl_wheelRateLodLevel(rate)], the rate's
// level 0, 1, 2 at |rate| < 20, < 60, else (0x46D290); gpl_carModelApplyWheelLoss 0x406F60 then
// points a lost rear wheel's at gpl_nullNode.
constexpr unsigned kInstanceWheels = 116;
constexpr unsigned kInstanceWheelModels = 52;
constexpr unsigned kWheelModels = 3;
// The instance's nodes gpl_carModelPoseFromRecord 0x46D490 and gpl_carRecordDraw rewrite per car
// (report 103 section 1): the wheels and hubframes, `car`, `cockpit`, `shifter`, the arms and
// elbows, the seven dials, `steer`, `head`, headSRB0..3 (the helmet colour, w2) and `shadow`.
constexpr unsigned kPosedOffsets[] = {116, 120, 124, 128, 132, 136, 140, 144, 148, 152,
                                      156, 160, 164, 168, 172, 176, 180, 184, 188, 192,
                                      196, 200, 204, 208, 216, 220, 224, 228, 232};
static_assert(sizeof(kPosedOffsets) / sizeof(kPosedOffsets[0]) <= kCarPosedNodes,
              "a snapshot holds every posed node");

constexpr int kRegisterCount = 1024;  // selectors and SETREG index registers & 0x3FF
constexpr int kMaxDepth = 512;
constexpr int kMaxSelectorCases = 4096;

using TeamInstanceFn = void*(__cdecl*)(int driver);
using PoseFn = void(__cdecl*)(void* instance, void* record);
using ShadowFn = int(__cdecl*)(void* record, void* node);
using CarDrawFn = void(__fastcall*)(void* record, void* unused);

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

float bitsFloat(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// -- the walk ------------------------------------------------------------------------------------
//
// Its counters are in CarWalkStats.h.

// The frame of one node: clip from local (P x MV x every pose above), the eye and the cell's
// clip planes in local space, as gpl_matrixTranslate and the rotations keep them, and the poses
// alone (root from local). `id` is its place in the walk's own frames (Walk::frames), which GPL's
// vertex memo is emulated against; with a draw list, `index` is its place in the list.
// `key` names the chain of XFORM nodes from the root, the same in every walk of the car; a union
// walk also gives each frame `canon`, the id of the first frame of this walk entered from the
// same XFORM under the same canonical parent, whose matrices are therefore the same.
struct Frame {
    float     clip[16];
    float     eye[3];
    float     planes[6][4];
    float     root[12];
    uintptr_t node;
    uint32_t  key;
    uint16_t  index;
    uint16_t  halfFrom;
    uint16_t  id;
    uint16_t  canon;
    uint16_t  parentCanon;
};

constexpr int      kMaxKnownObjects = 16;
constexpr uint16_t kNoFrameId = 0xFFFF;

struct Detached;
// A kept list's walk (report 105 section 4.23): the halves of its order nodes, and the leaves it
// may hold before it gives up (cars18 needs at most ~6,200 at 3 m).
constexpr uint16_t kMaxRegions = 8192;
constexpr uint16_t kNoOrder = 0xFFFF;
constexpr uint32_t kMaxProgramLeaves = 12288;

struct Walk {
    uint32_t planeMask = 0;
    uint32_t classVisible = 0;
    float    lodScale = 1.0f;
    float    dlong = 0.0f;
    float    dlat = 0.0f;
    int32_t  registers[kRegisterCount] = {};
    // The polygons the walk reached, as GPL's counter would move for the same draw: one per
    // polygon node and one per sprite. The list must hold them all (shareRefusal).
    uint32_t polygons = 0;
    // The cell's user planes take part in the decisions (GPL's own walk of one cell's record);
    // false for a whole-car list.
    bool     cellPlanes = true;
    bool     lightingOn = false;  // gpl_lightingOn: lit leaves memoise their normals

    // Every frame entered this walk, by Frame::id, for the memo's frame of first transform.
    Frame    frames[kMaxWalkFrames];
    uint16_t frameCount = 0;
    SkinCounts skin;
    // Per corner of the polygon being visited: the frame its vertex and its normal were memoised
    // in (Frame::id; 0 for GPL's own memo, taken as the dispatch frame).
    uint16_t cornerFrame[kMaxSkinCorners];
    uint16_t normalFrame[kMaxSkinCorners];
    // The objects this walk has found its leaves in, [prim, prim + size): GPL's memory does not
    // change during a walk, so carBakeObjectOf is asked once per object and walk.
    uintptr_t knownPrim[kMaxKnownObjects];
    uint32_t  knownSize[kMaxKnownObjects];
    int32_t   knownObject[kMaxKnownObjects];
    int       knownObjects = 0;

    // The list the walk fills, the MATERIAL state it fills it with --
    // the current CarMaterial and the texture list a slot indexes -- and the object the last
    // leaf was found in, [prim, prim + primSize).
    baked::CarDrawList* list = nullptr;
    EmitCounts emit;
    uint16_t   material = 0;
    uintptr_t  textureList = 0;
    uint32_t   texturedMask = 0;
    uintptr_t  prim = 0;
    uint32_t   primSize = 0;
    int32_t    object = -1;
    // The cockpit's walk (report 105 section 4.48): mirror.mip's handle, whose textured polygons
    // are left out of the list for GPL; 0 for a car. `glassOurs`: step 2 (section 4.49) lists them
    // with GPL's live UVs, under a material sampling the live mirror.
    uint32_t   glassTexture = 0;
    bool       glassOurs = false;

    // The first eye's walk of a car in VR: its eye-dependent decisions, for the second eye
    // (CarShare.h). Null otherwise.
    ShareTape* tape = nullptr;

    // The union (report 105 section 4.22): with a tape, a gate whose plane passes within
    // `unionMargin` of this eye is taken both ways, each side under a condition of the tape, and
    // `condition` is the one the current subtree is met under; 0 decides every gate. What a
    // condition would make depend on the eye beyond what the list can say is counted in
    // `unionHazards` by UnionRedone, and the car is walked again deciding every gate.
    float    unionMargin = 0.0f;
    uint16_t condition = 0;
    uint32_t unionHazards[kUnionRedoneCount] = {};
    uint32_t blendedLeaves = 0;  // leaves the renderer may blend, appended by a union walk
    // Per register, in a union walk: whether every eye that reaches this point of the walk holds
    // the value in `registers`, and else the condition of the last write, which is the value's
    // for the eyes that condition holds for.
    uint8_t  registerUniform[kRegisterCount];
    uint16_t registerCondition[kRegisterCount];

    // A kept list's walk (report 105 section 4.23, CarProgram.h): a union walk whose list serves
    // later frames and both eyes. Every gate within `unionMargin` -- every gate with `everyGate`,
    // and every gate inside a wheel XFORM (`wheels`), whose frame turns between frames -- is taken
    // both ways; a BOXCULL's subtree is walked under a box condition; a BSP4 or BSP3_ASYM taken
    // both ways seals its two halves (CarDrawList::sealed), so each pass can order them by its own
    // side, and each half is a region: a memo entry or a register written in one half and read in
    // the other, or after the node, depends on that order (kUnionOrder). The first hazard ends the
    // walk (`abort`).
    bool      program = false;
    bool      everyGate = false;
    bool      abort = false;
    uintptr_t wheels[4] = {};
    // Per wheel XFORM, the three models gpl_carModelSetWheelSpinLod 0x46D850 points its child at
    // by spin rate (report 105 section 4.30).
    uintptr_t wheelModels[4][kWheelModels] = {};
    int       wheelDepth = 0;
    uint16_t  region = 0;
    uint16_t  regionCount = 1;
    uint16_t  regionParent[kMaxRegions];
    uint16_t  regionOrder[kMaxRegions];  // the tape.orders index the region is a half of
    uint16_t  regionDepth[kMaxRegions];
    uint16_t  registerRegion[kRegisterCount];  // the region of a register's last eye-dependent write

    // A kept list's walk with configurations (report 105 section 4.24): every configuration gate
    // (CarConfig.h) is decided for this eye, `configGate` the last one met. A hazard that names the
    // gates whose sides it depends on teaches them to CarConfig and the walk goes on, so one walk
    // learns every such gate it meets (`learnedGates` new ones); a hazard that names none (nothing
    // to name, a full set) ends it.
    bool      learn = false;
    uintptr_t configGate = 0;
    uint32_t  learnedGates = 0;

    // GPL's vertex and normal memos as this walk emulates them (beginWalk): CarWalk's own, or the
    // worker's for a build there.
    Memo* vertexMemo = nullptr;
    Memo* normalMemo = nullptr;
    // A build on the worker (report 105 section 4.61, carWalkDetached); null on the render thread.
    Detached* detached = nullptr;
    // emitSkinned's corners.
    baked::CarVertex skinCorners[kMaxSkinCorners];
};

// A build on the worker: its snapshot, what it could not do there, the gates its hazards named
// (CarConfig is the render thread's to write), and its own texture cache.
struct Detached {
    const CarBuildSnapshot* snapshot = nullptr;
    int                     need = kDetachedOk;
    uintptr_t               needAt = 0;
    std::vector<uintptr_t>* learned = nullptr;
    struct Cached {
        uint32_t handle;
        int32_t  texture;
        bool     valid;
    };
    Cached textures[256] = {};
};

// What the worker could not do: the walk ends, and the car is built on the render thread next.
void detachedNeed(Walk& walk, int need, uintptr_t at) {
    if (walk.detached->need == kDetachedOk) {
        walk.detached->need = need;
        walk.detached->needAt = at;
    }
    walk.abort = true;
}

// A worker build's checkpoint, at every node (CarBuildWorker.h): out while the render thread
// closes the gate or pauses it, and on only while its epoch and the car bake it began on hold.
bool detachedGoOn(Walk& walk) {
    int check = carBuildCheckpoint(walk.detached->snapshot->epoch);
    if (check == kCarBuildGoOn ||
        (check == kCarBuildResumed && carBakeGeneration() == walk.detached->snapshot->generation))
        return true;
    detachedNeed(walk, kDetachedCancelled, 0);
    return false;
}

// The words GPL's pose and draw rewrite per car: the snapshot's on the worker, else GPL's own.
uint32_t posedWord(const Walk& walk, uintptr_t node, unsigned index) {
    if (walk.detached) {
        const CarBuildSnapshot& s = *walk.detached->snapshot;
        for (uint32_t i = 0; i < s.posedCount; ++i)
            if (s.posed[i].node == node) return s.posed[i].words[index];
    }
    uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(node + 4u * index), sizeof(value));
    return value;
}

// An XFORM's words 1-6 and its child (w8).
void xformWords(const Walk& walk, uintptr_t node, uint32_t words[7]) {
    for (unsigned i = 0; i < 6; ++i) words[i] = posedWord(walk, node, 1 + i);
    words[6] = posedWord(walk, node, 8);
}

// carConfigDecides, and carConfigLearn's answer; on the worker read only (CarConfig.h), with the
// gates this walk has named so far counted as learned.
bool configDecides(Walk& walk, uintptr_t node) {
    if (!walk.detached) return carConfigDecides(node);
    const std::vector<uintptr_t>& learned = *walk.detached->learned;
    if (std::find(learned.begin(), learned.end(), node) != learned.end()) return true;
    bool decides = false;
    if (!carConfigAnswered(node, decides)) detachedNeed(walk, kDetachedScan, node);
    return decides;
}

int configLearn(Walk& walk, uintptr_t node) {
    if (!walk.detached) return carConfigLearn(node);
    std::vector<uintptr_t>& learned = *walk.detached->learned;
    if (!node) return -1;
    if (std::find(learned.begin(), learned.end(), node) != learned.end()) return 0;
    int known = carConfigKnownLearned(node, static_cast<uint32_t>(learned.size()));
    if (known != 0) return known > 0 ? 0 : -1;
    learned.push_back(node);
    return 1;
}

// -- GPL's vertex and normal memos ----------------------------------------------------------------
//
// The walk's emulation (CarMemo.h), and GPL's own memo word for what GPL memoised before the walk
// (carMemoGplSlot).

static_assert(kNoFrameId == kMemoNoFrame, "memoStore leaves a frame past the walk's limit out");

Memo g_vertexMemo;
Memo g_normalMemo;

// The frame a vertex GPL transforms through its memo takes its transform from: a Frame::id of
// the walk (kNoFrameId: unmemoised, the current frame's), or kGplMemoFrame with `gplSlot` when
// GPL memoised it before the walk.
constexpr uint16_t kGplMemoFrame = 0xFFFE;

uint16_t canonOf(const Walk& walk, uint16_t id) {
    return id < walk.frameCount ? walk.frames[id].canon : id;
}

// What a union walk's list cannot say for every eye; a kept list's walk stops at the first one
// that no gate it named (`named`, CarConfig.h) will remove from later walks.
void unionHazard(Walk& walk, UnionRedone reason, bool named = false) {
    ++walk.unionHazards[reason];
    if (!walk.program || named) return;
    walk.abort = true;
}

bool regionWithin(const Walk& walk, uint16_t inner, uint16_t outer);

// A configurations walk's hazard: the gates whose sides, and the boxes whose insides, `writer` (a
// write's or a memo entry's condition) holds under and `reader` does not. Decided, they make the
// write every reader's. False when there is none, or one cannot join the set.
bool blameConditions(Walk& walk, uint16_t writer, uint16_t reader) {
    if (!walk.learn) return false;
    const ShareTape& tape = *walk.tape;
    bool             named = false;
    while (writer != 0 && !shareImplies(tape, reader, writer)) {
        if (writer > tape.conditions.size()) return false;
        const ShareCondition& c = tape.conditions[writer - 1];
        // A wheel model's condition is GPL's pose, no gate's side: nothing to decide.
        if (c.kind == kConditionChild) return false;
        uintptr_t node = c.node;
        if (c.kind == kConditionBox)
            node = c.plane < tape.boxes.size() ? tape.boxes[c.plane].node : 0;
        int learned = configLearn(walk, node);
        if (learned < 0) return false;
        walk.learnedGates += static_cast<uint32_t>(learned);
        named = true;
        writer = c.parent;
    }
    return named;
}

// The same for regions: the order nodes whose halves hold `writer` and not `reader`. Decided, they
// keep one order for every eye of the program.
bool blameRegions(Walk& walk, uint16_t writer, uint16_t reader) {
    if (!walk.learn) return false;
    const ShareTape& tape = *walk.tape;
    bool             named = false;
    while (writer != 0 && !regionWithin(walk, reader, writer)) {
        uint16_t order = walk.regionOrder[writer];
        if (order >= tape.orders.size()) return false;
        int learned = configLearn(walk, tape.orders[order].node);
        if (learned < 0) return false;
        walk.learnedGates += static_cast<uint32_t>(learned);
        named = true;
        writer = walk.regionParent[writer];
    }
    return named;
}

// Regions (Walk::program): the halves of the order nodes, a tree under region 0. Two regions
// part at the two halves of one order node exactly when their chains below the deepest common
// region start with halves of the same node: there each eye's side decides which comes first.
bool regionsCross(const Walk& walk, uint16_t a, uint16_t b) {
    if (a == b) return false;
    while (walk.regionDepth[a] > walk.regionDepth[b]) a = walk.regionParent[a];
    while (walk.regionDepth[b] > walk.regionDepth[a]) b = walk.regionParent[b];
    if (a == b) return false;
    while (walk.regionParent[a] != walk.regionParent[b]) {
        a = walk.regionParent[a];
        b = walk.regionParent[b];
    }
    return walk.regionOrder[a] == walk.regionOrder[b];
}

// Whether region `inner` is `outer` or lies inside it.
bool regionWithin(const Walk& walk, uint16_t inner, uint16_t outer) {
    while (walk.regionDepth[inner] > walk.regionDepth[outer]) inner = walk.regionParent[inner];
    return inner == outer;
}

// A memo entry a union walk made under `heldCondition`, met under another condition. The union
// walks every eye's visits in one sequence, so each eye's memo holds the first of them its own
// walk makes. When every eye the current condition holds for made that visit, the entry is each
// one's answer. Otherwise an eye may meet the record first here, in this frame: one answer serves
// all only if the entry's frame has the same matrices; anything else depends on the eye. An entry
// made in the other half of an order node than this one is first for the eyes that order that
// half first, and the eyes on the other side meet the record here first: only the same frame
// serves both.
uint16_t unionHeld(Walk& walk, uint16_t held, uint16_t heldCondition, uint16_t heldRegion,
                   const Frame& frame) {
    if (regionsCross(walk, heldRegion, walk.region)) {
        if (held == frame.id) return held;
        unionHazard(walk, kUnionOrder, blameRegions(walk, heldRegion, walk.region));
        return held;
    }
    if (shareImplies(*walk.tape, walk.condition, heldCondition)) return held;
    if (canonOf(walk, held) == canonOf(walk, frame.id)) return frame.id;
    unionHazard(walk, kUnionMemo, blameConditions(walk, heldCondition, walk.condition));
    return held;
}

uint16_t resolveVertex(Walk& walk, uintptr_t record, const Frame& frame, uintptr_t& gplSlot) {
    auto     key = static_cast<uint32_t>(record);
    Memo&    memo = *walk.vertexMemo;
    uint32_t entry = memoEntry(memo, key);
    uint16_t held = 0, heldCondition = 0, heldRegion = 0;
    if (memoAlive(memo, entry, key, held, heldCondition, heldRegion))
        return heldCondition == walk.condition && heldRegion == walk.region
                   ? held
                   : unionHeld(walk, held, heldCondition, heldRegion, frame);
    // A build on the worker takes GPL's memo as empty: each take's scanPool reads the live one.
    gplSlot = walk.detached ? 0 : carMemoGplSlot(record, walk.skin.staleWords);
    if (walk.tape) shareVertex(*walk.tape, record, gplSlot != 0);
    if (gplSlot) return kGplMemoFrame;
    memoStore(memo, entry, key, frame.id, walk.condition, walk.region, walk.skin.vertexOverflow);
    return frame.id;
}

// A NORM record lit by gpl_normalLightCached 0x4CFA70: the frame of its first lighting, kept the
// same way. A memo word GPL set before the walk is taken as the dispatch frame's (0); a build on
// the worker takes GPL's memo as empty.
uint16_t resolveNormal(Walk& walk, uintptr_t record, const Frame& frame) {
    auto     key = static_cast<uint32_t>(record);
    Memo&    memo = *walk.normalMemo;
    uint32_t entry = memoEntry(memo, key);
    uint16_t held = 0, heldCondition = 0, heldRegion = 0;
    if (memoAlive(memo, entry, key, held, heldCondition, heldRegion))
        return heldCondition == walk.condition && heldRegion == walk.region
                   ? held
                   : unionHeld(walk, held, heldCondition, heldRegion, frame);
    if (!walk.detached && at<uint32_t>(record) != 0) {
        ++walk.skin.liveNormals;
        return 0;
    }
    memoStore(memo, entry, key, frame.id, walk.condition, walk.region, walk.skin.normalOverflow);
    return frame.id;
}

// The walk's frame `id`, or `current` when it is not one (unmemoised).
const Frame& frameOf(const Walk& walk, uint16_t id, const Frame& current) {
    return id < walk.frameCount ? walk.frames[id] : current;
}

// gpl_matrixTranslateEuler(t, r): T(t) * Rz * Ry * Rx, the same 3x4 as Scene.cpp's poseMatrix,
// which places every track object (report 57 section 1.2). `words`: the XFORM's (xformWords).
void poseMatrix(const uint32_t words[7], float m[12]) {
    float tx = bitsFloat(words[0]), ty = bitsFloat(words[1]), tz = bitsFloat(words[2]);
    float yaw = bitsFloat(words[3]), pitch = bitsFloat(words[4]), roll = bitsFloat(words[5]);
    float cz = std::cos(yaw), sz = std::sin(yaw);
    float cy = std::cos(pitch), sy = std::sin(pitch);
    float cx = std::cos(roll), sx = std::sin(roll);
    float out[12] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, tx,
                     sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, ty,
                     -sy,     cy * sx,                cy * cx,                tz};
    std::memcpy(m, out, sizeof(out));
}

void enterPose(const Frame& parent, const float m[12], uint32_t planeMask, Frame& child) {
    for (int row = 0; row < 4; ++row) {
        const float* p = parent.clip + row * 4;
        for (int column = 0; column < 4; ++column) {
            float sum = p[0] * m[column] + p[1] * m[4 + column] + p[2] * m[8 + column];
            if (column == 3) sum += p[3];
            child.clip[row * 4 + column] = sum;
        }
    }
    float d[3] = {parent.eye[0] - m[3], parent.eye[1] - m[7], parent.eye[2] - m[11]};
    for (int j = 0; j < 3; ++j) child.eye[j] = m[j] * d[0] + m[4 + j] * d[1] + m[8 + j] * d[2];
    for (int i = 0; i < 6; ++i) {
        if (!(planeMask & (1u << i))) continue;
        const float* n = parent.planes[i];
        for (int j = 0; j < 3; ++j)
            child.planes[i][j] = m[j] * n[0] + m[4 + j] * n[1] + m[8 + j] * n[2];
        child.planes[i][3] = n[0] * m[3] + n[1] * m[7] + n[2] * m[11] + n[3];
    }
}

// gpl_drawQueueSidePredicate 0x4C7880 on a gate's plane (node word 1 points at it).
bool frontSide(const Frame& frame, uintptr_t node) {
    uintptr_t plane = word(node, 1);
    return at<float>(plane) * frame.eye[0] + at<float>(plane + 4) * frame.eye[1] +
               at<float>(plane + 8) * frame.eye[2] + at<float>(plane + 12) >=
           0.0f;
}

// A vertex record is {memo word, x, y, z}.
void vertexAt(uintptr_t vertex, float out[3]) {
    out[0] = at<float>(vertex + 4);
    out[1] = at<float>(vertex + 8);
    out[2] = at<float>(vertex + 12);
}

float clipW(const Frame& frame, const float p[3]) {
    return frame.clip[12] * p[0] + frame.clip[13] * p[1] + frame.clip[14] * p[2] + frame.clip[15];
}

// gpl_vertexTransformCached 0x4CFBB0's outcode, read off its integer tests: 16 when clip z is
// negative; else 4 / 8 when |x| > |w| with x positive / negative, plus 1 / 2 for y the same way.
// The magnitudes are compared as the float bit patterns shifted left one, as the code does.
uint32_t frustumOutcode(const Frame& frame, const float p[3]) {
    float clip[4];
    for (int row = 0; row < 4; ++row)
        clip[row] = frame.clip[row * 4] * p[0] + frame.clip[row * 4 + 1] * p[1] +
                    frame.clip[row * 4 + 2] * p[2] + frame.clip[row * 4 + 3];
    if (floatBits(clip[2]) >> 31) return 16;
    uint32_t w = floatBits(clip[3]) << 1;
    uint32_t code = 0;
    if (w < (floatBits(clip[0]) << 1)) code |= (floatBits(clip[0]) >> 31) ? 8u : 4u;
    if (w < (floatBits(clip[1]) << 1)) code |= (floatBits(clip[1]) >> 31) ? 2u : 1u;
    return code;
}

// gpl_vertexTransformWithClipPlanes 0x4C8710: bit i when the vertex is behind user plane i.
uint32_t planeOutcode(const Walk& walk, const Frame& frame, const float p[3]) {
    uint32_t code = 0;
    for (int i = 0; i < 6; ++i) {
        if (!(walk.planeMask & (1u << i))) continue;
        const float* plane = frame.planes[i];
        float distance = plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3];
        code |= (floatBits(distance) >> 31) << i;
    }
    return code;
}

// What gpl_vertexTransformWithClipPlanes 0x4C8710 hands BOXCULL, SPHERECULL and a kind-0
// selector for `record`: its memo's clip w, frustum outcode, user-plane bits and plane distances,
// from the frame it was first transformed in, or GPL's own slot (+12, +28, +32, +40).
struct Transformed {
    float    w = 0.0f;
    uint32_t outcode = 0;
    uint32_t planeBits = 0;
    float    planes[6] = {};
    uint16_t frame = 0;  // the walk frame they came from, or kGplMemoFrame
};

void transformVertex(Walk& walk, uintptr_t record, const Frame& frame, Transformed& out) {
    uintptr_t gplSlot = 0;
    uint16_t  id = resolveVertex(walk, record, frame, gplSlot);
    out.frame = id;
    if (id == kGplMemoFrame) {
        out.w = at<float>(gplSlot + 12);
        out.outcode = at<uint32_t>(gplSlot + 28);
        out.planeBits = at<uint32_t>(gplSlot + 32);
        for (int i = 0; i < 6; ++i) out.planes[i] = at<float>(gplSlot + 40 + 4u * i);
        return;
    }
    const Frame& where = frameOf(walk, id, frame);
    out.frame = where.id;
    float        p[3];
    vertexAt(record, p);
    out.w = clipW(where, p);
    out.outcode = frustumOutcode(where, p);
    out.planeBits = planeOutcode(walk, where, p);
    for (int i = 0; i < 6; ++i) {
        const float* plane = where.planes[i];
        out.planes[i] = (walk.planeMask & (1u << i))
                            ? plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3]
                            : 0.0f;
    }
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

// -- the draw list -------------------------------------------------------------------------------

// An XFORM's poses under its parent's (root from local).
void enterRoot(const Frame& parent, const float m[12], Frame& child) {
    for (int row = 0; row < 3; ++row) {
        const float* p = parent.root + row * 4;
        for (int column = 0; column < 4; ++column) {
            float sum = p[0] * m[column] + p[1] * m[4 + column] + p[2] * m[8 + column];
            if (column == 3) sum += p[3];
            child.root[row * 4 + column] = sum;
        }
    }
}

// An XFORM's child frame in the list (its root from local already set), and its eye. GPL rebuilds
// the half vector only when a rotation ran, and gpl_matrixTranslateEuler skips an angle that is
// exactly 0 (report 103 section 4). `words`: the XFORM's (xformWords).
void enterFrame(Walk& walk, const Frame& parent, const uint32_t words[7], Frame& child) {
    baked::CarDrawList& list = *walk.list;
    if (list.frames.size() >= 0xFFFF) {
        ++walk.emit.overflow;
        child.index = parent.index;
        child.halfFrom = parent.halfFrom;
        return;
    }
    bool rotates = bitsFloat(words[3]) != 0.0f || bitsFloat(words[4]) != 0.0f ||
                   bitsFloat(words[5]) != 0.0f;
    child.index = static_cast<uint16_t>(list.frames.size());
    child.halfFrom = rotates ? child.index : parent.halfFrom;
    baked::CarFrame out;
    std::memcpy(out.rootFromLocal, child.root, sizeof(out.rootFromLocal));
    std::memcpy(out.eye, child.eye, sizeof(out.eye));
    out.halfFrom = child.halfFrom;
    list.frames.push_back(out);
}

// carBakeTexture through a direct-mapped cache: a handle's texture never changes within one
// generation (CarBake keeps every answer, -1 included, until the set changes or a pass begin
// finds that GPL released what it was made from), so the cache only has to follow
// carBakeGeneration.
int32_t textureOf(uint32_t handle) {
    struct Cached {
        uint32_t handle;
        int32_t  texture;
        uint32_t generation;
        bool     valid;
    };
    static Cached cache[256] = {};
    uint32_t      generation = carBakeGeneration();
    Cached&       slot = cache[(handle * 2654435761u) >> 24];
    if (slot.valid && slot.handle == handle && slot.generation == generation) return slot.texture;
    int32_t texture = carBakeTexture(handle);
    slot = Cached{handle, texture, carBakeGeneration(), true};
    return texture;
}

// The same for a walk: on the worker from CarBake's answers so far (one generation a build), and a
// handle never resolved ends the walk for the render thread.
int32_t textureFor(Walk& walk, uint32_t handle) {
    if (!walk.detached) return textureOf(handle);
    Detached::Cached& slot = walk.detached->textures[(handle * 2654435761u) >> 24];
    if (slot.valid && slot.handle == handle) return slot.texture;
    bool    known = false;
    int32_t texture = carBakeTextureResolved(handle, known);
    if (!known) {
        detachedNeed(walk, kDetachedTexture, handle);
        return -1;
    }
    slot = Detached::Cached{handle, texture, true};
    return texture;
}

bool sameMaterial(const baked::CarMaterial& a, const baked::CarMaterial& b) {
    return a.textureHandle == b.textureHandle && a.surfaceClass == b.surfaceClass &&
           a.textured == b.textured && a.specularExponent == b.specularExponent &&
           floatBits(a.specular) == floatBits(b.specular);
}

// The state GPL's handler for these flags leaves for its subtree. A slot (0x20, 0x4CE160) binds
// entry `word 3` of the list published above, with no range check; a list (0x10, 0x4CDB50)
// binds its entry 0 and publishes itself; one texture (flag 1) binds word 3. A class (flag 4)
// sets the current class and whether family B is textured under it; a specular block (flag 8)
// follows the class word: {int n, float ks}.
void applyMaterial(Walk& walk, uintptr_t node, uint32_t flags, unsigned classWord) {
    baked::CarDrawList& list = *walk.list;
    baked::CarMaterial  material = list.materials[walk.material];
    if (flags & 0x20) {
        if (walk.textureList)
            material.textureHandle = at<uint32_t>(walk.textureList + 4 + 4 * word(node, 3));
        else
            ++walk.emit.slotNoList;
    } else if (flags & 0x10) {
        walk.textureList = node + 12;
        material.textureHandle = word(node, 4);
    } else if (flags & 1) {
        material.textureHandle = word(node, 3);
    }
    if (flags & 4) {
        material.surfaceClass = static_cast<int32_t>(word(node, classWord));
        material.textured =
            static_cast<uint8_t>((walk.texturedMask >> (material.surfaceClass & 31)) & 1);
    }
    if (flags & 8) {
        unsigned specularWord = classWord + ((flags & 4) ? 1 : 0);
        material.specularExponent = static_cast<int32_t>(word(node, specularWord));
        material.specular = wordFloat(node, specularWord + 1);
    }
    const baked::CarMaterial& current = list.materials[walk.material];
    if (sameMaterial(material, current)) return;
    if (material.textureHandle != current.textureHandle) {
        material.texture = textureFor(walk, material.textureHandle);
        if (material.texture < 0 && material.textureHandle) ++walk.emit.unresolved;
        material.liveMirror = walk.glassOurs && material.textureHandle == walk.glassTexture;
    }
    if (list.materials.size() >= 0xFFFF) {
        ++walk.emit.overflow;
        return;
    }
    walk.material = static_cast<uint16_t>(list.materials.size());
    list.materials.push_back(material);
}

// A point (or, with `point` false, a direction) of `frame` in the root frame, and back. The poses
// are rigid: gpl_matrixTranslateEuler ignores the XFORM's scale word.
void localToRoot(const Frame& frame, const float in[3], bool point, float out[3]) {
    const float* m = frame.root;
    for (int row = 0; row < 3; ++row)
        out[row] = m[row * 4] * in[0] + m[row * 4 + 1] * in[1] + m[row * 4 + 2] * in[2] +
                   (point ? m[row * 4 + 3] : 0.0f);
}

void rootToLocal(const Frame& frame, const float in[3], bool point, float out[3]) {
    const float* m = frame.root;
    float d[3] = {in[0] - (point ? m[3] : 0.0f), in[1] - (point ? m[7] : 0.0f),
                  in[2] - (point ? m[11] : 0.0f)};
    for (int j = 0; j < 3; ++j) out[j] = m[j] * d[0] + m[4 + j] * d[1] + m[8 + j] * d[2];
}

int16_t unitToShort(float value) {
    float clamped = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
    return static_cast<int16_t>(std::lround(clamped * 32767.0f));
}

// A polygon node as GPL's kernels take it: every corner through the vertex memo
// (gpl_polyTransformClipEmit 0x4C87D0 / 0x4C88C0 -> 0x4CFBB0) and, while lighting is on, a lit
// polygon's normals through the normal memo (gpl_polyLightVertexNormals 0x4C8B20 -> 0x4CFA70).
// The frames each came from are left in walk.cornerFrame / normalFrame. True when one is not
// `frame`: GPL draws that polygon skinned.
bool resolveCorners(Walk& walk, uintptr_t node, uint8_t type, const Frame& frame) {
    bool familyB = type >= 0x1F;
    bool hasShade = type == 0x1C || type == 0x1E || type == 0x20 || type == 0x22;
    bool hasNormal = type == 0x1D || type == 0x1E || type == 0x21 || type == 0x22;
    uint32_t count = word(node, familyB ? 3 : 2);
    if (count > kMaxSkinCorners) {
        ++walk.skin.tooManyCorners;
        return false;
    }
    unsigned vertexWord = familyB ? 4 + 2 * count : 3;
    bool     other = false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t gplSlot = 0;
        uint16_t  id = resolveVertex(walk, word(node, vertexWord + i), frame, gplSlot);
        if (id == kGplMemoFrame) {
            id = 0;
            ++walk.skin.liveCorners;
        } else if (id == kNoFrameId) {
            id = frame.id;
        } else if (id != frame.id) {
            ++walk.skin.corners;
        }
        walk.cornerFrame[i] = id;
        other = other || id != frame.id;
    }
    unsigned normalWord = vertexWord + count + (hasShade ? count : 0);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t id = frame.id;
        if (hasNormal && walk.lightingOn && walk.list) {
            id = resolveNormal(walk, word(node, normalWord + i), frame);
            if (id == kNoFrameId) id = frame.id;
            if (id != frame.id) ++walk.skin.normals;
        }
        walk.normalFrame[i] = id;
        other = other || id != frame.id;
    }
    return other;
}

// Whether the renderer may blend some of the leaf (its phase 1, alpha below kCoreAlpha): a sprite,
// a corner or literal alpha, or a texture with an alpha channel or a colour key under the
// current material.
bool mayBlend(const Walk& walk, const baked::CarScene& scene, const baked::CarLeaf& leaf) {
    if (leaf.flags & (baked::kCarLeafSprite | baked::kCarLeafCornerAlpha)) return true;
    if (!(leaf.flags & baked::kCarLeafTextured)) return false;
    const baked::CarMaterial& material = walk.list->materials[walk.material];
    if (!material.textured || material.texture < 0 ||
        static_cast<size_t>(material.texture) >= scene.textures.size())
        return false;
    return scene.textures[static_cast<size_t>(material.texture)].image.blendClass() != 0;
}

// A skinned polygon: the baked corners of its leaf, each carried from the frame its vertex (and
// normal) was memoised in through the root into `frame`, appended as a dynamic leaf under
// `frame`. The bake's corners are the polygon's corners in node order, consecutive from corner 0
// (CarScene.cpp, Builder::polygon).
void emitSkinned(Walk& walk, int32_t objectIndex, const baked::CarObject& object,
                 const baked::CarLeaf& leaf, uintptr_t node, uint8_t type, const Frame& frame) {
    uint32_t count = word(node, type >= 0x1F ? 3 : 2);
    // The fan's apex, the leaf's first index, is corner 1 (CarScene.cpp, appendGplFan).
    uint32_t base = leaf.firstIndex < object.indices.size() && object.indices[leaf.firstIndex] > 0
                        ? object.indices[leaf.firstIndex] - 1
                        : 0xFFFFFFFFu;
    if (count < 3 || count > kMaxSkinCorners || base == 0xFFFFFFFFu ||
        base + count > object.vertices.size()) {
        ++walk.skin.noBakedCorners;
        baked::appendCarLeaf(*walk.list, static_cast<uint32_t>(objectIndex), leaf, frame.index,
                             walk.material, 0, walk.condition);
        return;
    }
    baked::CarVertex* corners = walk.skinCorners;
    bool lit = (leaf.flags & baked::kCarLeafLit) != 0;
    // A kept list re-makes these corners from its re-posed frames each pass (CarProgram.cpp).
    if (walk.program) {
        ShareTape& tape = *walk.tape;
        if (frame.id != frame.index && tape.refusal == kShareOk) tape.refusal = kShareFrames;
        ShareSkin skin;
        skin.object = static_cast<uint32_t>(objectIndex);
        skin.base = base;
        skin.count = count;
        skin.firstVertex = static_cast<uint32_t>(walk.list->dynamicVertices.size());
        skin.firstCorner = static_cast<uint32_t>(tape.skinFrames.size());
        skin.frame = frame.id;
        skin.lit = lit;
        tape.skins.push_back(skin);
        for (uint32_t i = 0; i < count; ++i) {
            tape.skinFrames.push_back(walk.cornerFrame[i]);
            tape.skinFrames.push_back(walk.normalFrame[i]);
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        baked::CarVertex vertex = object.vertices[base + i];
        float            rooted[3];
        if (walk.cornerFrame[i] != frame.id) {
            localToRoot(frameOf(walk, walk.cornerFrame[i], frame), vertex.position, true, rooted);
            rootToLocal(frame, rooted, true, vertex.position);
        }
        if (lit && walk.normalFrame[i] != frame.id) {
            float normal[3] = {vertex.normal[0] / 32767.0f, vertex.normal[1] / 32767.0f,
                               vertex.normal[2] / 32767.0f};
            float local[3];
            localToRoot(frameOf(walk, walk.normalFrame[i], frame), normal, false, rooted);
            rootToLocal(frame, rooted, false, local);
            for (int k = 0; k < 3; ++k) vertex.normal[k] = unitToShort(local[k]);
        }
        corners[i] = vertex;
    }
    baked::appendDynamicCarLeaf(*walk.list, leaf, corners, count, frame.index, walk.material,
                                walk.condition);
}

// One polygon or BILLBOARD node: its leaf in the bake of the object that holds it, appended under
// the current frame and material. A sprite carries its colour word as read now (the helmet tint),
// or white when its type has no colour (bit 0x200 clear, gpl_polyEmitTexturedUV). A sprite's
// corners are never memoised: gpl_spriteDrawVerticalAxis 0x4D6490 and the screen-aligned drawer
// build them from the anchor's raw coordinates and transform them inside a push / pop(1).
void emitLeaf(Walk& walk, uintptr_t node, uint8_t type, const Frame& frame, bool skinned) {
    EmitCounts& emit = walk.emit;
    // The cockpit's mirror glass is GPL's in step 1 (CockpitGlass.h), ours in step 2; its corners
    // went through the memo all the same, as GPL's draw takes them.
    bool glass = walk.glassTexture && type >= 0x1F &&
                 walk.list->materials[walk.material].textureHandle == walk.glassTexture;
    if (glass && !walk.glassOurs) {
        ++emit.glass;
        if (!cockpitGlassAdd(node)) ++emit.glassOverflow;
        return;
    }
    if (node - walk.prim >= walk.primSize) {
        int known = 0;
        while (known < walk.knownObjects && node - walk.knownPrim[known] >= walk.knownSize[known])
            ++known;
        if (known < walk.knownObjects) {
            walk.prim = walk.knownPrim[known];
            walk.primSize = walk.knownSize[known];
            walk.object = walk.knownObject[known];
        } else if (walk.detached) {
            // The worker bakes nothing: an object not baked yet is the render thread's to bake.
            bool unbaked = false;
            walk.object = carBakeObjectBaked(node, walk.prim, walk.primSize, unbaked);
            if (unbaked) {
                detachedNeed(walk, kDetachedObject, node);
                return;
            }
        } else {
            walk.object = carBakeObjectOf(node, walk.prim, walk.primSize);
        }
        if (known >= walk.knownObjects && walk.object >= 0 &&
            walk.knownObjects < kMaxKnownObjects) {
            walk.knownPrim[walk.knownObjects] = walk.prim;
            walk.knownSize[walk.knownObjects] = walk.primSize;
            walk.knownObject[walk.knownObjects++] = walk.object;
        }
        if (walk.object < 0) {
            walk.prim = 0;
            walk.primSize = 0;
            ++emit.noObject;
            return;
        }
    }
    const baked::CarScene& scene = carBakeScene();
    if (static_cast<size_t>(walk.object) >= scene.objects.size()) {
        ++emit.noObject;
        return;
    }
    const baked::CarObject& object = scene.objects[static_cast<size_t>(walk.object)];
    uint32_t leafIndex = object.leafAt(static_cast<uint32_t>(node - walk.prim));
    if (leafIndex == baked::kNoCarLeaf) {
        ++emit.notLeaf;
        return;
    }
    const baked::CarLeaf& leaf = object.leaves[leafIndex];
    if (leaf.type != type) {
        ++emit.typeDiffers;
        return;
    }
    // Step 2's glass (report 105 section 4.49): a dynamic leaf with the UVs MirrorUv keeps in
    // GPL's PRIM, which a baked leaf would not follow. A skinned glass is refused, the list too.
    if (glass && !(leaf.flags & baked::kCarLeafNoGeometry)) {
        static baked::CarVertex corners[kMaxSkinCorners];
        uint32_t                count = 0;
        if (skinned || !cockpitGlassCorners(object, leaf, node, corners, kMaxSkinCorners, count)) {
            ++emit.glassRefused;
            return;
        }
        ++emit.leaves;
        ++emit.glassListed;
        // A kept list takes the UVs again at each assembly (report 105 section 4.50).
        if (walk.program)
            walk.tape->glass.push_back(ShareGlass{
                node, static_cast<uint32_t>(walk.list->dynamicVertices.size()), count});
        baked::appendDynamicCarLeaf(*walk.list, leaf, corners, count, frame.index, walk.material,
                                    walk.condition);
        return;
    }
    ++emit.leaves;
    if (walk.unionMargin > 0.0f && mayBlend(walk, scene, leaf)) ++walk.blendedLeaves;
    if (walk.program && walk.list->polygons >= kMaxProgramLeaves) unionHazard(walk, kUnionTooBig);
    if (skinned && !(leaf.flags & baked::kCarLeafNoGeometry)) {
        emitSkinned(walk, walk.object, object, leaf, node, type, frame);
        return;
    }
    uint32_t colour = 0;
    if (type == 0x03)
        colour = (posedWord(walk, node, 0) & 0x200) ? posedWord(walk, node, 2) : 0xFFFFFFFFu;
    if (walk.tape && type == 0x03) shareSprite(*walk.tape, node, colour);
    baked::appendCarLeaf(*walk.list, static_cast<uint32_t>(walk.object), leaf, frame.index,
                         walk.material, colour, walk.condition);
}

// A gate's side of the eye; a first-eye walk records it for the second eye (CarShare.h), a kept
// list's walk for its replay, marking a configuration gate.
bool gateFront(Walk& walk, const Frame& frame, uintptr_t node) {
    bool front = frontSide(frame, node);
    if (walk.tape)
        shareGate(*walk.tape, word(node, 1), frame.id, front, walk.condition,
                  walk.learn && walk.configGate == node);
    return front;
}

void visit(Walk& walk, uintptr_t node, const Frame& frame, int depth, uintptr_t rootChild);

void visitChild(Walk& walk, uintptr_t node, unsigned index, const Frame& frame, int depth) {
    visit(walk, word(node, index), frame, depth + 1, 0);
}

// -- the union (report 105 section 4.22) ---------------------------------------------------------

// Whether a union walk takes the gate both ways: its plane passes within the margin of this eye,
// |a x + b y + c z + d| <= margin |(a, b, c)|, so an eye within the margin may be on either side.
// A kept list's walk with configurations decides every configuration gate (CarConfig.h).
bool nearGate(Walk& walk, const Frame& frame, uintptr_t node) {
    if (walk.unionMargin <= 0.0f) return false;
    if (walk.learn && configDecides(walk, node)) {
        walk.configGate = node;
        return false;
    }
    if (walk.everyGate || walk.wheelDepth > 0) return true;
    uintptr_t plane = word(node, 1);
    float a = at<float>(plane), b = at<float>(plane + 4), c = at<float>(plane + 8);
    float s = a * frame.eye[0] + b * frame.eye[1] + c * frame.eye[2] + at<float>(plane + 12);
    return s * s <= walk.unionMargin * walk.unionMargin * (a * a + b * b + c * c);
}

void visitUnder(Walk& walk, uintptr_t node, unsigned index, const Frame& frame, int depth,
                uint16_t condition) {
    uint16_t enclosing = walk.condition;
    walk.condition = condition;
    visitChild(walk, node, index, frame, depth);
    walk.condition = enclosing;
}

// The list's draw count; a kept list's walk also seals it there, so the halves of an order node
// never share a draw.
uint32_t sealDraws(Walk& walk) {
    if (!walk.list) return 0;
    auto count = static_cast<uint32_t>(walk.list->draws.size());
    if (walk.program) walk.list->sealed = count;
    return count;
}

// A kept list's walk enters a half of order node `order` (a tape.orders index) met in `outer`.
void enterHalf(Walk& walk, size_t order, uint16_t outer) {
    if (!walk.program) return;
    if (walk.regionCount >= kMaxRegions || order >= kNoOrder) {
        unionHazard(walk, kUnionConditions);
        return;
    }
    uint16_t region = walk.regionCount++;
    walk.regionParent[region] = outer;
    walk.regionOrder[region] = static_cast<uint16_t>(order);
    walk.regionDepth[region] = static_cast<uint16_t>(walk.regionDepth[outer] + 1);
    walk.region = region;
}

// A gate a union walk takes both ways, each side's subtree under its condition. A BSP4 (front 5,
// 3, 2; back 2, 4, 5) or BSP3_ASYM (front 4, 3, 2; back 2, 4) draws w2 and w5 (w4) from either
// side, so they are walked once, in this eye's order, and only w3 (and BSP4's w4) are under a
// condition; the node is recorded for the second eye's count and its verify walk. False when the
// tape has no room for the conditions: the caller decides the gate, and the car is walked again.
bool visitBothWays(Walk& walk, uintptr_t node, uint8_t type, const Frame& frame, int depth) {
    ShareTape& tape = *walk.tape;
    uintptr_t  plane = word(node, 1);
    bool       front = frontSide(frame, node);
    uint16_t   enclosing = walk.condition;
    uint16_t   onFront = shareCondition(tape, plane, frame.id, enclosing, true, front);
    uint16_t   onBack = onFront ? shareCondition(tape, plane, frame.id, enclosing, false, front) : 0;
    if (!onBack) {
        unionHazard(walk, kUnionConditions);
        return false;
    }
    tape.conditions[onFront - 1].node = node;
    tape.conditions[onBack - 1].node = node;
    if (type == 0x06) {
        visitUnder(walk, node, 2, frame, depth, onFront);
        return true;
    }
    if (type == 0x07) {
        visitUnder(walk, node, 2, frame, depth, onFront);
        visitUnder(walk, node, 3, frame, depth, onBack);
        return true;
    }
    unsigned other = type == 0x08 ? 5 : 4;
    uint32_t leaves[2] = {}, blended[2] = {};  // w2's, the other child's
    auto side = [&](unsigned index, uint16_t condition, int which) {
        uint32_t polygons = walk.list->polygons;
        uint32_t blend = walk.blendedLeaves;
        visitUnder(walk, node, index, frame, depth, condition);
        if (which < 0) return;
        leaves[which] = walk.list->polygons - polygons;
        blended[which] = walk.blendedLeaves - blend;
    };
    // The front side's half: w5 (w4) then w3; the back side's: w2 then BSP4's w4. The node is
    // recorded before its children, which the tape's ranges of nested order nodes rely on.
    auto frontHalf = [&] {
        side(other, enclosing, 1);
        side(3, onFront, -1);
    };
    auto backHalf = [&] {
        side(2, enclosing, 0);
        if (type == 0x08) side(4, onBack, -1);
    };
    size_t   k = tape.orders.size();
    uint16_t outer = walk.region;
    tape.orders.emplace_back();
    tape.orders.back().node = node;  // a hazard inside its halves names it (blameRegions)
    uint32_t begin = sealDraws(walk);
    enterHalf(walk, k, outer);
    if (front)
        frontHalf();
    else
        backHalf();
    walk.region = outer;
    uint32_t split = sealDraws(walk);
    auto     splitOrder = static_cast<uint32_t>(tape.orders.size());
    enterHalf(walk, k, outer);
    if (front)
        backHalf();
    else
        frontHalf();
    walk.region = outer;
    ShareOrder& order = tape.orders[k];
    order.node = node;
    order.frameKey = frame.key;
    order.plane = plane;
    order.frame = frame.id;
    order.condition = enclosing;
    order.onFront = onFront;
    order.front = front;
    order.bothDrawn = leaves[0] && leaves[1];
    order.bothBlended = blended[0] && blended[1];
    order.begin = begin;
    order.split = split;
    order.end = sealDraws(walk);
    order.splitOrder = splitOrder;
    order.nested = static_cast<uint32_t>(tape.orders.size());
    return true;
}

// A BSP4 or BSP3_ASYM decided for this eye, in the child order of its side.
void visitOrdered(Walk& walk, uintptr_t node, uint8_t type, const Frame& frame, int depth,
                  bool front) {
    unsigned other = type == 0x08 ? 5 : 4;
    if (front) {
        visitChild(walk, node, other, frame, depth);
        visitChild(walk, node, 3, frame, depth);
        visitChild(walk, node, 2, frame, depth);
    } else {
        visitChild(walk, node, 2, frame, depth);
        if (type == 0x08) visitChild(walk, node, 4, frame, depth);
        visitChild(walk, node, other, frame, depth);
    }
}

// SETREG in a union walk: a write under a condition gives the register a value that depends on
// the eye, unless it writes what every eye already holds; so does a write in a half of an order
// node of a kept list's walk, which the eyes on the two sides make at different times.
void unionWrite(Walk& walk, uint32_t index, int32_t value) {
    if (walk.condition == 0 && walk.region == 0) {
        walk.registerUniform[index] = 1;
        walk.registerCondition[index] = 0;
        walk.registerRegion[index] = 0;
    } else if (!walk.registerUniform[index] || walk.registers[index] != value) {
        walk.registerUniform[index] = 0;
        walk.registerCondition[index] = walk.condition;
        walk.registerRegion[index] = walk.region;
    }
}

// A wheel XFORM's child in a kept list's walk (report 105 section 4.30). GPL points it at one of
// the wheel's three models each pose, by spin rate, so the walk takes every model, each under a
// condition that holds while the child is that model, the memo put back between them as the
// XFORM's pop would. The model GPL's pose set goes last, so what its subtree leaves (a register)
// is what GPL's walk would leave. Walked as posed when the child is none of the models (a lost
// wheel, another layout), when the XFORM's pop keeps the memo (0x16), or the tape is full.
// `current`: the child GPL's pose set (xformWords).
void visitWheel(Walk& walk, uintptr_t node, uint8_t type, unsigned wheel, uintptr_t current,
                const Frame& child, int depth, size_t vertexMark, size_t normalMark) {
    ShareTape& tape = *walk.tape;
    const uintptr_t* models = walk.wheelModels[wheel];
    bool       known = false;
    for (unsigned m = 0; m < kWheelModels; ++m) known = known || models[m] == current;
    if (!known || type == 0x16 || child.id == 0 || child.id == kNoFrameId ||
        child.id > tape.frames.size() || tape.frames[child.id - 1u].node != node ||
        tape.conditions.size() + kWheelModels > kMaxShareConditions) {
        visit(walk, current, child, depth + 1, 0);
        return;
    }
    // GPL's order of the distinct models, the current one moved last.
    uintptr_t order[kWheelModels];
    unsigned  count = 0;
    for (unsigned m = 0; m < kWheelModels; ++m) {
        bool seen = models[m] == current;
        for (unsigned k = 0; k < count; ++k) seen = seen || order[k] == models[m];
        if (!seen) order[count++] = models[m];
    }
    order[count++] = current;
    uint16_t enclosing = walk.condition;
    uint16_t first = 0;
    for (unsigned m = 0; m < count; ++m) {
        uint16_t condition =
            shareChildCondition(tape, order[m], child.id, enclosing, order[m] == current);
        if (!first) first = condition;
    }
    ShareFrame& shared = tape.frames[child.id - 1u];
    shared.switchFirst = first;
    shared.switchCount = static_cast<uint16_t>(count);
    for (unsigned m = 0; m < count; ++m) {
        if (m > 0) {
            memoUndo(*walk.vertexMemo, vertexMark);
            memoUndo(*walk.normalMemo, normalMark);
        }
        walk.condition = static_cast<uint16_t>(first + m);
        visit(walk, order[m], child, depth + 1, 0);
    }
    walk.condition = enclosing;
}

// One node, as gpl_sceneNodeRenderTable 0x570330 dispatches it. `rootChild` replaces word 1 of the
// root MATERIAL, as gpl_carRecordDraw points it at the `car` or `cockpit` handle for the dispatch.
void visit(Walk& walk, uintptr_t node, const Frame& frame, int depth, uintptr_t rootChild) {
    if (walk.abort) return;
    if (walk.detached && !detachedGoOn(walk)) return;
    if (depth > kMaxDepth) return;
    int32_t typeWord = at<int32_t>(node);
    if (typeWord < 0) return;  // every dispatcher skips a negative type word (gpl_nullNode)
    uint8_t type = static_cast<uint8_t>(typeWord);

    switch (type) {
    case 0x00: case 0x01: case 0x02: case 0x10: case 0x12: case 0x1A: case 0x23:
        return;  // empty, IDXLIST and the null handlers

    case 0x17: {  // PRETRANSFORM: gpl_nodePretransformVertices 0x4CF170, {type, n, vertices[n]}
        uint32_t count = word(node, 1);
        for (uint32_t i = 0; i < count && i < kMaxSkinCorners; ++i) {
            uintptr_t gplSlot = 0;
            resolveVertex(walk, word(node, 2 + i), frame, gplSlot);
        }
        return;
    }

    case 0x03:
        ++walk.polygons;
        if (walk.list) emitLeaf(walk, node, type, frame, false);
        return;

    case 0x04: {  // GROUP
        uint32_t count = word(node, 1);
        for (uint32_t i = 0; i < count; ++i) visitChild(walk, node, 2 + i, frame, depth);
        return;
    }

    case 0x05: {  // MATERIAL: gpl_renderNodeDispatch 0x4CE740 by flags & 0x3F
        uint32_t flags = word(node, 2);
        bool     handled = ((kMaterialHandlers >> (flags & 0x3F)) & 1) != 0;
        // The class follows the texture words (one texture or a slot: 1; a list: its count + 1)
        // and flag 2's word.
        unsigned textureWords = 0;
        if (((flags & 1) && !(flags & 0x10)) || (flags & 0x20))
            textureWords = 1;
        else if ((flags & 1) && (flags & 0x10))
            textureWords = word(node, 3) + 1;
        unsigned classWord = 3 + textureWords + ((flags & 2) ? 1 : 0);
        // The handlers with a surface class (flag 4: gpl_materialRender_class 0x4CD190, 0x4CD300, 0x4CE210, 0x4CE4B0 ...)
        // skip the whole subtree when the class is not visible in this pass.
        if (handled && (flags & 4)) {
            uint32_t surfaceClass = word(node, classWord);
            if (!(walk.classVisible & (1u << (surfaceClass & 31)))) return;
        }
        uintptr_t child = rootChild ? rootChild : word(node, 1);
        if (!walk.list || !handled) {
            visit(walk, child, frame, depth + 1, 0);
            return;
        }
        // Every handler restores what it set once its child returns.
        uint16_t  material = walk.material;
        uintptr_t textureList = walk.textureList;
        applyMaterial(walk, node, flags, classWord);
        visit(walk, child, frame, depth + 1, 0);
        walk.material = material;
        walk.textureList = textureList;
        return;
    }

    case 0x06:  // SIDEGATE
        if (nearGate(walk, frame, node) && visitBothWays(walk, node, type, frame, depth)) return;
        if (gateFront(walk, frame, node)) visitChild(walk, node, 2, frame, depth);
        return;
    case 0x07:  // SIDEPICK
        if (nearGate(walk, frame, node) && visitBothWays(walk, node, type, frame, depth)) return;
        visitChild(walk, node, gateFront(walk, frame, node) ? 2 : 3, frame, depth);
        return;
    case 0x08:    // BSP4: front 5, 3, 2; back 2, 4, 5 (gpl_nodePlane08Four 0x4D76B0)
    case 0x09: {  // BSP3_ASYM: front 4, 3, 2; back 2, 4
        if (nearGate(walk, frame, node) && visitBothWays(walk, node, type, frame, depth)) return;
        bool front = gateFront(walk, frame, node);
        visitOrdered(walk, node, type, frame, depth, front);
        return;
    }
    case 0x0A:  // BSP3: every child from either side; the order does not change the count
        visitChild(walk, node, 2, frame, depth);
        visitChild(walk, node, 3, frame, depth);
        visitChild(walk, node, 4, frame, depth);
        return;
    case 0x0B:  // BSP2
        visitChild(walk, node, 2, frame, depth);
        visitChild(walk, node, 3, frame, depth);
        return;

    case 0x0C:  // ALTSET
        visitChild(walk, node, 1, frame, depth);
        return;

    case 0x0D:
    case 0x13:
    case 0x16: {  // XFORM: gpl_nodePoseRender 0x4CC650, child at word 8
        uint32_t words[7];
        xformWords(walk, node, words);
        float pose[12];
        poseMatrix(words, pose);
        Frame child;
        std::memcpy(child.planes, frame.planes, sizeof(child.planes));
        enterPose(frame, pose, walk.planeMask, child);
        enterRoot(frame, pose, child);
        child.index = frame.index;
        child.halfFrom = frame.halfFrom;
        child.node = node;
        child.key = ((frame.key ^ static_cast<uint32_t>(node)) * 2654435761u) ^ (frame.key >> 15);
        child.parentCanon = frame.canon;
        if (walk.list) enterFrame(walk, frame, words, child);
        if (walk.frameCount < kMaxWalkFrames) {
            child.id = walk.frameCount++;
            child.canon = child.id;
            if (walk.unionMargin > 0.0f)
                for (uint16_t k = 1; k < child.id; ++k)
                    if (walk.frames[k].node == node && walk.frames[k].parentCanon == frame.canon) {
                        child.canon = walk.frames[k].canon;
                        break;
                    }
            walk.frames[child.id] = child;
        } else {
            child.id = kNoFrameId;
            child.canon = kNoFrameId;
            ++walk.skin.frameOverflow;
        }
        if (walk.tape) shareFrame(*walk.tape, node, words, frame.id, child.id, pose);
        // gpl_matrixPush saves the memo pools' marks; 0x0D/0x13 (gpl_nodePoseRender 0x4CC650)
        // pop(1) back to them, 0x16 (0x4CC6A0) pops(0) and leaves what its child memoised.
        size_t vertexMark = walk.vertexMemo->pool.size();
        size_t normalMark = walk.normalMemo->pool.size();
        int wheel = -1;
        for (int w = 0; walk.program && w < 4; ++w)
            if (node == walk.wheels[w]) wheel = w;
        walk.wheelDepth += wheel >= 0 ? 1 : 0;
        if (wheel >= 0 && walk.tape)
            visitWheel(walk, node, type, static_cast<unsigned>(wheel), words[6], child, depth,
                       vertexMark, normalMark);
        else
            visit(walk, words[6], child, depth + 1, 0);
        walk.wheelDepth -= wheel >= 0 ? 1 : 0;
        if (type != 0x16) {
            memoUndo(*walk.vertexMemo, vertexMark);
            memoUndo(*walk.normalMemo, normalMark);
        }
        return;
    }

    case 0x0E:  // SUBOBJECT: gpl_objDrawRootNode(word 1 = the Obj3d)
        visit(walk, at<uint32_t>(word(node, 1) + kObjectRoot), frame, depth + 1, 0);
        return;

    case 0x0F: {  // SORTGROUP with no dynamic records: the static child, then the elements
        visitChild(walk, node, 10, frame, depth);
        uint32_t count = word(node, 15);
        for (uint32_t i = 0; i < count; ++i) visitChild(walk, node, 19 + 4 * i, frame, depth);
        return;
    }

    case 0x11: {  // SELECTOR: gpl_nodeSelectorRender 0x4CF9B0, value by gpl_lodEvaluateVariable
        uint32_t kind = word(node, 1);
        uint32_t param = word(node, 2);
        uint32_t op = word(node, 3);
        int32_t  count = static_cast<int32_t>(word(node, 4));
        if (count <= 0 || count > kMaxSelectorCases) return;
        float    value = 0.0f;
        uint16_t referenceFrame = 0;
        switch (kind) {
        case 0: {  // the reference vertex's memoised clip w (0x4CF7F6)
            Transformed reference;
            transformVertex(walk, param, frame, reference);
            value = reference.w * walk.lodScale;
            if (value < 0.0f) value = 0.0f;
            referenceFrame = reference.frame;
            break;
        }
        case 1: value = walk.dlong; break;
        case 2: value = walk.dlat; break;
        case 3: break;  // the clock, taken as case value 0
        case 4: value = static_cast<float>(walk.registers[param & 0x3FF]); break;
        default: break;
        }
        // In a union walk a register written under a condition holds its value only for the
        // eyes that condition holds for; read under it, the value is the write's.
        // A write in a half of an order node is the value only inside that half.
        bool fromWrite = false;
        if (kind == 4 && walk.unionMargin > 0.0f && !walk.registerUniform[param & 0x3FF]) {
            uint32_t index = param & 0x3FF;
            if (!shareImplies(*walk.tape, walk.condition, walk.registerCondition[index]))
                unionHazard(walk, kUnionRegister,
                            blameConditions(walk, walk.registerCondition[index], walk.condition));
            else if (!regionWithin(walk, walk.region, walk.registerRegion[index]))
                unionHazard(walk, kUnionOrder,
                            blameRegions(walk, walk.registerRegion[index], walk.region));
            else
                fromWrite = true;
        }
        if (walk.tape)
            shareSelector(*walk.tape, node, kind, param, referenceFrame, value, walk.condition,
                          fromWrite);
        if (op == 5) {
            long long index = std::llrint(value);
            if (index < 0 || index >= count) return;  // GPL reads garbage there
            visitChild(walk, node, 6 + 2 * static_cast<unsigned>(index), frame, depth);
            return;
        }
        for (int32_t i = 0; i < count; ++i) {
            if (selectorTakes(op, value, wordFloat(node, 5 + 2 * i))) {
                visitChild(walk, node, 6 + 2 * i, frame, depth);
                return;
            }
        }
        return;
    }

    case 0x14: {  // SETREG: gpl_nodeRegisterWrite_type20 0x4CFA50
        uint32_t index = word(node, 1) & 0x3FF;
        auto     value = static_cast<int32_t>(word(node, 2));
        if (walk.unionMargin > 0.0f) unionWrite(walk, index, value);
        walk.registers[index] = value;
        // A write some eyes skip leaves them the pass's value: only one every eye makes stops
        // the second eye's replay from comparing later reads with the pass's file.
        if (walk.tape && walk.condition == 0) shareSetRegister(*walk.tape, index);
        return;
    }

    case 0x15: {  // LINE: gpl_lineDraw3d memoises both ends (0x4C7E68, 0x4C7E7A)
        uintptr_t gplSlot = 0;
        resolveVertex(walk, word(node, 2), frame, gplSlot);
        resolveVertex(walk, word(node, 3), frame, gplSlot);
        return;
    }

    case 0x18: {  // SPHERECULL: the user planes only (gpl_nodeSphereCull 0x4D7120)
        if (walk.planeMask) {
            Transformed centre;
            transformVertex(walk, word(node, 1), frame, centre);
            float radius = wordFloat(node, 2);
            for (int i = 0; i < 6; ++i) {
                if (!(walk.planeMask & (1u << i))) continue;
                float distance = centre.planes[i];
                if (radius + distance < 0.0f && distance - radius < 0.0f) return;
            }
        }
        visitChild(walk, node, 3, frame, depth);
        return;
    }

    case 0x19: {  // BOXCULL: 8 corners, culled when all share an outcode bit (0x4D7210)
        uint32_t common = 2047;
        uint16_t corners[8];
        for (unsigned corner = 0; corner < 8; ++corner) {
            Transformed p;
            transformVertex(walk, word(node, 1 + corner), frame, p);
            common &= (p.outcode << 6) | (walk.cellPlanes ? p.planeBits : 0u);
            corners[corner] = p.frame;
        }
        if (walk.tape) shareBox(*walk.tape, node, corners, common != 0, walk.condition);
        // A kept list's walk draws the box's subtree for every eye the box does not cull, unless
        // the box is a configuration box (a register written inside it, report 105 section 4.24),
        // which it decides for this eye like a plain walk.
        if (walk.program && walk.learn && configDecides(walk, node)) {
            walk.tape->boxes.back().config = true;
        } else if (walk.program) {
            uint16_t drawn = shareBoxCondition(*walk.tape, walk.tape->boxes.size() - 1,
                                               walk.condition, common == 0);
            if (!drawn) {
                unionHazard(walk, kUnionConditions);
                return;
            }
            visitUnder(walk, node, 9, frame, depth, drawn);
            return;
        }
        if (common) return;
        visitChild(walk, node, 9, frame, depth);
        return;
    }

    case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: {
        ++walk.polygons;
        bool skinned = resolveCorners(walk, node, type, frame);
        if (walk.list) emitLeaf(walk, node, type, frame, skinned);
        return;
    }

    default:
        return;
    }
}

// -- state ---------------------------------------------------------------------------------------

struct Addresses {
    uintptr_t polygonsDrawn = 0;
    uintptr_t eyeLocal = 0;
    uintptr_t planeMask = 0;
    uintptr_t planes = 0;
    uintptr_t modelViewSlot = 0;
    uintptr_t projectionSlot = 0;
    uintptr_t lodScale = 0;
    uintptr_t classVisible = 0;
    uintptr_t backgroundTable = 0;
    uintptr_t dlong = 0;
    uintptr_t dlat = 0;
    uintptr_t registersMain = 0;
    uintptr_t registersMirror = 0;
    uintptr_t texturedMask = 0;
    uintptr_t classTextured = 0;
    uintptr_t surfaceClass = 0;
    uintptr_t textureList = 0;
    uintptr_t specularExponent = 0;
    uintptr_t specular = 0;
    uintptr_t lightLocal = 0;
    uintptr_t lightingOn = 0;
    uintptr_t mirrorMip = 0;
};

// One pass's lists, reused pass after pass so their vectors keep their capacity.
struct PassArena {
    std::vector<baked::CarDrawList> lists;
    std::vector<uintptr_t>          records;
    uint32_t                        count = 0;
    std::vector<uintptr_t>          failed;  // records whose walk faulted this pass
    int cockpitWhole = -1;  // why GPL draws the cockpit whole this pass (CockpitWhole), or -1
    bool cockpitOverlay = false;  // the cockpit's list this pass is step 2's (section 4.49)
};

struct State {
    bool      installed = false;
    CarDrawFn body = nullptr;  // gpl_renderNodeDispatch, the body dispatch's own target
    PassArena arena;
    Addresses at;
    CarDrawFn original = nullptr;
    TeamInstanceFn teamInstance = nullptr;
    PoseFn    pose = nullptr;
    ShadowFn  shadow = nullptr;
    // Logged once each: a car's or the cockpit's walk faulted, the cockpit was drawn whole by GPL
    // (CockpitWhole), a car past kMaxListsPerPass.
    bool      saidFaulted = false;
    bool      saidCockpitWhole[kCockpitWholeKinds] = {};
    bool      saidArenaFull = false;
};

State g_car;

// Set around GPL's draw of a car gplvr draws, so the body dispatch returns at once.
volatile uint32_t g_skipBody = 0;

uint32_t polygonsDrawn() { return *reinterpret_cast<const volatile uint32_t*>(g_car.at.polygonsDrawn); }

// Replaces the rel32 of the body dispatch at 0x48703F: ecx is the root MATERIAL, nothing is on
// the stack, as gpl_renderNodeDispatch takes it.
void __fastcall bodyWrapper(void* node, void*) {
    if (g_skipBody) return;
    g_car.body(node, nullptr);
}

// In a baked main pass the residual flush also learns which of its polygons this call submitted
// and where GPL's walk was, to test them by that place (ResidualDepth.h, report 105 section 4.8).
int __cdecl shadowWrapper(void* record, void* node) {
    residualShadowBegin();
    int result = g_car.shadow(record, node);
    residualShadowEnd(bakedPassWalkPosition());
    return result;
}

// The root frame as GPL holds it when the car's record is dispatched: the current model-view
// under the projection, and the eye and the user planes in that same frame. Also hands back the
// two matrices, which a draw list keeps.
void rootFrame(const Addresses& a, Frame& frame, float projection[16], float modelView[16]) {
    std::memcpy(projection, reinterpret_cast<const void*>(at<uint32_t>(a.projectionSlot)),
                16 * sizeof(float));
    std::memcpy(modelView, reinterpret_cast<const void*>(at<uint32_t>(a.modelViewSlot)),
                16 * sizeof(float));
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += projection[row * 4 + k] * modelView[k * 4 + column];
            frame.clip[row * 4 + column] = sum;
        }
    std::memcpy(frame.eye, reinterpret_cast<const void*>(a.eyeLocal), sizeof(frame.eye));
    std::memcpy(frame.planes, reinterpret_cast<const void*>(a.planes), sizeof(frame.planes));
    float identity[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::memcpy(frame.root, identity, sizeof(identity));
    frame.index = 0;
    frame.halfFrom = 0;
    frame.id = 0;
    frame.node = 0;
    frame.key = 0x9E3779B9u;
    frame.canon = 0;
    frame.parentCanon = 0;
}

// A fresh walk from the root frame: GPL's memos as this walk will grow them start empty, with
// the root as frame 0 (a memo GPL made before the walk is read from GPL's own pool).
// A walk on the worker has the worker's memos set already (carWalkDetached).
void beginWalk(Walk& walk, const Frame& root) {
    if (!walk.detached) {
        walk.vertexMemo = &g_vertexMemo;
        walk.normalMemo = &g_normalMemo;
    }
    const baked::CarScene& scene = carBakeScene();
    uint32_t               generation = carBakeGeneration();
    memoBegin(*walk.vertexMemo, scene, generation, false);
    memoBegin(*walk.normalMemo, scene, generation, true);
    walk.frames[0] = root;
    walk.frameCount = 1;
    walk.skin = SkinCounts{};
    walk.knownObjects = 0;
}

// An empty list for this draw, holding the root frame and the MATERIAL state GPL has at
// the car's dispatch as its first frame and material.
void beginList(const Addresses& a, Walk& walk, baked::CarDrawList& list, const Frame& frame,
               const float projection[16], const float modelView[16]) {
    list.clear();
    std::memcpy(list.projection, projection, sizeof(list.projection));
    std::memcpy(list.rootModelView, modelView, sizeof(list.rootModelView));
    if (a.lightLocal)
        std::memcpy(list.light, reinterpret_cast<const void*>(a.lightLocal), sizeof(list.light));
    baked::CarFrame root;
    std::memcpy(root.rootFromLocal, frame.root, sizeof(root.rootFromLocal));
    std::memcpy(root.eye, frame.eye, sizeof(root.eye));
    root.halfFrom = 0;
    list.frames.push_back(root);

    baked::CarMaterial material;
    material.surfaceClass = at<int32_t>(a.surfaceClass);
    material.textured = at<uint8_t>(a.classTextured) != 0 ? 1 : 0;
    material.specularExponent = at<int32_t>(a.specularExponent);
    material.specular = at<float>(a.specular);
    list.materials.push_back(material);

    walk.list = &list;
    walk.emit = EmitCounts{};
    walk.material = 0;
    walk.textureList = at<uint32_t>(a.textureList);
    walk.texturedMask = at<uint32_t>(a.texturedMask);
    walk.prim = 0;
    walk.primSize = 0;
    walk.object = -1;
}

// The walk reads only GPL's own graph, which GPL dispatches right after, but an op-5 selector or
// a node GPL never reaches could still point anywhere; a fault ends the walk, not the game.
bool walkGuarded(Walk& walk, uintptr_t root, uintptr_t rootChild, const Frame& frame) {
    __try {
        visit(walk, root, frame, 0, rootChild);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A walk with the pass's decisions as GPL holds them at this call: the cell's clip planes, the
// class mask, the LOD scale, the selector camera and a copy of the pass's register file.
void setupWalk(const Addresses& a, Walk& walk, uintptr_t registers) {
    walk.polygons = 0;
    walk.planeMask = at<uint32_t>(a.planeMask) & 63u;
    walk.classVisible = at<uint32_t>(a.classVisible);
    walk.lodScale = at<float>(a.lodScale);
    walk.dlong = at<float>(a.dlong);
    walk.dlat = at<float>(a.dlat);
    walk.cellPlanes = true;
    walk.lightingOn = a.lightingOn && at<uint8_t>(a.lightingOn) != 0;
    if (registers)
        std::memcpy(walk.registers, reinterpret_cast<const void*>(registers), sizeof(walk.registers));
    else
        std::memset(walk.registers, 0, sizeof(walk.registers));
    walk.tape = nullptr;
    walk.unionMargin = 0.0f;
    walk.condition = 0;
}

// A union walk's state (report 105 section 4.22), with `margin` from the tape: 0 decides every
// gate.
void beginUnion(Walk& walk, float margin) {
    walk.unionMargin = margin;
    walk.condition = 0;
    walk.blendedLeaves = 0;
    std::memset(walk.unionHazards, 0, sizeof(walk.unionHazards));
    walk.program = false;
    walk.everyGate = false;
    walk.abort = false;
    walk.learn = false;
    walk.configGate = 0;
    walk.learnedGates = 0;
    walk.wheelDepth = 0;
    walk.region = 0;
    walk.regionCount = 1;
    walk.regionParent[0] = 0;
    walk.regionOrder[0] = kNoOrder;
    walk.regionDepth[0] = 0;
    if (margin <= 0.0f) return;
    std::memset(walk.registerUniform, 1, sizeof(walk.registerUniform));
    std::memset(walk.registerCondition, 0, sizeof(walk.registerCondition));
    std::memset(walk.registerRegion, 0, sizeof(walk.registerRegion));
}

// The first reason a union walk's list cannot say what an eye draws (UnionRedone), or -1.
int firstHazard(const Walk& walk) {
    for (int reason = 0; reason < kUnionRedoneCount; ++reason)
        if (walk.unionHazards[reason]) return reason;
    return -1;
}

// A kept list's walk (report 105 section 4.23): a union walk with `margin` (0: every gate both
// ways) and the instance's four wheel XFORMs (report 103 section 1: I+116, 124, 132, 140, whose
// w5 the pose turns by the spin angle) with their models (section 4.30). It decides the
// configuration gates (section 4.24).
void beginProgram(Walk& walk, float margin, uintptr_t instance) {
    beginUnion(walk, margin > 0.0f ? margin : kMaxUnionMargin);
    walk.program = true;
    walk.everyGate = margin <= 0.0f;
    walk.learn = true;
    for (unsigned i = 0; i < 4; ++i) {
        walk.wheels[i] = at<uint32_t>(instance + kInstanceWheels + 8 * i);
        for (unsigned m = 0; m < kWheelModels; ++m)
            walk.wheelModels[i][m] =
                at<uint32_t>(instance + kInstanceWheelModels + 4 * (kWheelModels * i + m));
    }
}

// What a stage-2b walk is about to start from, for sharing it between the eyes (CarShare.h).
ShareCall shareCall(const Walk& walk, const Frame& frame, uintptr_t record, uintptr_t root,
                    uintptr_t handle, int driver, uintptr_t instance) {
    ShareCall call;
    call.instance = instance;
    call.record = record;
    call.root = root;
    call.handle = handle;
    call.driver = driver;
    call.clip = frame.clip;
    call.eye = frame.eye;
    call.lodScale = walk.lodScale;
    call.dlong = walk.dlong;
    call.dlat = walk.dlat;
    call.classVisible = walk.classVisible;
    call.lightingOn = walk.lightingOn;
    call.planeMask = walk.planeMask;
    call.cellPlanes = walk.cellPlanes;
    call.registers = walk.registers;
    call.textureList = static_cast<uint32_t>(walk.textureList);
    call.texturedMask = walk.texturedMask;
    call.poolIndex = carMemoGplPoolIndex();
    call.poolSources = carMemoGplSources();
    return call;
}

// Whether a first-eye walk's list can stand for the second eye's: every input the tape does not
// replay must have been the walk's own.
ShareRefusal shareRefusal(const Walk& walk, const baked::CarDrawList& list, bool walked) {
    const EmitCounts& e = walk.emit;
    if (!walked) return kShareFaulted;
    // The cockpit's glass is left to GPL (report 105 section 4.48); a car has none.
    if (list.polygons + e.glass != walk.polygons) return kShareContract;
    if (walk.skin.liveCorners || walk.skin.liveNormals) return kShareGplMemo;
    if (walk.skin.frameOverflow) return kShareFrames;
    if (e.noObject || e.notLeaf || e.typeDiffers || e.overflow || e.slotNoList ||
        e.glassOverflow || e.glassRefused)
        return kShareLeaves;
    return kShareOk;
}

// How a pass's list of a car or the cockpit came to be (listOurs; kListEmpty: CarPassView.h).
enum ListOutcome { kListFailed, kListRefused, kListEmpty, kListWalked, kListTaken, kListKept };

// The list of a car's first call this pass, in the pass's arena -- the whole car,
// without the cell's clip planes, which only cut GPL's per-cell copies: taken from its kept
// program or from the first eye, else walked. `glassTexture` is mirror.mip's handle for the
// cockpit (report 105 section 4.48), whose glass the walk leaves to GPL, and 0 for a car; with
// `overlay` (step 2, section 4.49) the glass is listed and the list drawn over the eye image. A
// walk that faulted, or a cockpit list that does not hold every polygon walked but the glass
// left to GPL, is not listed and adds the record to the pass's failed ones.
ListOutcome listOurs(State& s, uintptr_t record, int driver, uintptr_t instance,
                     uintptr_t root, uintptr_t handle, uintptr_t registers, bool mirror,
                     uint32_t glassTexture, bool overlay) {
    PassArena& arena = s.arena;
    if (arena.lists.size() <= arena.count) {
        arena.lists.emplace_back();
        arena.records.push_back(0);
    }
    baked::CarDrawList& list = arena.lists[arena.count];
    static Walk walk;
    setupWalk(s.at, walk, registers);
    walk.planeMask = 0;
    walk.cellPlanes = false;
    walk.glassTexture = glassTexture;
    walk.glassOurs = overlay;
    Frame frame;
    float projection[16], modelView[16];
    rootFrame(s.at, frame, projection, modelView);
    beginList(s.at, walk, list, frame, projection, modelView);
    list.driver = driver;
    list.cockpit = glassTexture != 0;
    list.overlay = overlay;
    list.mirror = mirror;
    // The cockpit's translucent parts (the windscreen) go after all of the track's walk-order
    // content, where nothing GPL's walk reaches later paints over them.
    list.walkSlot = glassTexture ? baked::kWalkSlotLast : bakedPassWalkPosition();

    // The car's kept list serves both eyes and later frames (report 105 section 4.23); without
    // one, in VR the second eye takes the first eye's list when this walk would build the same.
    ShareCall      share = shareCall(walk, frame, record, root, handle, driver, instance);
    ProgramOutcome program = carProgramTake(share, list);
    // A build handed to the worker (report 105 section 4.61): this call walks the car as before.
    if (program == kProgramQueued) program = kProgramWalk;
    if (program == kProgramEmpty) return kListEmpty;
    if (program == kProgramReused || program == kProgramVerify)
        carShareTapeSkip(share, arena.count);
    if (program == kProgramReused) {
        arena.records[arena.count++] = record;
        return kListKept;
    }
    ShareOutcome shared = program == kProgramWalk ? carShareTake(share, list) : kShareWalk;
    if (shared == kShareReused) {
        arena.records[arena.count++] = record;
        return kListTaken;
    }
    float programMargin = 0.0f;
    walk.tape = program == kProgramBuild ? carProgramTapeBegin(share, programMargin) : nullptr;
    if (program == kProgramBuild && !walk.tape) program = kProgramWalk;
    if (program == kProgramWalk) walk.tape = carShareTapeBegin(share, arena.count);

    if (program == kProgramBuild)
        beginProgram(walk, programMargin, instance);
    else
        beginUnion(walk, walk.tape ? walk.tape->margin : 0.0f);
    beginWalk(walk, frame);
    bool walked = walkGuarded(walk, root, handle, frame);
    int  redone = firstHazard(walk);
    // A kept list's walk the car's program cannot keep (a hazard, a fault, a refused leaf) is
    // thrown away, and the car is walked as in section 4.22, with CarShare's tape.
    if (walk.program) {
        if (carProgramTapeEnd(*walk.tape, walked ? redone : -1, shareRefusal(walk, list, walked),
                              list, walk.learnedGates)) {
            carShareTapeSkip(share, arena.count);
            walk.tape = nullptr;
        } else {
            setupWalk(s.at, walk, registers);
            walk.planeMask = 0;
            walk.cellPlanes = false;
            beginList(s.at, walk, list, frame, projection, modelView);
            walk.tape = carShareTapeBegin(share, arena.count);
            beginUnion(walk, walk.tape ? walk.tape->margin : 0.0f);
            beginWalk(walk, frame);
            walked = walkGuarded(walk, root, handle, frame);
            redone = firstHazard(walk);
        }
    }
    // A union walk whose list cannot say what an eye draws (a memoised frame or a register that
    // depends on a side, or no room for the conditions) is thrown away, and the car is walked
    // again deciding every gate (report 105 section 4.22).
    if (walked && walk.unionMargin > 0.0f && redone >= 0) {
        setupWalk(s.at, walk, registers);
        walk.planeMask = 0;
        walk.cellPlanes = false;
        // CarDrawList::clear keeps the driver, kinds and walk slot set above.
        beginList(s.at, walk, list, frame, projection, modelView);
        walk.tape = carShareTapeBegin(share, arena.count);
        if (walk.tape) walk.tape->margin = 0.0f;
        beginUnion(walk, 0.0f);
        beginWalk(walk, frame);
        walked = walkGuarded(walk, root, handle, frame);
    }
    ShareRefusal refusal = shareRefusal(walk, list, walked);
    if (walk.tape) carShareTapeEnd(*walk.tape, refusal, list);
    walk.tape = nullptr;
    if (program == kProgramVerify) carProgramVerify(list, walked);
    if (!walked) {
        if (!s.saidFaulted) {
            s.saidFaulted = true;
            logf("baked cars: a walk of driver %d's %s faulted -- GPL draws it whole on its later "
                 "calls of the pass (logged once)",
                 driver, glassTexture ? "cockpit" : "car");
        }
        arena.failed.push_back(record);
        return kListFailed;
    }
    // A car's list is drawn with what it holds; a cockpit list missing a polygon is not, and GPL
    // draws the cockpit whole (report 105 section 4.48).
    if (glassTexture && (refusal == kShareContract || refusal == kShareLeaves)) {
        arena.failed.push_back(record);
        return kListRefused;
    }
    arena.records[arena.count++] = record;
    return kListWalked;
}

// A car call in a baked main or mirror pass. Every call runs GPL's draw with the body dispatch
// skipped, so the pose, the helmet tint, register 11 and the blob shadow (clipped to this call's
// cell) stay GPL's. The car's first call this pass then makes its list (listOurs). The walk comes
// AFTER GPL's draw because that draw writes this car's helmet colour into the BILLBOARDs of the
// team model all cars of the team share (0x486ECB), and leaves the MATERIAL state as its body
// dispatch would see it. A car whose walk faulted is drawn by GPL whole on its later calls this
// pass (its first call lost the body), as is a car past the arena's limit. A mirror pass is the
// same with the mirror's registers and camera.
void drawOurs(State& s, void* recordPointer, int driver, uintptr_t instance, uintptr_t root,
              uintptr_t handle, uintptr_t registers, bool mirror) {
    PassArena& arena = s.arena;
    auto       record = reinterpret_cast<uintptr_t>(recordPointer);
    bool listed = false;
    for (uint32_t i = 0; i < arena.count && !listed; ++i) listed = arena.records[i] == record;
    bool failed = false;
    for (size_t i = 0; i < arena.failed.size() && !listed && !failed; ++i)
        failed = arena.failed[i] == record;
    bool fresh = !listed && !failed && arena.count < kMaxListsPerPass;
    if (!listed && !failed && !fresh && !s.saidArenaFull) {
        s.saidArenaFull = true;
        logf("baked cars: more than %u cars in one pass -- GPL draws the rest whole (logged once)",
             kMaxListsPerPass);
    }

    g_skipBody = listed || fresh ? 1u : 0u;
    s.original(recordPointer, nullptr);
    g_skipBody = 0;
    if (!fresh) return;
    ListOutcome made =
        listOurs(s, record, driver, instance, root, handle, registers, mirror, 0, false);
    if (made >= kListWalked) carProgramListed(arena.lists[arena.count - 1]);
}

// Report 105 section 4.48: the player's cockpit in a baked main pass is
// gplvr's but for its mirror glass. Its first call this pass makes its list as a car's is made
// (listOurs: walked whole, or the first eye's taken; in step 2 also its kept program, section
// 4.50), the glass left out.
// Every call then runs GPL's draw under its cell's planes with the node filter handing GPL the
// glass alone (CockpitGlass.h), so mirror.mip, its flip and the UVs MirrorUv rewrites stay GPL's,
// and the residual flush tests the glass against the baked depth, the cockpit's included. The
// walk comes BEFORE GPL's draw, unlike a car's: the cockpit's branch of GPL's draw (0x487032)
// only points the root's word 1 at the handle, which the walk does itself, and the filter needs
// the glass the walk finds. A cockpit call outside a baked main pass, or one gplvr cannot list, is
// drawn whole by GPL on every call of the pass (CockpitWhole; a refusal is logged once by why).
// Step 2 (section 4.49): when the pass can draw it over its eye image, the list holds the glass
// too and GPL's draw hands GPL no leaf; it still runs, for the registers the cockpit's SETREG
// nodes write and for any line, unless CockpitDraw.h skips it and writes those registers itself
// (section 4.50).
void drawCockpit(State& s, void* recordPointer, int driver, uintptr_t instance, uintptr_t root,
                 uintptr_t handle, uintptr_t registers, bool mainPass) {
    PassArena& arena = s.arena;
    auto       record = reinterpret_cast<uintptr_t>(recordPointer);
    uint32_t   mirrorMip = s.at.mirrorMip ? at<uint32_t>(s.at.mirrorMip) : 0;
    int whole = arena.cockpitWhole;
    if (!mainPass)
        whole = kCockpitNotMainFile;
    else if (!bakedPassCollecting() || bakedPassMirror() || !bakedPassDrawsCars())
        whole = kCockpitNotBaked;
    else if (!bakedFilterSeesEveryLeaf())
        whole = kCockpitFilter;
    else if (!mirrorMip)
        whole = kCockpitNoMirrorMip;
    bool listed = false;
    for (uint32_t i = 0; i < arena.count && !listed; ++i) listed = arena.records[i] == record;
    if (whole < 0 && !listed && arena.count >= kMaxListsPerPass) {
        whole = kCockpitArenaFull;
    } else if (whole < 0 && !listed) {
        cockpitGlassFor(handle, carBakeGeneration());
        arena.cockpitOverlay = bakedCockpitOverlayReady(mirrorMip) == kOverlayReady;
        ListOutcome made = listOurs(s, record, driver, instance, root, handle, registers, false,
                                    mirrorMip, arena.cockpitOverlay);
        if (made == kListFailed) arena.cockpitWhole = whole = kCockpitFaulted;
        if (made == kListRefused) arena.cockpitWhole = whole = kCockpitRefused;
    }
    // Outside a baked main pass the cockpit is GPL's as a matter of course.
    if (whole > kCockpitNotMainFile && !s.saidCockpitWhole[whole]) {
        static const char* const kWhy[kCockpitWholeKinds] = {
            "",
            "",
            "the node filter does not see the lit polygon types",
            "mirror.mip's handle is not known",
            "past the pass's list limit",
            "its walk faulted",
            "its list did not hold every polygon walked but the glass"};
        s.saidCockpitWhole[whole] = true;
        logf("baked cars: GPL draws the cockpit whole -- %s (logged once)", kWhy[whole]);
    }

    static const uintptr_t kNoLeaf = 0;
    bool             ours = whole < 0;
    bool             overlay = ours && arena.cockpitOverlay;
    // In step 2 GPL's draw hands GPL nothing; it is skipped while what it leaves can be written.
    int              gpl = overlay ? cockpitGplDrawAsk(handle, carBakeGeneration(), registers)
                                   : kCockpitGplRun;
    if (gpl == kCockpitGplSkip) return;
    uint32_t         count = 0;
    const uintptr_t* glass = overlay ? &kNoLeaf : cockpitGlassNodes(count);
    uint32_t         before = polygonsDrawn();
    if (ours) bakedCockpitGlassOnly(glass, count);
    s.original(recordPointer, nullptr);
    if (ours) bakedCockpitGlassOnly(nullptr, 0);
    if (!ours) return;
    // What the filter handed GPL in that draw; GPL's skip check reads it (CockpitDraw.h).
    uint32_t passed = 0, dropped = 0, lines = 0;
    bakedCockpitGlassTake(passed, dropped, lines);
    if (gpl == kCockpitGplCheck)
        cockpitGplDrawChecked(registers, passed, lines, polygonsDrawn() - before);
}

// GPL's car draw, through the car vtable slot. A call outside a baked pass of its kind (the
// renderer switched to vanilla, a pass it refused, a pass of another kind) goes to GPL whole.
void __fastcall carDraw(void* recordPointer, void*) {
    State& s = g_car;
    auto record = reinterpret_cast<uintptr_t>(recordPointer);
    int  driver = at<int32_t>(record + kRecordDriver);
    auto instance = reinterpret_cast<uintptr_t>(s.teamInstance(driver));
    s.pose(reinterpret_cast<void*>(instance), recordPointer);

    bool      cockpit = at<uint32_t>(record + kRecordCockpit) != 0;
    uintptr_t object = at<uint32_t>(record + kRecordDriverObject);
    if (!object) object = at<uint32_t>(instance);
    uintptr_t root = at<uint32_t>(object + kObjectRoot);
    uintptr_t handle = at<uint32_t>(instance + (cockpit ? kInstanceCockpit : kInstanceCar));

    uintptr_t registers = at<uint32_t>(s.at.backgroundTable);
    bool      mirror = registers == s.at.registersMirror;
    bool      mainPass = registers == s.at.registersMain;
    // The register file names the pass (gpl_sceneRender switches it at 0x487766 and 0x487C9C); a
    // call is gplvr's only inside a baked pass of the same kind.
    if (cockpit) {
        drawCockpit(s, recordPointer, driver, instance, root, handle, registers, mainPass);
        return;
    }
    if ((mainPass || mirror) && bakedPassCollecting() && bakedPassMirror() == mirror &&
        bakedPassDrawsCars()) {
        drawOurs(s, recordPointer, driver, instance, root, handle, registers, mirror);
        return;
    }
    s.original(recordPointer, nullptr);
}

// The rel32 target of the `call` at `site`, or 0 when the byte there is not E8.
uintptr_t callTarget(uintptr_t site) {
    if (at<uint8_t>(site) != 0xE8) return 0;
    return site + 5 + static_cast<uintptr_t>(at<int32_t>(site + 1));
}

}  // namespace

void carWalkInstall() {
    State& s = g_car;
    if (s.installed) return;

    Addresses& a = s.at;
    a.polygonsDrawn = rebase(kPolygonsDrawn);
    a.eyeLocal = rebase(kEyeLocal);
    a.planeMask = rebase(kUserPlaneMask);
    a.planes = rebase(kUserPlanes);
    a.modelViewSlot = rebase(kModelViewSlot);
    a.projectionSlot = rebase(kProjectionSlot);
    a.lodScale = rebase(kLodScreenScale);
    a.classVisible = rebase(kClassVisibleMask);
    a.backgroundTable = rebase(kBackgroundTable);
    a.dlong = rebase(kSelectorDlong);
    a.dlat = rebase(kSelectorDlat);
    a.registersMain = rebase(kRegistersMain);
    a.registersMirror = rebase(kRegistersMirror);
    a.lightingOn = rebase(kLightingOn);
    a.texturedMask = rebase(kTexturedMask);
    a.classTextured = rebase(kClassTextured);
    a.surfaceClass = rebase(kSurfaceClass);
    a.textureList = rebase(kTextureList);
    a.specularExponent = rebase(kSpecularExponent);
    a.specular = rebase(kSpecular);
    a.lightLocal = rebase(kLightLocal);
    a.mirrorMip = rebase(kMirrorMip);

    // Every check comes before the first patch, so a refusal leaves GPL drawing every car.
    // GPL's own vertex memo, for what it memoised before a walk (CarMemo.h).
    carMemoGplBind();
    uintptr_t slot = rebase(kCarVtableSlot);
    if (!a.polygonsDrawn || !a.eyeLocal || !a.planeMask || !a.planes || !a.modelViewSlot ||
        !a.projectionSlot || !a.lodScale || !a.classVisible || !a.backgroundTable || !a.dlong ||
        !a.dlat || !a.registersMain || !a.registersMirror || !slot || !a.texturedMask ||
        !a.classTextured || !a.surfaceClass || !a.textureList || !a.specularExponent ||
        !a.specular || !a.lightLocal) {
        logf("baked cars: NOT installed -- rebase refused one of the game's addresses (%s); GPL "
             "draws the cars",
             rebaseSummary());
        return;
    }

    // Is this the code report 103 read? The slot must hold the stock draw, which must start as
    // read, and its two calls must reach the functions named above.
    if (at<uint32_t>(slot) != kCarRecordDraw ||
        std::memcmp(reinterpret_cast<const void*>(kCarRecordDraw), kCarRecordDrawStart,
                    sizeof(kCarRecordDrawStart)) != 0) {
        logf("baked cars: NOT installed -- the car vtable slot %08X holds %08X, or "
             "gpl_carRecordDraw does not start as read; expected %08X",
             static_cast<unsigned>(slot), at<uint32_t>(slot), static_cast<unsigned>(kCarRecordDraw));
        return;
    }
    uintptr_t team = callTarget(kTeamInstanceCall);
    uintptr_t pose = callTarget(kPoseCall);
    if (!team || !pose) {
        logf("baked cars: NOT installed -- the calls in gpl_carRecordDraw reach %08X and %08X "
             "(expected the team lookup and %08X)",
             static_cast<unsigned>(team), static_cast<unsigned>(pose),
             static_cast<unsigned>(kPoseFunction));
        return;
    }
    if (pose != kPoseFunction) {
        const char* why = "";
        uintptr_t   poseData = carPoseWrapperData(pose, kPoseFunction, why);
        if (!poseData) {
            logf("baked cars: NOT installed -- the pose call in gpl_carRecordDraw reaches %08X, "
                 "neither gpl_carModelPoseFromRecord %08X nor its known wrapper: %s",
                 static_cast<unsigned>(pose), static_cast<unsigned>(kPoseFunction), why);
            return;
        }
        logf("baked cars: the pose call in gpl_carRecordDraw reaches %08X, this image's wrapper of "
             "gpl_carModelPoseFromRecord %08X: it stores the float at %08X into R+248 (the last "
             "dial) and calls it. Every car is posed through the wrapper, as GPL's draw poses it",
             static_cast<unsigned>(pose), static_cast<unsigned>(kPoseFunction),
             static_cast<unsigned>(poseData));
    }
    // The body dispatch must be the call report 105 section 4.3 read, behind the `mov ecx, esi`
    // that hands it the root MATERIAL.
    uintptr_t body = callTarget(kBodyCall);
    if (body != kNodeDispatch ||
        std::memcmp(reinterpret_cast<const void*>(kBodyCall - sizeof(kBodyCallBefore)),
                    kBodyCallBefore, sizeof(kBodyCallBefore)) != 0) {
        logf("baked cars: NOT installed -- the body dispatch at %08X reaches %08X (expected %08X "
             "behind 8B CE); GPL draws the cars",
             static_cast<unsigned>(kBodyCall), static_cast<unsigned>(body),
             static_cast<unsigned>(kNodeDispatch));
        return;
    }
    if (!carBakeBind()) return;
    s.teamInstance = reinterpret_cast<TeamInstanceFn>(team);
    s.pose = reinterpret_cast<PoseFn>(pose);
    s.original = reinterpret_cast<CarDrawFn>(kCarRecordDraw);
    s.shadow = reinterpret_cast<ShadowFn>(kShadowFunction);
    s.body = reinterpret_cast<CarDrawFn>(kNodeDispatch);

    // The body wrapper and the shadow wrappers pass every call through until the slot is ours.
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&bodyWrapper) - (kBodyCall + 5));
    if (!patchBytes(reinterpret_cast<void*>(kBodyCall + 1), &rel, sizeof(rel))) {
        logf("baked cars: NOT installed -- could not write the body dispatch at %08X; GPL draws "
             "the cars",
             static_cast<unsigned>(kBodyCall));
        return;
    }
    unsigned wrapped = 0;
    for (uintptr_t site : kShadowCalls) {
        if (callTarget(site) != kShadowFunction) continue;
        auto shadowRel =
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(&shadowWrapper) - (site + 5));
        if (patchBytes(reinterpret_cast<void*>(site + 1), &shadowRel, sizeof(shadowRel))) ++wrapped;
    }
    void* hook = reinterpret_cast<void*>(&carDraw);
    if (!patchBytes(reinterpret_cast<void*>(slot), &hook, sizeof(hook))) {
        logf("baked cars: NOT installed -- could not write the car vtable slot %08X; GPL draws "
             "the cars",
             static_cast<unsigned>(slot));
        return;
    }
    s.installed = true;
    logf("baked cars: installed on the car vtable slot %08X (team lookup %08X, pose %08X), body "
         "dispatch %08X wrapped, shadow calls wrapped %u of 2 -- in a baked pass gplvr draws each "
         "car and the cockpit, GPL only the shadows; car calls outside one stay GPL's",
         static_cast<unsigned>(slot), static_cast<unsigned>(team), static_cast<unsigned>(pose),
         static_cast<unsigned>(kBodyCall), wrapped);
    carProgramInstall();
}

void carWalkPassBegin(bool mirror, int pane) {
    PassArena& arena = g_car.arena;
    // The pass that ends keeps its lists for the other eye; the arena may get other vectors back.
    carSharePassBegin(mirror, g_car.installed, arena.lists, arena.count);
    carProgramPassBegin(mirror, g_car.installed, pane);
    arena.records.resize(arena.lists.size());
    arena.count = 0;
    arena.failed.clear();
    arena.cockpitWhole = -1;
    arena.cockpitOverlay = false;
}

const baked::CarDrawList* carWalkPassLists(uint32_t& count) {
    count = g_car.arena.count;
    return count ? g_car.arena.lists.data() : nullptr;
}

// -- builds on the worker (CarBuildWorker.h, report 105 section 4.61) -----------------------------

namespace {

// The posed nodes' words 0-8 (an XFORM is 36 bytes; a BILLBOARD's colour is w2), and the wheels
// and their models beginProgram would read; a fault refuses the snapshot.
bool snapshotPoseGuarded(uintptr_t instance, CarBuildSnapshot& s) {
    __try {
        s.posedCount = 0;
        for (unsigned offset : kPosedOffsets) {
            uintptr_t node = at<uint32_t>(instance + offset);
            bool      seen = !node;
            for (uint32_t i = 0; i < s.posedCount && !seen; ++i) seen = s.posed[i].node == node;
            if (seen) continue;
            CarPosedNode& posed = s.posed[s.posedCount++];
            posed.node = node;
            std::memcpy(posed.words, reinterpret_cast<const void*>(node), sizeof(posed.words));
        }
        for (unsigned i = 0; i < 4; ++i) {
            s.wheels[i] = at<uint32_t>(instance + kInstanceWheels + 8 * i);
            for (unsigned m = 0; m < kWheelModels; ++m)
                s.wheelModels[i][m] =
                    at<uint32_t>(instance + kInstanceWheelModels + 4 * (kWheelModels * i + m));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool carWalkSnapshotPose(uintptr_t instance, CarBuildSnapshot& snapshot) {
    return instance && snapshotPoseGuarded(instance, snapshot);
}

// listOurs's build (setupWalk, rootFrame, beginList, beginProgram, the walk), every input from the
// snapshot; on the worker only, which this Walk, these memos and this cache are.
void carWalkDetached(const CarBuildSnapshot& s, ShareTape& tape, baked::CarDrawList& list,
                     CarDetachedWalk& out) {
    static Walk     walk;
    static Memo     vertexMemo;
    static Memo     normalMemo;
    static Detached detached;
    detached = Detached{};
    detached.snapshot = &s;
    detached.learned = out.learnedGates;
    walk.detached = &detached;
    walk.vertexMemo = &vertexMemo;
    walk.normalMemo = &normalMemo;

    walk.polygons = 0;
    walk.planeMask = 0;
    walk.classVisible = s.classVisible;
    walk.lodScale = s.lodScale;
    walk.dlong = s.dlong;
    walk.dlat = s.dlat;
    walk.cellPlanes = false;
    walk.lightingOn = s.lightingOn;
    std::memcpy(walk.registers, s.registers, sizeof(walk.registers));
    walk.glassTexture = 0;
    walk.glassOurs = false;

    Frame frame;
    std::memcpy(frame.clip, s.clip, sizeof(frame.clip));
    std::memcpy(frame.eye, s.eye, sizeof(frame.eye));
    std::memset(frame.planes, 0, sizeof(frame.planes));
    float identity[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::memcpy(frame.root, identity, sizeof(identity));
    frame.index = 0;
    frame.halfFrom = 0;
    frame.id = 0;
    frame.node = 0;
    frame.key = 0x9E3779B9u;
    frame.canon = 0;
    frame.parentCanon = 0;

    list.clear();
    std::memcpy(list.projection, s.projection, sizeof(list.projection));
    std::memcpy(list.rootModelView, s.rootModelView, sizeof(list.rootModelView));
    std::memcpy(list.light, s.light, sizeof(list.light));
    baked::CarFrame root;
    std::memcpy(root.rootFromLocal, frame.root, sizeof(root.rootFromLocal));
    std::memcpy(root.eye, frame.eye, sizeof(root.eye));
    root.halfFrom = 0;
    list.frames.push_back(root);
    baked::CarMaterial material;
    material.surfaceClass = s.surfaceClass;
    material.textured = s.textured;
    material.specularExponent = s.specularExponent;
    material.specular = s.specular;
    list.materials.push_back(material);
    list.driver = s.driver;
    list.cockpit = false;
    list.overlay = false;
    list.mirror = s.mirror;
    list.walkSlot = s.walkSlot;
    list.flagman = false;
    walk.list = &list;
    walk.emit = EmitCounts{};
    walk.material = 0;
    walk.textureList = s.textureList;
    walk.texturedMask = s.texturedMask;
    walk.prim = 0;
    walk.primSize = 0;
    walk.object = -1;

    walk.tape = &tape;
    beginUnion(walk, s.margin > 0.0f ? s.margin : kMaxUnionMargin);
    walk.program = true;
    walk.everyGate = s.margin <= 0.0f;
    walk.learn = true;
    std::memcpy(walk.wheels, s.wheels, sizeof(walk.wheels));
    std::memcpy(walk.wheelModels, s.wheelModels, sizeof(walk.wheelModels));
    beginWalk(walk, frame);
    out.walked = walkGuarded(walk, s.root, s.handle, frame);
    out.hazard = out.walked ? firstHazard(walk) : -1;
    out.refusal = shareRefusal(walk, list, out.walked);
    out.learned = walk.learnedGates;
    out.need = detached.need;
    out.needAt = detached.needAt;
    walk.tape = nullptr;
    walk.list = nullptr;
    walk.detached = nullptr;
}

}  // namespace gplvr
