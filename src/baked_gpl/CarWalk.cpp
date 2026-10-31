#include "baked_gpl/CarWalk.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"

#include "baked/CarScene.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarConfig.h"
#include "baked_gpl/CarMemo.h"
#include "baked_gpl/CarProgram.h"
#include "baked_gpl/CarShare.h"
#include "baked_gpl/CarWalkStats.h"
#include "baked_gpl/ResidualDepth.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <algorithm>
#include <vector>

namespace gplvr {
namespace {

using namespace carwalk;

// -- game addresses (gplc67.exe; reports 103 and 105, IDA 2026-10-24) ------------------------------

// gpl_carRenderRecordVtbl[0], the only entry into a car's draw (report 103 section 11): reached
// with ecx = the car render record R from gpl_dynamicObjectSelectNode 0x4871F0 (`jmp [eax]`, once
// per cell the car is in) and the mirror-detail-0 loop 0x487BC2 (`call [eax]`).
constexpr uintptr_t kCarVtableSlot = 0x00539AA4;
constexpr uintptr_t kCarRecordDraw = 0x00486E50;  // gpl_carRecordDraw, thiscall, no arguments
// Inside gpl_carRecordDraw, both rel32 calls: gpl_driverGetTeamModelInstance(R+32) and
// gpl_carModelPoseFromRecord(I, R) 0x46D490, both cdecl.
constexpr uintptr_t kTeamInstanceCall = 0x00486E5D;
constexpr uintptr_t kPoseCall = 0x00486E69;
constexpr uintptr_t kPoseFunction = 0x0046D490;
// gpl2020demo.exe's mod layer points the pose call at a wrapper in its own code section (0x989140
// there, report 105 section 4.19): it stores one float of the mod's data into R+248, the last
// dial gpl_carModelPoseDials turns, then calls gpl_carModelPoseFromRecord(I, R) with its own two
// arguments, cdecl. Called in place of the stock function it poses a car as GPL's draw does.
// Matched byte for byte but for the data address, which must be a writable global of the image,
// and the inner call, which must reach kPoseFunction.
constexpr unsigned char kPoseWrapperHead[] = {0x55, 0x8B, 0xEC,        // push ebp; mov ebp, esp
                                              0x56, 0x8B, 0x75, 0x0C,  // push esi; mov esi, [ebp+12]
                                              0xD9, 0x05};             // fld dword [data]
constexpr unsigned char kPoseWrapperBody[] = {0xD9, 0x9E, 0xF8, 0x00, 0x00, 0x00,  // fstp [esi+248]
                                              0x56, 0xFF, 0x75, 0x08};  // push esi; push [ebp+8]
// After the inner call: pop ecx; pop ecx; pop esi; pop ebp; ret.
constexpr unsigned char kPoseWrapperTail[] = {0x59, 0x59, 0x5E, 0x5D, 0xC3};
constexpr unsigned kPoseWrapperData = sizeof(kPoseWrapperHead);
constexpr unsigned kPoseWrapperBodyAt = kPoseWrapperData + 4;
constexpr unsigned kPoseWrapperCall = kPoseWrapperBodyAt + sizeof(kPoseWrapperBody);
constexpr unsigned kPoseWrapperTailAt = kPoseWrapperCall + 5;
constexpr unsigned kPoseWrapperSize = kPoseWrapperTailAt + sizeof(kPoseWrapperTail);
// Its two calls of gpl_carDrawTrackProjected 0x487060 (R, node), cdecl: the projected car shadow
// (shadow mode 2, never under roglv2) and the blob shadow. Wrapped to count their polygons apart.
constexpr uintptr_t kShadowCalls[] = {0x00486F52, 0x0048702A};
constexpr uintptr_t kShadowFunction = 0x00487060;
// Stage 2b: its body dispatch, `mov ecx, esi; call gpl_renderNodeDispatch` (8B CE E8 rel32 at
// 0x48703D), the root MATERIAL with word 1 swapped to the `car` or `cockpit` handle. With the two
// shadow calls it is the only call in gpl_carRecordDraw that emits polygons (IDA, task "R2 stage
// 2b": the others are the team and driver lookups, the pose, lighting / LOD / textured-mask
// get-set around the never-taken shadow mode 2, the wheel states and the register write).
constexpr uintptr_t kBodyCall = 0x0048703F;
constexpr uintptr_t kNodeDispatch = 0x004CE740;  // gpl_renderNodeDispatch, thiscall (ecx = node)
constexpr unsigned char kBodyCallBefore[] = {0x8B, 0xCE};  // mov ecx, esi

// gpl_carRecordDraw's first 13 bytes, up to the call of the team lookup.
constexpr unsigned char kCarRecordDrawStart[] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57,
                                                 0x8B, 0xF9, 0x8B, 0x47, 0x20, 0x50};

// Reference-link data, placed through rebase().
constexpr uintptr_t kPolygonsDrawn = 0x0086A0E4;    // gpl_statGfxPolysDrawn, ++ per polygon emitted
constexpr uintptr_t kEyeLocal = 0x008676A0;         // gpl_eyePositionLocal, the eye in the current frame
constexpr uintptr_t kUserPlaneMask = 0x00580818;    // gpl_userClipPlaneMask
constexpr uintptr_t kUserPlanes = 0x00580734;       // gpl_userClipPlanes, 6 x {a, b, c, d}, current frame
constexpr uintptr_t kModelViewSlot = 0x0056FE28;    // -> the current 4x4 model-view, row-major
constexpr uintptr_t kProjectionSlot = 0x0056FE2C;   // -> the 4x4 projection; MVP = P x MV (0x4E5B57)
constexpr uintptr_t kLodScreenScale = 0x0056FE64;   // gpl_lodScreenScale
constexpr uintptr_t kClassVisibleMask = 0x0056FE24;  // gpl_surfaceClassVisibleMask
constexpr uintptr_t kBackgroundTable = 0x00872168;  // gpl_backgroundTable, the pass's register file
constexpr uintptr_t kSelectorDlong = 0x0087215C;    // gpl_selectorCameraDlong
constexpr uintptr_t kSelectorDlat = 0x00872160;     // gpl_selectorCameraDlat
constexpr uintptr_t kRegistersMain = 0x0076D768;    // gpl_renderRegistersMain
constexpr uintptr_t kRegistersMirror = 0x007702D8;  // gpl_renderRegistersMirror
// Stage 2: the MATERIAL state at the car's dispatch, which the walk carries down as GPL's
// handlers do (0x4CD300 and siblings, report 105 section 4.2).
constexpr uintptr_t kTexturedMask = 0x0056FE20;     // gpl_surfaceClassTexturedMaskLive
constexpr uintptr_t kClassTextured = 0x00872154;    // gpl_materialClassTextured, a byte
constexpr uintptr_t kSurfaceClass = 0x00570218;     // gpl_materialSurfaceClass
constexpr uintptr_t kTextureList = 0x00872158;      // gpl_materialTextureList -> {count, handles[]}
constexpr uintptr_t kSpecularExponent = 0x00580810;  // gpl_lightSpecularExponent, an int
constexpr uintptr_t kSpecular = 0x00580814;          // gpl_lightSpecular, a float
// The light direction in the current frame (gpl_lightDirectionSet 0x4C5210 writes it, the
// rotations carry it, gpl_matrixPush / Pop save and restore it): at the car's dispatch, the
// light in the dispatch frame.
constexpr uintptr_t kLightLocal = 0x005807D4;
// gpl_lightingOn: the lit kernels light (and memoise) a polygon's normals only while it is set.
constexpr uintptr_t kLightingOn = 0x0086A0CC;
// GPL's transformed-vertex memo (report 105 section 4.4; gpl_vertexTransformCached 0x4CFBB0): a
// vertex record's word 0 points at its 64-byte slot in the pool while it is cached -- clip xyzw at
// +0, the frustum outcode at +28, the user-plane bits at +32, the plane distances at +40 -- and
// gpl_transformedVertexSourceList[i] names the record that owns slot i.
constexpr uintptr_t kVertexPool = 0x00578730;       // gpl_transformedVertexPool, -> slot 0
constexpr uintptr_t kVertexPoolIndex = 0x005807F0;  // gpl_transformedVertexPoolIndex, slots in use
constexpr uintptr_t kVertexSources = 0x00570730;    // gpl_transformedVertexSourceList[8192]
constexpr uint32_t  kVertexSourceCount = 8192;

// The MATERIAL handler table funcs_4CE747 (0x570220) by flags & 0x3F: a set bit has a handler of
// its own; every other index is gpl_materialRender_passThrough 0x4CD170, which only dispatches the child -- no texture, no class
// gate. Indices 1 2 4 5 8 9 C D, 10 11 14 15 18 19 1C 1D, 20 21 24 25 28 29 2C 2D.
constexpr uint64_t kMaterialHandlers = 0x0000333333333336ull;

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

constexpr int kRegisterCount = 1024;  // selectors and SETREG index registers & 0x3FF
constexpr int kMaxDepth = 512;
constexpr int kMaxSelectorCases = 4096;
constexpr int kMaxInstancesLogged = 32;

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
constexpr int      kMaxSkinnedObjects = 16;
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
    WalkCounts counts;
    // The cell's user planes take part in the decisions (GPL's own walk of one cell's record);
    // false for a whole-car list in stage 2b.
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
    // Skinned leaves of the first list per object, for the instance log line.
    int32_t  skinnedObject[kMaxSkinnedObjects];
    uint32_t skinnedLeaves[kMaxSkinnedObjects];
    int      skinnedObjects = 0;
    // The objects this walk has found its leaves in, [prim, prim + size): GPL's memory does not
    // change during a walk, so carBakeObjectOf is asked once per object and walk.
    uintptr_t knownPrim[kMaxKnownObjects];
    uint32_t  knownSize[kMaxKnownObjects];
    int32_t   knownObject[kMaxKnownObjects];
    int       knownObjects = 0;
    // Sampled walks (kProfileEvery) time their parts in TSC ticks.
    bool      profile = false;
    WalkTicks ticks;

