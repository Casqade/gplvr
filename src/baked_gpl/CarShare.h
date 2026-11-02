#pragma once

// The car walk shared between the eyes (docs/audit-2026-09-18/105-baked-renderer-rewrite.md
// section 4.21). In VR each eye is one gpl_sceneRender, the left eye first, and each pass walks
// every car it draws into a draw list (CarWalk.cpp). Both eyes of a
// frame pose a car from the same render record, so of what a list holds only these can differ
// between them:
// - the walk's eye-dependent decisions: the eye's side of every gate (SIDEGATE, SIDEPICK, BSP4,
//   BSP3_ASYM), every BOXCULL against the frustum, every LOD selector (kind 0: the reference
//   vertex's clip w; kinds 1 and 2: the selector camera);
// - the fields read from the camera: the projection, the root model-view, the light in the
//   dispatch frame, the walk slot and each frame's eye.
// The first eye's walk records a tape per car: each of those decisions with what it was taken
// from (the walk frame, and the frame GPL's vertex memo placed each vertex in), and everything
// else the walk read that GPL writes between two calls of the car -- the pose words and child
// pointers of every XFORM it entered, the BILLBOARD colours, the registers its selectors read from
// the pass's register file, the pass's masks and MATERIAL state. The second eye's pass replays
// the tape from its own camera instead of walking. When every read and every decision agree, the
// walk would take the same path and build the same list, so the first eye's list is taken with
// its camera fields replaced; otherwise the car is walked as before.
//
// Mirror passes share the same way, each mirror pass with the first eye's mirror pass of the same
// place in the eye's order, apart from the main passes.
//
// Eye-independent lists (report 105 section 4.22): the first eye's walk takes
// every gate whose plane passes within the union margin of its eye -- the measured separation of
// the two eyes, with room to spare -- both ways. Each side's subtree is walked under a condition
// (ShareCondition: the plane, the walk frame, the side, the enclosing condition), its draws carry
// it, and each pass evaluates the conditions for its own eye into CarDrawList::conditionOn, which
// the renderer skips draws by. Every other gate is decided as before and holds for any eye within
// the margin, so the second eye's replay agrees on every gate and only a box, a selector or an
// input that moved still makes it walk. A BSP4 or BSP3_ASYM taken both ways keeps the walking
// eye's child order (ShareOrder): the second eye's list is its own walk with that order there.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

// A kept list's walk (report 105 section 4.23) takes every gate of a cars18 car both ways: up to
// ~3,700 conditions at 3 m.
constexpr uint32_t kMaxShareConditions = 16384;

// Why a first-eye walk's list cannot be shared, decided as it is walked.
enum ShareRefusal : uint8_t {
    kShareOk = 0,
    kShareFaulted,   // the walk faulted: there is no list
    kShareContract,  // the list's leaves are not the walk's polygons
    kShareGplMemo,   // the walk read a vertex or normal GPL had memoised before it
    kShareFrames,    // a frame past the walk's 256, or one the tape cannot place
    kShareLeaves,    // a refused leaf, or a frame or material overflow
    kSharePlanes,    // the walk used the cell's clip planes
    kShareProgram,   // the car's kept list drew it (CarProgram.h): no walk recorded this tape
    kShareRefusalCount
};

// One XFORM the walk entered, in the order of the walk's frame ids (the root is id 0, this one
// its index + 1): the words GPL's pose writes, as the walk read them, and the matrix made of them.
struct ShareFrame {
    uintptr_t node = 0;
    uint32_t  words[7] = {};  // w1..w6 and the child pointer w8
    float     pose[12] = {};
    uint16_t  parent = 0;     // the frame id the XFORM was entered from
    // A wheel XFORM a kept list's walk took every model of (report 105 section 4.30): the child
    // conditions [switchFirst, switchFirst + switchCount), one a model; w8 must be one of them.
    uint16_t switchFirst = 0;
    uint16_t switchCount = 0;
};

// Every decision below carries the condition it was met under (0: none); an eye for which that
// condition does not hold never meets it, so its replay skips it.
struct ShareGate {
    uintptr_t plane;   // the gate's word 1
    uint16_t  frame;   // the walk frame it was tested in
    uint16_t  condition;
    bool      front;
    // A kept list's walk decided it as a configuration gate (CarConfig.h, report 105 section
    // 4.24), not for being outside the margin.
    bool      config = false;
};