    // Stage 2 (bakedCars = 2): the list the walk fills, the MATERIAL state it fills it with --
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
    // A verify walk of a union list: the first eye's order nodes, whose child order it keeps.
    const std::vector<ShareOrder>* orders = nullptr;

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

    // A kept list's walk with configurations (report 105 section 4.24, bakedCarConfigs): every
    // configuration gate (CarConfig.h) is decided for this eye, `configGate` the last one met. A
    // hazard that names the gates whose sides it depends on teaches them to CarConfig and the walk
    // goes on, so one walk learns every such gate it meets (`learnedGates` new ones); a hazard
    // that names none (nothing to name, a full set: `unlearned`, by UnionRedone) ends it.
    bool      learn = false;
    uintptr_t configGate = 0;
    uint32_t  learnedGates = 0;
    uint32_t  unlearned[kUnionRedoneCount] = {};
};

// -- GPL's vertex and normal memos ----------------------------------------------------------------
//
// The walk's emulation (CarMemo.h), and GPL's own memo word for what GPL memoised before the walk.

static_assert(kNoFrameId == kMemoNoFrame, "memoStore leaves a frame past the walk's limit out");

Memo g_vertexMemo;
Memo g_normalMemo;

// GPL's own memo, placed through rebase() at install; 0 when refused, and then nothing GPL
// memoised before a walk is seen (counted as stale words).
uintptr_t g_vertexPool = 0;
uintptr_t g_vertexPoolIndex = 0;
uintptr_t g_vertexSources = 0;

// A vertex GPL memoised before this walk, outside every pop(1) the walk makes: the slot its
// record's word 0 names, when that slot is in use and its source is the record; else 0.
uintptr_t gplMemoSlot(uintptr_t record, SkinCounts& skin) {
    uint32_t slot = at<uint32_t>(record);
    if (!slot) return 0;
    if (!g_vertexPool) {
        ++skin.staleWords;
        return 0;
    }
    uint32_t pool = at<uint32_t>(g_vertexPool);
    int32_t  used = at<int32_t>(g_vertexPoolIndex);
    uint32_t offset = slot - pool;
    uint32_t index = offset >> 6;
    if (slot < pool || (offset & 63) || used <= 0 || index >= static_cast<uint32_t>(used) ||
        index >= kVertexSourceCount || at<uint32_t>(g_vertexSources + 4u * index) != record) {
        ++skin.staleWords;
        return 0;
    }
    return slot;
}

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
    if (walk.learn) ++walk.unlearned[reason];
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
        int learned = carConfigLearn(node);
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
        int learned = carConfigLearn(tape.orders[order].node);
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
    uint32_t entry = memoEntry(g_vertexMemo, key);
    uint16_t held = 0, heldCondition = 0, heldRegion = 0;
    if (memoAlive(g_vertexMemo, entry, key, held, heldCondition, heldRegion))
        return heldCondition == walk.condition && heldRegion == walk.region
                   ? held
                   : unionHeld(walk, held, heldCondition, heldRegion, frame);
    gplSlot = gplMemoSlot(record, walk.skin);
    if (walk.tape) shareVertex(*walk.tape, record, gplSlot != 0);
    if (gplSlot) return kGplMemoFrame;
    memoStore(g_vertexMemo, entry, key, frame.id, walk.condition, walk.region,
              walk.skin.vertexOverflow);
    return frame.id;
}

// A NORM record lit by gpl_normalLightCached 0x4CFA70: the frame of its first lighting, kept the
// same way. A memo word GPL set before the walk is taken as the dispatch frame's (0).
uint16_t resolveNormal(Walk& walk, uintptr_t record, const Frame& frame) {
    auto     key = static_cast<uint32_t>(record);
    uint32_t entry = memoEntry(g_normalMemo, key);
    uint16_t held = 0, heldCondition = 0, heldRegion = 0;
    if (memoAlive(g_normalMemo, entry, key, held, heldCondition, heldRegion))
        return heldCondition == walk.condition && heldRegion == walk.region
                   ? held
                   : unionHeld(walk, held, heldCondition, heldRegion, frame);
    if (at<uint32_t>(record) != 0) {
        ++walk.skin.liveNormals;
        return 0;
    }
    memoStore(g_normalMemo, entry, key, frame.id, walk.condition, walk.region,
              walk.skin.normalOverflow);
    return frame.id;
}

// The walk's frame `id`, or `current` when it is not one (unmemoised).
const Frame& frameOf(const Walk& walk, uint16_t id, const Frame& current) {
    return id < walk.frameCount ? walk.frames[id] : current;
}

// gpl_matrixTranslateEuler(t, r): T(t) * Rz * Ry * Rx, the same 3x4 as Scene.cpp's poseMatrix,
// which places every track object (report 57 section 1.2).
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
    uint64_t start = walk.profile ? __rdtsc() : 0;
    uintptr_t gplSlot = 0;
    uint16_t  id = resolveVertex(walk, record, frame, gplSlot);
    if (walk.profile) walk.ticks.memo += __rdtsc() - start;
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