struct ShareBox {
    uintptr_t node;
    uint16_t  corner[8];  // the frame each corner's outcode came from (GPL's memo, emulated)
    uint16_t  condition;
    bool      culled;
    // A kept list's walk decided it as a configuration box (CarConfig.h, report 105 section
    // 4.24): its subtree is drawn or skipped as `culled` says, under no box condition.
    bool      config = false;
};

// A selector of kind 0, 1 or 2 and the child word it took (-1 none, -2 an invalid case count).
struct ShareSelector {
    uintptr_t node;
    uint32_t  param;
    uint16_t  frame;  // kind 0: the frame the reference vertex's clip w came from
    uint16_t  condition;
    uint8_t   kind;
    int32_t   chosen;
};

// One side of a gate the walk took both ways; condition id = its index + 1. It holds for an eye
// when `parent` holds and the eye is on side `front` of the plane in walk frame `frame`. A kept
// list's walk (report 105 section 4.23) also makes a BOXCULL a condition (kConditionBox): it
// holds when `parent` holds and the box tape.boxes[plane] is not culled for the eye; and each
// model of a wheel XFORM (kConditionChild, section 4.30): it holds when `parent` holds and the
// XFORM of walk frame `frame` points its child (w8) at `plane`, whatever the eye.
enum ShareConditionKind : uint8_t {
    kConditionSide = 0,
    kConditionBox = 1,
    kConditionChild = 2,
};

struct ShareCondition {
    uintptr_t plane;
    uint16_t  frame;
    uint16_t  parent;
    bool      front;
    bool      holds;  // for the walking eye
    uint8_t   kind = kConditionSide;
    uintptr_t node = 0;  // a side's gate, which a kept list's hazard can name (CarConfig.h)
};

// A BSP4 or BSP3_ASYM the walk took both ways: its children follow the walking eye's side. The
// list's draws [begin, end) are its children's, [begin, split) the half the walking eye drew
// first; the order nodes met inside it follow it in the tape, [index + 1, splitOrder) in the
// first half and [splitOrder, nested) in the second. A kept list's walk seals the draws at
// begin, split and end, so another eye's pass can swap the halves.
struct ShareOrder {
    uintptr_t node;
    uint32_t  frameKey;   // Frame::key of the walk frame it was met in
    uintptr_t plane;
    uint16_t  frame;
    uint16_t  condition;  // what the node itself was met under
    uint16_t  onFront;    // the condition of its front side (w3's)
    bool      front;      // the walking eye's side, whose order the list keeps
    bool      bothDrawn;  // both of the children whose order the side sets appended leaves
    bool      bothBlended;  // both appended leaves that may be blended (phase 1 of the renderer)
    uint32_t  begin;
    uint32_t  split;
    uint32_t  end;
    uint32_t  splitOrder;
    uint32_t  nested;
};

// A skinned leaf as a kept list's walk built it (emitSkinned), re-made from the re-posed frames
// each pass: `count` baked corners of object `object` from `base`, appended at `firstVertex` of
// the list's dynamicVertices; for corner i, ShareTape::skinFrames[firstCorner + 2i] is the walk
// frame its vertex was memoised in and [+ 1] its normal's.
struct ShareSkin {
    uint32_t object;
    uint32_t base;
    uint32_t count;
    uint32_t firstVertex;
    uint32_t firstCorner;
    uint16_t frame;  // the walk frame the leaf is drawn in
    bool     lit;
};

// A kind-4 selector's register, read from the pass's register file (no SETREG of this walk had
// written it yet), as the float the selector compared.
struct ShareRegister {
    uint32_t index;
    uint32_t valueBits;
};

struct ShareSprite {
    uintptr_t node;
    uint32_t  colour;
};

// A mirror glass of the cockpit's kept list (report 105 section 4.50): polygon node `node`, listed
// as `count` dynamic corners at `firstVertex` of the list's dynamicVertices (CockpitGlass.h,
// cockpitGlassCorners), whose UVs each assembly takes again from GPL's PRIM, where MirrorUv
// rewrites them.
struct ShareGlass {
    uintptr_t node;
    uint32_t  firstVertex;
    uint32_t  count;
};

struct ShareTape {
    uintptr_t record = 0;
    uintptr_t root = 0;
    uintptr_t handle = 0;
    int       driver = 0;
    uint32_t  classVisible = 0;
    uint32_t  textureList = 0;
    uint32_t  texturedMask = 0;
    bool      lightingOn = false;
    uint32_t  generation = 0;  // carBakeGeneration
    uint32_t  rebaked = 0;     // carBakeTotals().objectsRebaked
    // Every vertex record the walk took through the vertex memo lies in [low, high], and is one of
    // `vertices` (report 105 section 4.43): sorted and unique once shareSealVertices has run.
    uintptr_t low = 0;
    uintptr_t high = 0;
    std::vector<uint32_t> vertices;
    ShareRefusal refusal = kShareOk;
    uint32_t     written[32] = {};  // registers a SETREG of this walk wrote, one bit each
    std::vector<ShareFrame>    frames;
    std::vector<ShareGate>     gates;
    std::vector<ShareBox>      boxes;
    std::vector<ShareSelector> selectors;
    std::vector<ShareRegister> registers;
    std::vector<ShareSprite>   sprites;
    // The union walk (report 105 section 4.22): its margin (0 when it decided every gate), its
    // conditions and its order nodes, and the eye in the dispatch frame, which the second eye's
    // call measures the eyes' separation from.
    float                       margin = 0.0f;
    std::vector<ShareCondition> conditions;
    std::vector<ShareOrder>     orders;
    float                       rootEye[3] = {};
    // A kept list's walk only (report 105 section 4.23): its skinned leaves.
    std::vector<ShareSkin> skins;
    std::vector<uint16_t>  skinFrames;
    // And the cockpit's mirror glass, listed with GPL's live UVs (section 4.50).
    std::vector<ShareGlass> glass;
};

// -- recording: from the walk while CarWalk's Walk::tape is set ----------------------------------

inline void shareGate(ShareTape& tape, uintptr_t plane, uint16_t frame, bool front,
                      uint16_t condition, bool config = false) {
    tape.gates.push_back(ShareGate{plane, frame, condition, front, config});
}

// Whether every eye `condition` holds for is one `ancestor` holds for: `ancestor` encloses it.
inline bool shareImplies(const ShareTape& tape, uint16_t condition, uint16_t ancestor) {
    while (condition != ancestor) {
        if (condition == 0 || condition > tape.conditions.size()) return false;
        condition = tape.conditions[condition - 1].parent;
    }
    return true;
}

// A side of a gate taken both ways, under `parent`; `eyeFront` is the walking eye's side. Its id,
// or 0 when the tape holds kMaxShareConditions already.
uint16_t shareCondition(ShareTape& tape, uintptr_t plane, uint16_t frame, uint16_t parent,
                        bool front, bool eyeFront);
// A BOXCULL's subtree in a kept list's walk: holds where box tape.boxes[box] is not culled;
// `drawn` for the walking eye. Its id, or 0 when the tape is full.
uint16_t shareBoxCondition(ShareTape& tape, size_t box, uint16_t parent, bool drawn);
// A model `child` of the wheel XFORM entered as walk frame `frame`, in a kept list's walk; `current`
// when it is the child GPL's pose set. Its id, or 0 when the tape is full.
uint16_t shareChildCondition(ShareTape& tape, uintptr_t child, uint16_t frame, uint16_t parent,
                             bool current);inline void shareSetRegister(ShareTape& tape, uint32_t index) {
    index &= 0x3FF;
    tape.written[index >> 5] |= 1u << (index & 31);
}

// A vertex record the memo resolved for the first time this walk; `gplMemo` when GPL's own memo
// held it.
inline void shareVertex(ShareTape& tape, uintptr_t record, bool gplMemo) {
    if (record < tape.low) tape.low = record;
    if (record > tape.high) tape.high = record;
    tape.vertices.push_back(static_cast<uint32_t>(record));
    if (gplMemo && tape.refusal == kShareOk) tape.refusal = kShareGplMemo;
}

// After the walk: the tape's vertex records sorted and unique, for shareReadsVertex.
void shareSealVertices(ShareTape& tape);
// Whether a replay of the tape reads `record` through GPL's vertex memo: one of its vertices. A
// tape with an address range and no records (not sealed) answers yes for any record.
bool shareReadsVertex(const ShareTape& tape, uint32_t record);