// -- stage 2: the draw list ----------------------------------------------------------------------

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
// exactly 0 (report 103 section 4).
void enterFrame(Walk& walk, const Frame& parent, uintptr_t node, Frame& child) {
    baked::CarDrawList& list = *walk.list;
    if (list.frames.size() >= 0xFFFF) {
        ++walk.emit.overflow;
        child.index = parent.index;
        child.halfFrom = parent.halfFrom;
        return;
    }
    bool rotates = wordFloat(node, 4) != 0.0f || wordFloat(node, 5) != 0.0f ||
                   wordFloat(node, 6) != 0.0f;
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
        material.texture = textureOf(material.textureHandle);
        if (material.texture < 0 && material.textureHandle) ++walk.emit.unresolved;
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
    static baked::CarVertex corners[kMaxSkinCorners];
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
    ++walk.skin.leaves;
    ++walk.skin.byType[type];
    walk.skin.dynamicVertices += count;
    for (int i = 0; i < walk.skinnedObjects; ++i)
        if (walk.skinnedObject[i] == objectIndex) {
            ++walk.skinnedLeaves[i];
            return;
        }
    if (walk.skinnedObjects < kMaxSkinnedObjects) {
        walk.skinnedObject[walk.skinnedObjects] = objectIndex;
        walk.skinnedLeaves[walk.skinnedObjects++] = 1;
    }
}