// An XFORM entered from frame `parent` as walk frame `id`, with the matrix the walk made of it.
void shareFrame(ShareTape& tape, uintptr_t node, uint16_t parent, uint16_t id,
                const float pose[12]);
void shareBox(ShareTape& tape, uintptr_t node, const uint16_t corner[8], bool culled,
              uint16_t condition);
// A selector's value as the walk computed it; `frame` is the reference vertex's for kind 0. A
// kind-4 value is recorded as read from the pass's file unless `fromWrite`: a SETREG of this walk
// under a condition the selector's implies wrote it.
void shareSelector(ShareTape& tape, uintptr_t node, uint32_t kind, uint32_t param, uint16_t frame,
                   float value, uint16_t condition, bool fromWrite);
void shareSprite(ShareTape& tape, uintptr_t node, uint32_t colour);

// -- the car hook --------------------------------------------------------------------------------

// What a car's first call in a pass walks from, as CarWalk read it for this call.
struct ShareCall {
    uintptr_t      record = 0;
    uintptr_t      root = 0;
    uintptr_t      handle = 0;
    int            driver = 0;
    const float*   clip = nullptr;  // the root frame's clip from local (P x MV)
    const float*   eye = nullptr;   // gpl_eyePositionLocal
    float          lodScale = 1.0f;
    float          dlong = 0.0f;
    float          dlat = 0.0f;
    uint32_t       classVisible = 0;
    bool           lightingOn = false;
    uint32_t       planeMask = 0;
    bool           cellPlanes = false;
    const int32_t* registers = nullptr;  // the walk's copy of the pass's register file
    uint32_t       textureList = 0;
    uint32_t       texturedMask = 0;
    uintptr_t      poolIndex = 0;    // GPL's vertex memo, 0 when CarWalk could not place it
    uintptr_t      poolSources = 0;
};

enum ShareOutcome {
    kShareWalk,    // walk the car as before
    kShareReused,  // `list` now holds the first eye's list with this eye's camera fields
};

// From carWalkPassBegin, before the arena is emptied: a pass that recorded hands its lists (the
// arena's first `count`) and tapes to its store, swapping vectors, and the new pass learns its
// eye and its place in the eye. `enabled`: the car hook is installed. Also ages the eye
// separations the union margin follows, once a second.
void carSharePassBegin(bool mirror, bool enabled, std::vector<baked::CarDrawList>& lists,
                       uint32_t count);

// A car's first call in a pass, `list` begun for it (its header, root frame and root material
// read from this call). In the second eye's pass, the first eye's list when the replay agrees.
ShareOutcome carShareTake(const ShareCall& call, baked::CarDrawList& list);

// In the first eye's pass, the tape the walk into arena slot `slot` records; else null.
ShareTape* carShareTapeBegin(const ShareCall& call, uint32_t slot);
// Also gives a union walk's list its conditions for the walking eye (CarDrawList::conditionOn).
void carShareTapeEnd(ShareTape& tape, ShareRefusal refusal, baked::CarDrawList& list);
// In the first eye's pass, a car its kept list drew (CarProgram.h): arena slot `slot`'s tape is
// marked kShareProgram, so the second eye never takes an older pass's list for it.
void carShareTapeSkip(const ShareCall& call, uint32_t slot);

// The margin a union walk of this pass takes gates both ways within, metres: 1.25 x the largest
// separation of the eyes measured over the last two seconds in this pass's slot, plus 2 mm, at
// most kMaxUnionMargin; kDefaultUnionMargin before any is measured. 0 when the walk decides every
// gate: not a recording pass.
constexpr float kDefaultUnionMargin = 0.1f;
constexpr float kMaxUnionMargin = 0.5f;
float carShareUnionMargin();

// Why a union walk was thrown away and the car walked again deciding every gate.
enum UnionRedone : int {
    kUnionMemo,        // a memoised frame depends on a side some of its eyes did not take
    kUnionRegister,    // a register a selector read depends on such a side
    kUnionConditions,  // past kMaxShareConditions
    // A kept list's walk only (report 105 section 4.23): a memo entry or a register written in
    // one half of an order node and read in the other or after it, so each eye's order decides
    // it; and a list past its leaf limit.
    kUnionOrder,
    kUnionTooBig,
    kUnionRedoneCount
};

}  // namespace gplvr