// One polygon or BILLBOARD node: its leaf in the bake of the object that holds it, appended under
// the current frame and material. A sprite carries its colour word as read now (the helmet tint),
// or white when its type has no colour (bit 0x200 clear, gpl_polyEmitTexturedUV). A sprite's
// corners are never memoised: gpl_spriteDrawVerticalAxis 0x4D6490 and the screen-aligned drawer
// build them from the anchor's raw coordinates and transform them inside a push / pop(1).
void emitLeaf(Walk& walk, uintptr_t node, uint8_t type, const Frame& frame, bool skinned) {
    EmitCounts& emit = walk.emit;
    if (node - walk.prim >= walk.primSize) {
        uint64_t start = walk.profile ? __rdtsc() : 0;
        int      known = 0;
        while (known < walk.knownObjects && node - walk.knownPrim[known] >= walk.knownSize[known])
            ++known;
        if (known < walk.knownObjects) {
            walk.prim = walk.knownPrim[known];
            walk.primSize = walk.knownSize[known];
            walk.object = walk.knownObject[known];
        } else {
            walk.object = carBakeObjectOf(node, walk.prim, walk.primSize);
            if (walk.object >= 0 && walk.knownObjects < kMaxKnownObjects) {
                walk.knownPrim[walk.knownObjects] = walk.prim;
                walk.knownSize[walk.knownObjects] = walk.primSize;
                walk.knownObject[walk.knownObjects++] = walk.object;
            }
        }
        if (walk.profile) walk.ticks.object += __rdtsc() - start;
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
    ++emit.leaves;
    if (walk.unionMargin > 0.0f && mayBlend(walk, scene, leaf)) ++walk.blendedLeaves;
    if (walk.program && walk.list->polygons >= kMaxProgramLeaves) unionHazard(walk, kUnionTooBig);
    if (skinned && !(leaf.flags & baked::kCarLeafNoGeometry)) {
        uint64_t start = walk.profile ? __rdtsc() : 0;
        emitSkinned(walk, walk.object, object, leaf, node, type, frame);
        if (walk.profile) walk.ticks.skinned += __rdtsc() - start;
        return;
    }
    uint32_t colour = 0;
    if (type == 0x03) colour = (word(node, 0) & 0x200) ? word(node, 2) : 0xFFFFFFFFu;
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
    if (walk.learn && carConfigDecides(node)) {
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

// A BSP4 or BSP3_ASYM decided for this eye, in the child order of side `order`: this eye's, or in
// a verify walk of a union list the first eye's (walk.orders). w3 and BSP4's w4 are drawn on this
// eye's side either way.
void visitOrdered(Walk& walk, uintptr_t node, uint8_t type, const Frame& frame, int depth,
                  bool front, bool order) {
    unsigned other = type == 0x08 ? 5 : 4;
    if (order) {
        visitChild(walk, node, other, frame, depth);
        if (front) visitChild(walk, node, 3, frame, depth);
        visitChild(walk, node, 2, frame, depth);
        if (type == 0x08 && !front) visitChild(walk, node, 4, frame, depth);
    } else {
        visitChild(walk, node, 2, frame, depth);
        if (type == 0x08 && !front) visitChild(walk, node, 4, frame, depth);
        visitChild(walk, node, other, frame, depth);
        if (front) visitChild(walk, node, 3, frame, depth);
    }
}

bool orderSide(const Walk& walk, uintptr_t node, const Frame& frame, bool front) {
    if (!walk.orders) return front;
    for (const ShareOrder& o : *walk.orders)
        if (o.node == node && o.frameKey == frame.key) return o.front;
    return front;
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
void visitWheel(Walk& walk, uintptr_t node, uint8_t type, unsigned wheel, const Frame& child,
                int depth, size_t vertexMark, size_t normalMark) {
    ShareTape& tape = *walk.tape;
    uintptr_t  current = word(node, 8);
    const uintptr_t* models = walk.wheelModels[wheel];
    bool       known = false;
    for (unsigned m = 0; m < kWheelModels; ++m) known = known || models[m] == current;
    if (!known || type == 0x16 || child.id == 0 || child.id == kNoFrameId ||
        child.id > tape.frames.size() || tape.frames[child.id - 1u].node != node ||
        tape.conditions.size() + kWheelModels > kMaxShareConditions) {
        ++tape.wheelsAsPosed;
        visitChild(walk, node, 8, child, depth);
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
    ++tape.wheelsSwitched;
    for (unsigned m = 0; m < count; ++m) {
        if (m > 0) {
            memoUndo(g_vertexMemo, vertexMark);
            memoUndo(g_normalMemo, normalMark);
        }
        walk.condition = static_cast<uint16_t>(first + m);
        visit(walk, order[m], child, depth + 1, 0);
    }
    walk.condition = enclosing;
}

// One node, as gpl_sceneNodeRenderTable 0x570330 dispatches it. `rootChild` replaces word 1 of the
// root MATERIAL, as gpl_carRecordDraw points it at the `car` or `cockpit` handle for the dispatch.
void visit(Walk& walk, uintptr_t node, const Frame& frame, int depth, uintptr_t rootChild) {
    WalkCounts& counts = walk.counts;
    if (walk.abort) return;
    if (depth > kMaxDepth) {
        ++counts.depthLimit;
        return;
    }
    int32_t typeWord = at<int32_t>(node);
    if (typeWord < 0) return;  // every dispatcher skips a negative type word (gpl_nullNode)
    uint8_t type = static_cast<uint8_t>(typeWord);
    if (type < 0x24) ++counts.byType[type];

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
        ++counts.sprites;
        ++counts.polygons;
        if (walk.list) emitLeaf(walk, node, type, frame, false);
        return;

    case 0x04: {  // GROUP
        ++counts.controls;
        uint32_t count = word(node, 1);
        for (uint32_t i = 0; i < count; ++i) visitChild(walk, node, 2 + i, frame, depth);
        return;
    }

    case 0x05: {  // MATERIAL: gpl_renderNodeDispatch 0x4CE740 by flags & 0x3F
        ++counts.controls;
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
            if (!(walk.classVisible & (1u << (surfaceClass & 31)))) {
                ++counts.classHidden;
                return;
            }
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
        ++counts.controls;
        ++counts.gates;
        if (nearGate(walk, frame, node) && visitBothWays(walk, node, type, frame, depth)) return;
        if (gateFront(walk, frame, node)) visitChild(walk, node, 2, frame, depth);
        return;
    case 0x07:  // SIDEPICK
        ++counts.controls;
        ++counts.gates;
        if (nearGate(walk, frame, node) && visitBothWays(walk, node, type, frame, depth)) return;
        visitChild(walk, node, gateFront(walk, frame, node) ? 2 : 3, frame, depth);
        return;
    case 0x08:    // BSP4: front 5, 3, 2; back 2, 4, 5 (gpl_nodePlane08Four 0x4D76B0)
    case 0x09: {  // BSP3_ASYM: front 4, 3, 2; back 2, 4
        ++counts.controls;
        ++counts.gates;
        if (nearGate(walk, frame, node) && visitBothWays(walk, node, type, frame, depth)) return;
        bool front = gateFront(walk, frame, node);
        visitOrdered(walk, node, type, frame, depth, front, orderSide(walk, node, frame, front));
        return;
    }
    case 0x0A:  // BSP3: every child from either side; the order does not change the count
        ++counts.controls;
        ++counts.gates;
        visitChild(walk, node, 2, frame, depth);
        visitChild(walk, node, 3, frame, depth);
        visitChild(walk, node, 4, frame, depth);
        return;
    case 0x0B:  // BSP2
        ++counts.controls;
        ++counts.gates;
        visitChild(walk, node, 2, frame, depth);
        visitChild(walk, node, 3, frame, depth);
        return;

    case 0x0C:  // ALTSET
        ++counts.controls;
        visitChild(walk, node, 1, frame, depth);
        return;

    case 0x0D:
    case 0x13:
    case 0x16: {  // XFORM: gpl_nodePoseRender 0x4CC650, child at word 8
        ++counts.controls;
        ++counts.transforms;
        float pose[12];
        poseMatrix(node, pose);
        Frame child;
        std::memcpy(child.planes, frame.planes, sizeof(child.planes));
        enterPose(frame, pose, walk.planeMask, child);
        enterRoot(frame, pose, child);
        child.index = frame.index;
        child.halfFrom = frame.halfFrom;
        child.node = node;
        child.key = ((frame.key ^ static_cast<uint32_t>(node)) * 2654435761u) ^ (frame.key >> 15);
        child.parentCanon = frame.canon;
        if (walk.list) enterFrame(walk, frame, node, child);
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
        if (walk.tape) shareFrame(*walk.tape, node, frame.id, child.id, pose);
        // gpl_matrixPush saves the memo pools' marks; 0x0D/0x13 (gpl_nodePoseRender 0x4CC650)
        // pop(1) back to them, 0x16 (0x4CC6A0) pops(0) and leaves what its child memoised.
        size_t vertexMark = g_vertexMemo.pool.size();
        size_t normalMark = g_normalMemo.pool.size();
        int wheel = -1;
        for (int w = 0; walk.program && w < 4; ++w)
            if (node == walk.wheels[w]) wheel = w;
        walk.wheelDepth += wheel >= 0 ? 1 : 0;
        if (wheel >= 0 && walk.tape)
            visitWheel(walk, node, type, static_cast<unsigned>(wheel), child, depth, vertexMark,
                       normalMark);
        else
            visitChild(walk, node, 8, child, depth);
        walk.wheelDepth -= wheel >= 0 ? 1 : 0;
        if (type != 0x16) {
            memoUndo(g_vertexMemo, vertexMark);
            memoUndo(g_normalMemo, normalMark);
        }
        return;
    }

    case 0x0E:  // SUBOBJECT: gpl_objDrawRootNode(word 1 = the Obj3d)
        ++counts.controls;
        visit(walk, at<uint32_t>(word(node, 1) + kObjectRoot), frame, depth + 1, 0);
        return;

    case 0x0F: {  // SORTGROUP with no dynamic records: the static child, then the elements
        ++counts.controls;
        visitChild(walk, node, 10, frame, depth);
        uint32_t count = word(node, 15);
        for (uint32_t i = 0; i < count; ++i) visitChild(walk, node, 19 + 4 * i, frame, depth);
        return;
    }

    case 0x11: {  // SELECTOR: gpl_nodeSelectorRender 0x4CF9B0, value by gpl_lodEvaluateVariable
        ++counts.controls;
        ++counts.selectors;
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
        case 3: ++counts.clockSelectors; break;
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
            if (index < 0 || index >= count) {
                ++counts.indexOutOfRange;
                return;
            }
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
        ++counts.setRegisters;
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
        ++counts.lines;
        uintptr_t gplSlot = 0;
        resolveVertex(walk, word(node, 2), frame, gplSlot);
        resolveVertex(walk, word(node, 3), frame, gplSlot);
        return;
    }

    case 0x18: {  // SPHERECULL: the user planes only (gpl_nodeSphereCull 0x4D7120)
        ++counts.controls;
        if (walk.planeMask) {
            Transformed centre;
            transformVertex(walk, word(node, 1), frame, centre);
            float radius = wordFloat(node, 2);
            for (int i = 0; i < 6; ++i) {
                if (!(walk.planeMask & (1u << i))) continue;
                float distance = centre.planes[i];
                if (radius + distance < 0.0f && distance - radius < 0.0f) {
                    ++counts.sphereCulled;
                    return;
                }
            }
        }
        visitChild(walk, node, 3, frame, depth);
        return;
    }

    case 0x19: {  // BOXCULL: 8 corners, culled when all share an outcode bit (0x4D7210)
        ++counts.controls;
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
        if (walk.program && walk.learn && carConfigDecides(node)) {
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
        if (common) {
            ++counts.boxCulled;
            return;
        }
        visitChild(walk, node, 9, frame, depth);
        return;
    }

    case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: {
        ++counts.polygons;
        if (walk.profile) {
            uint64_t start = __rdtsc();
            bool     skinned = resolveCorners(walk, node, type, frame);
            uint64_t resolved = __rdtsc();
            walk.ticks.memo += resolved - start;
            if (walk.list) emitLeaf(walk, node, type, frame, skinned);
            walk.ticks.emit += __rdtsc() - resolved;
            if (!walk.list && skinned) {
                ++walk.skin.leaves;
                ++walk.skin.byType[type];
            }
            return;
        }
        bool skinned = resolveCorners(walk, node, type, frame);
        if (walk.list) {
            emitLeaf(walk, node, type, frame, skinned);
        } else if (skinned) {
            ++walk.skin.leaves;
            ++walk.skin.byType[type];
        }
        return;
    }

    default:
        ++counts.unknownTypes;
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
};

// One pass's lists, reused pass after pass so their vectors keep their capacity.
struct PassArena {
    std::vector<baked::CarDrawList> lists;
    std::vector<uintptr_t>          records;
    uint32_t                        count = 0;
    std::vector<uintptr_t>          failed;  // records whose walk faulted this pass
};

struct State {
    bool      installed = false;
    bool      stage2 = false;  // bakedCars >= 2 and the bake bound
    // bakedCars = 2: every walk fills a draw list. In stage 2b only the cars gplvr draws get one.
    bool      listsForAll = false;
    // bakedCars = 3 with the body dispatch wrapped: cars in baked main passes are gplvr's.
    bool      stage3 = false;
    CarDrawFn body = nullptr;  // gpl_renderNodeDispatch, the body dispatch's own target
    PassArena arena;
    Addresses at;
    CarDrawFn original = nullptr;
    TeamInstanceFn teamInstance = nullptr;
    PoseFn    pose = nullptr;
    ShadowFn  shadow = nullptr;
    bool      shadowWrapped = false;

    int64_t       windowStart = 0;
    Second        second;
    void*         instancesLogged[kMaxInstancesLogged] = {};
    int           instancesLoggedCount = 0;
};

State g_car;

// Polygons the shadow calls emitted during the current car draw.
volatile uint32_t g_shadowPolygons = 0;

// Stage 2b: set around GPL's draw of a car gplvr draws, so the body dispatch returns at once --
// kSkipMain in a main pass, kSkipMirror in a mirror pass; and how often it did, per second.
constexpr uint32_t kSkipMain = 1;
constexpr uint32_t kSkipMirror = 2;
volatile uint32_t  g_skipBody = 0;
uint32_t           g_bodySkipped = 0;
uint32_t           g_bodySkippedMirror = 0;

uint32_t polygonsDrawn() { return *reinterpret_cast<const volatile uint32_t*>(g_car.at.polygonsDrawn); }

// Replaces the rel32 of the body dispatch at 0x48703F: ecx is the root MATERIAL, nothing is on
// the stack, as gpl_renderNodeDispatch takes it.
void __fastcall bodyWrapper(void* node, void*) {
    if (g_skipBody) {
        if (g_skipBody == kSkipMirror)
            ++g_bodySkippedMirror;
        else
            ++g_bodySkipped;
        return;
    }
    g_car.body(node, nullptr);
}

// In a baked main pass the residual flush also learns which of its polygons this call submitted
// and where GPL's walk was, to test them by that place (ResidualDepth.h, report 105 section 4.8).
int __cdecl shadowWrapper(void* record, void* node) {
    uint32_t before = polygonsDrawn();
    residualShadowBegin();
    int      result = g_car.shadow(record, node);
    uint32_t emitted = polygonsDrawn() - before;
    residualShadowEnd(bakedPassWalkPosition(), emitted);
    g_shadowPolygons += emitted;
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
void beginWalk(Walk& walk, const Frame& root) {
    const baked::CarScene& scene = carBakeScene();
    uint32_t               generation = carBakeGeneration();
    memoBegin(g_vertexMemo, scene, generation, false);
    memoBegin(g_normalMemo, scene, generation, true);
    walk.frames[0] = root;
    walk.frameCount = 1;
    walk.skin = SkinCounts{};
    walk.skinnedObjects = 0;
    walk.knownObjects = 0;
    walk.ticks = WalkTicks{};
}

// Stage 2: an empty list for this draw, holding the root frame and the MATERIAL state GPL has at
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

void logInstance(State& s, void* instance, int driver, bool cockpit, const Walk& walk,
                 const baked::CarDrawList* list) {
    const WalkCounts& c = walk.counts;
    const EmitCounts& emit = walk.emit;
    for (int i = 0; i < s.instancesLoggedCount; ++i)
        if (s.instancesLogged[i] == instance) return;
    if (s.instancesLoggedCount >= kMaxInstancesLogged) return;
    s.instancesLogged[s.instancesLoggedCount++] = instance;
    logf("baked cars: team instance %p first drawn for driver %d (%s) -- the walk met %u control "
         "nodes (%u gates, %u selectors, %u SETREG, %u transforms), %u polygons (1B %u, 1C %u, 1D "
         "%u, 1E %u, 1F %u, 20 %u, 21 %u, 22 %u), %u sprites, %u lines, %u MATERIALs; culled by "
         "box %u, by sphere %u, by hidden class %u; unknown node types %u",
         instance, driver, cockpit ? "cockpit" : "car", c.controls, c.gates, c.selectors,
         c.setRegisters, c.transforms, c.polygons - c.sprites, c.byType[0x1B], c.byType[0x1C],
         c.byType[0x1D], c.byType[0x1E], c.byType[0x1F], c.byType[0x20], c.byType[0x21],
         c.byType[0x22], c.sprites, c.lines, c.byType[0x05], c.boxCulled, c.sphereCulled,
         c.classHidden, c.unknownTypes);
    if (!list) return;
    uint32_t spriteRuns = 0;
    for (const baked::CarDraw& draw : list->draws)
        if (draw.flags & baked::kCarLeafSprite) ++spriteRuns;
    uint32_t textured = 0, unresolved = 0;
    for (const baked::CarMaterial& material : list->materials) {
        if (!material.textureHandle) continue;
        ++textured;
        if (material.texture < 0) ++unresolved;
    }
    logf("baked cars: team instance %p stage 2 -- its first draw list holds %u leaves of the "
         "walk's %u polygons (%u without geometry) in %zu ranges (%u of sprites), %zu frames, %zu "
         "materials (%u with a texture, %u of them unresolved); leaves refused: no object %u, not "
         "a baked leaf %u, type differs %u",
         instance, list->polygons, c.polygons, list->emptyLeaves, list->draws.size(), spriteRuns,
         list->frames.size(), list->materials.size(), textured, unresolved, emit.noObject,
         emit.notLeaf, emit.typeDiffers);

    // Which objects GPL's vertex memo skins (report 105 section 4.4): in stock cars, the
    // suspension and the arms, whose 0x16 XFORMs leave their vertices memoised for the parent.
    const SkinCounts& k = walk.skin;
    char   objects[512];
    size_t used = 0;
    objects[0] = '\0';
    const baked::CarScene& scene = carBakeScene();
    for (int i = 0; i < walk.skinnedObjects && used + 64 < sizeof(objects); ++i) {
        int32_t     index = walk.skinnedObject[i];
        const char* name = index >= 0 && static_cast<size_t>(index) < scene.objects.size() &&
                                   !scene.objects[static_cast<size_t>(index)].name.empty()
                               ? scene.objects[static_cast<size_t>(index)].name.c_str()
                               : "?";
        used += static_cast<size_t>(std::snprintf(objects + used, sizeof(objects) - used,
                                                  " %.40s x %u", name, walk.skinnedLeaves[i]));
    }
    logf("baked cars: team instance %p skinning -- %u of its %u leaves drawn from corners GPL's "
         "vertex memo holds from another frame (1B %u, 1C %u, 1D %u, 1E %u, 1F %u, 20 %u, 21 %u, "
         "22 %u; %u corners, %u normals lit in another frame, %u corners and %u normals GPL "
         "memoised before the walk), by object:%s",
         instance, k.leaves, list->polygons, k.byType[0x1B], k.byType[0x1C], k.byType[0x1D],
         k.byType[0x1E], k.byType[0x1F], k.byType[0x20], k.byType[0x21], k.byType[0x22],
         k.corners, k.normals, k.liveCorners, k.liveNormals, used ? objects : " none");
}

// The per-second report (CarWalkReport.cpp).
void reportSecond(State& s) {
    if (!carwalk::reportSecond(s.second, s.windowStart, s.stage3, g_bodySkipped,
                               g_bodySkippedMirror))
        return;
    g_bodySkipped = 0;
    g_bodySkippedMirror = 0;
}

// A walk with the pass's decisions as GPL holds them at this call: the cell's clip planes, the
// class mask, the LOD scale, the selector camera and a copy of the pass's register file.
void setupWalk(const Addresses& a, Walk& walk, uintptr_t registers) {
    walk.counts = WalkCounts{};
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
    walk.orders = nullptr;
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
    std::memset(walk.unlearned, 0, sizeof(walk.unlearned));
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
// w5 the pose turns by the spin angle) with their models (section 4.30). With bakedCarConfigs it
// decides the configuration gates (section 4.24).
void beginProgram(Walk& walk, float margin, uintptr_t instance) {
    beginUnion(walk, margin > 0.0f ? margin : kMaxUnionMargin);
    walk.program = true;
    walk.everyGate = margin <= 0.0f;
    walk.learn = g_options.bakedCarConfigs != 0;
    for (unsigned i = 0; i < 4; ++i) {
        walk.wheels[i] = at<uint32_t>(instance + kInstanceWheels + 8 * i);
        for (unsigned m = 0; m < kWheelModels; ++m)
            walk.wheelModels[i][m] =
                at<uint32_t>(instance + kInstanceWheelModels + 4 * (kWheelModels * i + m));
    }
}

// What a stage-2b walk is about to start from, for sharing it between the eyes (CarShare.h).
ShareCall shareCall(const Walk& walk, const Frame& frame, uintptr_t record, uintptr_t root,
                    uintptr_t handle, int driver) {
    ShareCall call;
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
    call.poolIndex = g_vertexPoolIndex;
    call.poolSources = g_vertexSources;
    return call;
}

// Whether a first-eye walk's list can stand for the second eye's: every input the tape does not
// replay must have been the walk's own.
ShareRefusal shareRefusal(const Walk& walk, const baked::CarDrawList& list, bool walked) {
    const EmitCounts& e = walk.emit;
    if (!walked) return kShareFaulted;
    if (list.polygons != walk.counts.polygons) return kShareContract;
    if (walk.skin.liveCorners || walk.skin.liveNormals) return kShareGplMemo;
    if (walk.skin.frameOverflow) return kShareFrames;
    if (e.noObject || e.notLeaf || e.typeDiffers || e.overflow || e.slotNoList) return kShareLeaves;
    return kShareOk;
}

// Stage 2b: a car call in a baked main pass. Every call runs GPL's draw with the body dispatch
// skipped, so the pose, the helmet tint, register 11 and the blob shadow (clipped to this call's
// cell) stay GPL's. The car's first call this pass then walks it into the pass's arena -- the
// whole car, without the cell's clip planes, which only cut GPL's per-cell copies. The walk comes
// AFTER GPL's draw because that draw writes this car's helmet colour into the BILLBOARDs of the
// team model all cars of the team share (0x486ECB), and leaves the MATERIAL state as its body
// dispatch would see it. A car whose walk faulted is drawn by GPL whole on its later calls this
// pass (its first call lost the body: counted), as is a car past the arena's limit. A mirror pass
// (bakedMirrors = 1) is the same with the mirror's registers and camera, counted apart.
void drawOurs(State& s, void* recordPointer, int driver, uintptr_t instance, uintptr_t root,
              uintptr_t handle, uintptr_t registers, bool mirror) {
    OursSecond& o = mirror ? s.second.mirrorCars : s.second.cars;
    PassArena&  arena = s.arena;
    auto        record = reinterpret_cast<uintptr_t>(recordPointer);
    ++o.calls;
    bool listed = false;
    for (uint32_t i = 0; i < arena.count && !listed; ++i) listed = arena.records[i] == record;
    bool failed = false;
    for (size_t i = 0; i < arena.failed.size() && !listed && !failed; ++i)
        failed = arena.failed[i] == record;
    bool fresh = !listed && !failed && arena.count < kMaxListsPerPass;
    if (listed)
        ++o.deduped;
    else if (!failed && !fresh)
        ++o.arenaFull;

    // Debug view 6 leaves GPL's draw of every car out, blob shadow included.
    bool leftOut = bakedPassWithoutGpl();
    if (leftOut) {
        ++o.leftOut;
    } else {
        g_shadowPolygons = 0;
        uint32_t before = polygonsDrawn();
        bool     skip = listed || fresh;
        g_skipBody = !skip ? 0u : mirror ? kSkipMirror : kSkipMain;
        s.original(recordPointer, nullptr);
        g_skipBody = 0;
        uint32_t shadow = g_shadowPolygons;
        s.second.shadow += shadow;
        if (skip) {
            ++o.bodyCalls;
            o.gplPolygons += polygonsDrawn() - before - shadow;
        }
    }
    if (!fresh) return;

    if (arena.lists.size() <= arena.count) {
        arena.lists.emplace_back();
        arena.records.push_back(0);
    }
    baked::CarDrawList& list = arena.lists[arena.count];
    static Walk walk;
    setupWalk(s.at, walk, registers);
    walk.planeMask = 0;
    walk.cellPlanes = false;
    Frame frame;
    float projection[16], modelView[16];
    rootFrame(s.at, frame, projection, modelView);
    beginList(s.at, walk, list, frame, projection, modelView);
    list.driver = driver;
    list.cockpit = false;
    list.mirror = mirror;
    list.walkSlot = bakedPassWalkPosition();

    // The car's kept list serves both eyes and later frames (report 105 section 4.23); without
    // one, in VR the second eye takes the first eye's list when this walk would build the same.
    ShareCall      share = shareCall(walk, frame, record, root, handle, driver);
    ProgramOutcome program = carProgramTake(share, list);
    if (program == kProgramReused || program == kProgramVerify)
        carShareTapeSkip(share, arena.count);
    if (program == kProgramReused) {
        arena.records[arena.count++] = record;
        return;
    }
    ShareOutcome shared = program == kProgramWalk ? carShareTake(share, list) : kShareWalk;
    if (shared == kShareReused) {
        arena.records[arena.count++] = record;
        return;
    }
    float programMargin = 0.0f;
    walk.tape = program == kProgramBuild ? carProgramTapeBegin(share, programMargin) : nullptr;
    if (program == kProgramBuild && !walk.tape) program = kProgramWalk;
    if (program == kProgramWalk) walk.tape = carShareTapeBegin(share, arena.count);
    // A verify walk of an eye-independent list keeps the first eye's order where that list does.
    if (shared == kShareVerify) walk.orders = carShareVerifyOrders();

    static uint32_t walks = 0;
    LARGE_INTEGER   walkStart, walkEnd;
    QueryPerformanceCounter(&walkStart);
    if (program == kProgramBuild)
        beginProgram(walk, programMargin, instance);
    else
        beginUnion(walk, walk.tape ? walk.tape->margin : 0.0f);
    beginWalk(walk, frame);
    walk.profile = ++walks % kProfileEvery == 0;
    uint64_t tscStart = walk.profile ? __rdtsc() : 0;
    bool     walked = walkGuarded(walk, root, handle, frame);
    if (walk.profile) {
        walk.ticks.total = __rdtsc() - tscStart;
        ++o.profiled;
        o.ticks.memo += walk.ticks.memo;
        o.ticks.emit += walk.ticks.emit;
        o.ticks.skinned += walk.ticks.skinned;
        o.ticks.object += walk.ticks.object;
        o.ticks.total += walk.ticks.total;
    }
    walk.profile = false;
    int redone = firstHazard(walk);
    // A kept list's walk the car's program cannot keep (a hazard, a fault, a refused leaf) is
    // thrown away, and the car is walked as in section 4.22, with CarShare's tape.
    if (walk.program) {
        LARGE_INTEGER built;
        QueryPerformanceCounter(&built);
        if (carProgramTapeEnd(*walk.tape, walked ? redone : -1, shareRefusal(walk, list, walked),
                              list, built.QuadPart - walkStart.QuadPart, walk.learnedGates,
                              walk.unlearned)) {
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
    // again deciding every gate, as at bakedCarUnion = 0 (report 105 section 4.22).
    if (walked && walk.unionMargin > 0.0f && redone >= 0) {
        carShareUnionRedone(static_cast<UnionRedone>(redone));
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
    QueryPerformanceCounter(&walkEnd);
    o.walkTicks += walkEnd.QuadPart - walkStart.QuadPart;
    addSkinCounts(o.skin, walk.skin);
    if (walk.tape) carShareTapeEnd(*walk.tape, shareRefusal(walk, list, walked), list);
    walk.tape = nullptr;
    walk.orders = nullptr;
    if (shared == kShareVerify) carShareVerify(list, walked);
    if (program == kProgramVerify)
        carProgramVerify(list, walked, walkEnd.QuadPart - walkStart.QuadPart);
    if (!walked) {
        ++o.walkFailed;
        if (!leftOut) ++o.bodyLost;
        arena.failed.push_back(record);
        return;
    }
    arena.records[arena.count++] = record;
    ++o.lists;
    o.leaves += list.polygons;
    o.walked += walk.counts.polygons;
    o.draws += list.draws.size();
    addEmitCounts(o.emit, walk.emit);
    if (list.polygons == walk.counts.polygons) {
        ++o.matched;
    } else if (!o.differed) {
        o.differed = true;
        o.driver = driver;
        o.firstLeaves = list.polygons;
        o.firstWalked = walk.counts.polygons;
    }
    logInstance(s, reinterpret_cast<void*>(instance), driver, false, walk, &list);
}

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
    bool bakedKind = mainPass || (mirror && g_options.bakedMirrors);
    if (s.stage3 && bakedKind && !cockpit) {
        OursSecond& o = mirror ? s.second.mirrorCars : s.second.cars;
        if (!bakedPassCollecting() || bakedPassMirror() != mirror) {
            ++o.mainOutside;
        } else {
            ++o.mainInside;
            if (bakedPassDrawsCars()) {
                drawOurs(s, recordPointer, driver, instance, root, handle, registers, mirror);
                reportSecond(s);
                return;
            }
        }
    }
    // bakedCars = 3 does not verify what GPL draws itself (mirror passes, the cockpit, car calls
    // outside a baked pass): it is counted and handed to GPL unwalked. Levels 1 and 2 walk and
    // compare every draw, and so does debug view 15 for the mirror cars (bakedMirrors = 1): GPL
    // draws the mirror then, and the walk with the mirror's registers, masks and LOD scale is
    // checked against GPL's polygon counter.
    bool mirrorContract = mirror && !cockpit && bakedPassMirrorsByGpl();
    if (s.stage3 && !mirrorContract) {
        OursSecond& o = s.second.cars;
        ++o.passedThrough;
        if (cockpit)
            ++o.passedCockpit;
        else if (mirror)
            ++o.passedMirror;
        else
            ++o.passedOther;
        // Debug view 6 leaves every car but the cockpit out of the main pass.
        bool leftOut = !cockpit && bakedPassWithoutGpl();
        if (cockpit) bakedCockpitDraw(true);
        if (!leftOut) s.original(recordPointer, nullptr);
        if (cockpit) bakedCockpitDraw(false);
        reportSecond(s);
        return;
    }

    static Walk walk;
    setupWalk(s.at, walk, registers);
    Frame frame;
    float projection[16], modelView[16];
    rootFrame(s.at, frame, projection, modelView);

    static baked::CarDrawList list;
    walk.list = nullptr;
    if (s.stage2 && s.listsForAll) {
        beginList(s.at, walk, list, frame, projection, modelView);
        list.driver = driver;
        list.cockpit = cockpit;
        list.mirror = mirror;
    }

    LARGE_INTEGER walkStart, walkEnd;
    QueryPerformanceCounter(&walkStart);
    beginWalk(walk, frame);
    bool walked = walkGuarded(walk, root, handle, frame);
    QueryPerformanceCounter(&walkEnd);
    addSkinCounts(s.second.skin, walk.skin);

    // Debug view 6 leaves every car but the cockpit out of the main pass, blob shadow included.
    bool leftOut = !cockpit && bakedPassWithoutGpl();
    g_shadowPolygons = 0;
    uint32_t before = polygonsDrawn();
    if (cockpit) bakedCockpitDraw(true);
    if (!leftOut) s.original(recordPointer, nullptr);
    if (cockpit) bakedCockpitDraw(false);
    uint32_t shadow = g_shadowPolygons;
    uint32_t gpl = polygonsDrawn() - before - shadow;

    Second& t = s.second;
    ++t.draws;
    if (cockpit) ++t.cockpitDraws;
    if (mirror)
        ++t.mirrorDraws;
    else if (registers == s.at.registersMain)
        ++t.mainDraws;
    t.walkTicks += walkEnd.QuadPart - walkStart.QuadPart;
    t.shadow += shadow;
    if (!walked) {
        ++t.walkFailed;
    } else if (leftOut) {
        ++t.leftOut;
    } else {
        t.ours += walk.counts.polygons;
        t.gpl += gpl;
        addCounts(t.sums, walk.counts);
        if (walk.counts.polygons == gpl) {
            ++t.matched;
            if (cockpit) ++t.cockpitMatched;
        } else if (!t.differed) {
            t.differed = true;
            t.driver = driver;
            t.instance = reinterpret_cast<void*>(instance);
            t.cockpit = cockpit;
            t.mirror = mirror;
            t.planeMask = walk.planeMask;
            t.first = walk.counts;
            t.firstGpl = gpl;
        }
        if (walk.list) {
            // The contract of stage 2: every polygon the walk counts is a leaf of the list.
            EmitSecond& e = t.emit;
            ++e.draws;
            e.leaves += list.polygons;
            e.walked += walk.counts.polygons;
            e.ranges += list.draws.size();
            e.frames += list.frames.size();
            e.materials += list.materials.size();
            e.emptyLeaves += list.emptyLeaves;
            addEmitCounts(e.counts, walk.emit);
            if (list.polygons == walk.counts.polygons) {
                ++e.matched;
            } else if (!e.differed) {
                e.differed = true;
                e.driver = driver;
                e.cockpit = cockpit;
                e.mirror = mirror;
                e.firstLeaves = list.polygons;
                e.firstWalked = walk.counts.polygons;
                e.first = walk.emit;
            }
        }
        logInstance(s, reinterpret_cast<void*>(instance), driver, cockpit, walk, walk.list);
    }
    reportSecond(s);
}

// The rel32 target of the `call` at `site`, or 0 when the byte there is not E8.
uintptr_t callTarget(uintptr_t site) {
    if (at<uint8_t>(site) != 0xE8) return 0;
    return site + 5 + static_cast<uintptr_t>(at<int32_t>(site + 1));
}

// The data global of the pose wrapper at `target` (kPoseWrapperHead), or 0 with `why` set when
// `target` is not one.
uintptr_t poseWrapperData(uintptr_t target, const char*& why) {
    ModuleImage image;
    if (!getModuleImage(GetModuleHandleA(nullptr), image)) {
        why = "the game's image could not be read";
        return 0;
    }
    bool code = false;
    for (const Section& section : image.sections) {
        auto begin = reinterpret_cast<uintptr_t>(section.begin);
        auto end = reinterpret_cast<uintptr_t>(section.end);
        if (section.executable && section.hasRawData && target >= begin && target < end &&
            end - target >= kPoseWrapperSize)
            code = true;
    }
    if (!code) {
        why = "it is not in a code section of the game's image";
        return 0;
    }
    auto bytes = reinterpret_cast<const unsigned char*>(target);
    if (std::memcmp(bytes, kPoseWrapperHead, sizeof(kPoseWrapperHead)) != 0 ||
        std::memcmp(bytes + kPoseWrapperBodyAt, kPoseWrapperBody, sizeof(kPoseWrapperBody)) != 0 ||
        std::memcmp(bytes + kPoseWrapperTailAt, kPoseWrapperTail, sizeof(kPoseWrapperTail)) != 0) {
        why = "its bytes are not the known pose wrapper's";
        return 0;
    }
    if (callTarget(target + kPoseWrapperCall) != kPoseFunction) {
        why = "the known pose wrapper's inner call does not reach it";
        return 0;
    }
    uintptr_t data = at<uint32_t>(target + kPoseWrapperData);
    if (!isWritable(image, reinterpret_cast<const void*>(data)) ||
        !isWritable(image, reinterpret_cast<const void*>(data + 3))) {
        why = "the known pose wrapper's data operand is not a global of the game";
        return 0;
    }
    return data;
}

}  // namespace

void carWalkInstall() {
    State& s = g_car;
    if (s.installed || g_options.bakedRenderer == 0 || g_options.bakedCars == 0) return;

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

    // GPL's own vertex memo, for what it memoised before a walk: the three operands
    // gpl_vertexTransformCached reads them through (0x4CFBC0 `A1 index`, 0x4CFBC5 `8B 3D pool`,
    // 0x4CFBD3 `8D 14 85 sources`) must name the rebased addresses, or the walk does without it.
    {
        uintptr_t index = rebase(kVertexPoolIndex);
        uintptr_t pool = rebase(kVertexPool);
        uintptr_t sources = rebase(kVertexSources);
        bool code = at<uint8_t>(0x004CFBC0) == 0xA1 && at<uint16_t>(0x004CFBC5) == 0x3D8B &&
                    at<uint8_t>(0x004CFBD3) == 0x8D && at<uint16_t>(0x004CFBD4) == 0x8514;
        if (index && pool && sources && code && at<uint32_t>(0x004CFBC1) == index &&
            at<uint32_t>(0x004CFBC7) == pool && at<uint32_t>(0x004CFBD6) == sources) {
            g_vertexPoolIndex = index;
            g_vertexPool = pool;
            g_vertexSources = sources;
        } else {
            logf("baked cars: GPL's vertex memo NOT read -- gpl_vertexTransformCached does not "
                 "name the rebased pool (%08X %08X %08X); what GPL memoised before a walk counts "
                 "as `memo words not a live slot`",
                 static_cast<unsigned>(index), static_cast<unsigned>(pool),
                 static_cast<unsigned>(sources));
        }
    }
    uintptr_t slot = rebase(kCarVtableSlot);
    if (!a.polygonsDrawn || !a.eyeLocal || !a.planeMask || !a.planes || !a.modelViewSlot ||
        !a.projectionSlot || !a.lodScale || !a.classVisible || !a.backgroundTable || !a.dlong ||
        !a.dlat || !a.registersMain || !a.registersMirror || !slot) {
        logf("baked cars: NOT installed -- rebase refused one of the game's addresses (%s)",
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
        uintptr_t   poseData = poseWrapperData(pose, why);
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
    s.teamInstance = reinterpret_cast<TeamInstanceFn>(team);
    s.pose = reinterpret_cast<PoseFn>(pose);
    s.original = reinterpret_cast<CarDrawFn>(kCarRecordDraw);
    s.shadow = reinterpret_cast<ShadowFn>(kShadowFunction);

    // The shadow wrappers first, so the count is clean from the first hooked draw.
    unsigned wrapped = 0;
    for (uintptr_t site : kShadowCalls) {
        if (callTarget(site) != kShadowFunction) continue;
        auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&shadowWrapper) - (site + 5));
        if (patchBytes(reinterpret_cast<void*>(site + 1), &rel, sizeof(rel))) ++wrapped;
    }
    s.shadowWrapped = wrapped == sizeof(kShadowCalls) / sizeof(kShadowCalls[0]);

    void* hook = reinterpret_cast<void*>(&carDraw);
    if (!patchBytes(reinterpret_cast<void*>(slot), &hook, sizeof(hook))) {
        logf("baked cars: NOT installed -- could not write the car vtable slot %08X",
             static_cast<unsigned>(slot));
        return;
    }
    s.installed = true;
    logf("baked cars: stage 1 installed on the car vtable slot %08X -- every car GPL draws is "
         "walked first (team lookup %08X, pose %08X); shadow calls wrapped %u of 2%s. Watch the "
         "per-second `baked cars:` lines: MATCHED should equal the walks that did not fault",
         static_cast<unsigned>(slot), static_cast<unsigned>(team), static_cast<unsigned>(pose),
         wrapped, s.shadowWrapped ? "" : " -- GPL's count then includes the shadow");

    if (g_options.bakedCars < 2) return;
    a.texturedMask = rebase(kTexturedMask);
    a.classTextured = rebase(kClassTextured);
    a.surfaceClass = rebase(kSurfaceClass);
    a.textureList = rebase(kTextureList);
    a.specularExponent = rebase(kSpecularExponent);
    a.specular = rebase(kSpecular);
    a.lightLocal = rebase(kLightLocal);
    if (!a.texturedMask || !a.classTextured || !a.surfaceClass || !a.textureList ||
        !a.specularExponent || !a.specular || !a.lightLocal) {
        logf("baked cars: stage 2 NOT on -- rebase refused one of the MATERIAL state or light "
             "addresses (%s); stage 1 runs alone",
             rebaseSummary());
        return;
    }
    if (!carBakeBind()) return;
    s.stage2 = true;
    s.listsForAll = true;
    if (g_options.bakedCars < 3) {
        logf("baked cars: stage 2 on -- each walk also fills a draw list from car objects baked "
             "out of GPL's memory on first sight (one `baked cars: object` line each); the "
             "per-second `baked cars: stage 2` line should say MATCHED on every list. Nothing is "
             "drawn from them yet");
        return;
    }

    // Stage 2b: the body dispatch must be the call report 105 section 4.3 read, behind the
    // `mov ecx, esi` that hands it the root MATERIAL; otherwise GPL keeps drawing every car.
    uintptr_t body = callTarget(kBodyCall);
    if (body != kNodeDispatch ||
        std::memcmp(reinterpret_cast<const void*>(kBodyCall - sizeof(kBodyCallBefore)),
                    kBodyCallBefore, sizeof(kBodyCallBefore)) != 0) {
        logf("baked cars: stage 2b NOT on -- the body dispatch at %08X reaches %08X (expected "
             "%08X behind 8B CE); stage 2a runs, GPL draws every car",
             static_cast<unsigned>(kBodyCall), static_cast<unsigned>(body),
             static_cast<unsigned>(kNodeDispatch));
        return;
    }
    s.body = reinterpret_cast<CarDrawFn>(kNodeDispatch);
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&bodyWrapper) - (kBodyCall + 5));
    if (!patchBytes(reinterpret_cast<void*>(kBodyCall + 1), &rel, sizeof(rel))) {
        logf("baked cars: stage 2b NOT on -- could not write the body dispatch at %08X; stage 2a "
             "runs, GPL draws every car",
             static_cast<unsigned>(kBodyCall));
        return;
    }
    s.listsForAll = false;
    s.stage3 = true;
    logf("baked cars: stage 2b on -- body dispatch %08X wrapped; in a baked main pass gplvr draws "
         "each car from one list a pass and GPL draws only its shadow; mirrors, the cockpit and "
         "car calls outside a baked pass stay GPL's, counted and not walked. Watch "
         "`baked cars: stage 2b` (calls OUTSIDE 0, contract MATCHED, GPL polygons 0) and `baked: "
         "cars` (commands == list draws)",
         static_cast<unsigned>(kBodyCall));
    carProgramInstall();
}

void carWalkPassBegin(bool mirror) {
    PassArena& arena = g_car.arena;
    // The pass that ends keeps its lists for the other eye; the arena may get other vectors back.
    carSharePassBegin(mirror, g_car.stage3, arena.lists, arena.count);
    carProgramPassBegin(mirror, g_car.stage3);
    arena.records.resize(arena.lists.size());
    arena.count = 0;
    arena.failed.clear();
}

const baked::CarDrawList* carWalkPassLists(uint32_t& count) {
    count = g_car.arena.count;
    return count ? g_car.arena.lists.data() : nullptr;
}

}  // namespace gplvr
