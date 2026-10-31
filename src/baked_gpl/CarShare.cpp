#include "baked_gpl/CarShare.h"

#include "Hooks.h"
#include "Log.h"
#include "Stereo.h"

#include "baked/CarScene.h"
#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarUnion.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <utility>
#include <vector>

namespace gplvr {
namespace {

constexpr int      kMaxMirrorSlots = 4;  // mirror passes per eye that can share (one per glass)
constexpr int      kSlots = 1 + kMaxMirrorSlots;
constexpr int32_t  kMaxSelectorCases = 4096;  // as CarWalk's
constexpr size_t   kMaxFrames = 257;          // the root and CarWalk's kMaxWalkFrames
// GPL's vertex memo entries scanned for the walk's vertices at most; a fuller pool refuses.
constexpr int32_t  kMaxPoolScan = 2048;

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

// enterPose's clip and eye (no user planes: a shared walk has none).
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

// gpl_drawQueueSidePredicate 0x4C7880 on the gate's plane.
bool frontSide(const float eye[3], uintptr_t plane) {
    return at<float>(plane) * eye[0] + at<float>(plane + 4) * eye[1] +
               at<float>(plane + 8) * eye[2] + at<float>(plane + 12) >=
           0.0f;
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

// A BILLBOARD's colour as emitLeaf reads it.
uint32_t spriteColour(uintptr_t node) {
    return (word(node, 0) & 0x200) ? word(node, 2) : 0xFFFFFFFFu;
}

bool sameMaterial(const baked::CarMaterial& a, const baked::CarMaterial& b) {
    return a.textureHandle == b.textureHandle && a.texture == b.texture &&
           a.surfaceClass == b.surfaceClass && a.textured == b.textured &&
           a.specularExponent == b.specularExponent &&
           floatBits(a.specular) == floatBits(b.specular);
}

// -- state ---------------------------------------------------------------------------------------

// Why the second eye walked a car the first eye had listed (or had not).
enum Refused : int {
    kNotListed,    // the first eye's pass of this kind did not list the car
    kFirstEye,     // the first eye's walk of it was not shareable (ShareRefusal, counted there)
    kInputs,       // the car's root, the pass's masks, MATERIAL state or lighting, or the bake moved
    kRegisters,    // a register a selector read from the pass's file
    kPose,         // a pose word or child pointer of an XFORM the walk entered
    kSprite,       // a BILLBOARD colour (the helmet tint)
    kGplMemo,      // GPL's vertex memo holds one of the walk's vertices
    kPoolFull,     // GPL's vertex memo too full to scan
    kNoPool,       // GPL's vertex memo was not placed at install
    kFrames,       // the list's frames are not the walk's, or the tape names no walk frame
    kFault,        // the replay faulted on a read
    kRefusedCount
};

enum ReplayResult {
    kReplayAgrees,
    kReplayDiffers,
    kReplayRegisters,
    kReplayPose,
    kReplaySprite,
    kReplayGplMemo,
    kReplayPoolFull,
    kReplayFrames,
    kReplayFault,
};

struct Second {
    uint32_t passesFirst = 0;    // first-eye passes that recorded
    uint32_t passesSecond = 0;   // second-eye passes with the first eye's lists of this frame
    uint32_t passesAlone = 0;    // second-eye passes without (no such first-eye pass this frame)
    uint32_t passesUnkeyed = 0;  // past the mirror slots, or outside the eye loop
    uint32_t passesOff = 0;      // in the eye loop with bakedCarShare 0 or debug view 16
    uint32_t tapes = 0;
    uint32_t tapeRefused[kShareRefusalCount] = {};
    uint64_t gates = 0;  // over the shareable tapes
    uint64_t boxes = 0;
    uint64_t selectors = 0;
    uint64_t frames = 0;
    uint64_t registers = 0;
    uint64_t sprites = 0;
    uint32_t calls = 0;  // first calls of a car in second-eye passes with lists
    uint32_t reused = 0;
    uint32_t reWalked = 0;
    uint32_t differLists[3] = {};  // re-walked lists with a gate, a box, a selector differing
    uint64_t differTotal[3] = {};  // decisions differing, by kind, over the re-walked lists
    uint32_t differCount[4] = {};  // re-walked lists by decisions differing: 1, 2-3, 4-7, 8+
    uint32_t verified = 0;
    uint32_t mismatched = 0;
    uint32_t verifyFaulted = 0;
    uint32_t refused[kRefusedCount] = {};
    int64_t  replayTicks = 0;
    uint32_t replays = 0;
    uint64_t poolEntries = 0;
    uint32_t poolScans = 0;
    // Scans that met a slot in the tape's address range whose source is none of its vertices,
    // which the range alone refused (report 105 section 4.43).
    uint32_t poolNarrowed = 0;
    char     firstMismatch[256] = {};
    // Eye-independent lists (report 105 section 4.22). First eye: its union walks, their margins,
    // conditions and order nodes, the leaves of their lists it does not draw itself, the walks
    // thrown away and done again deciding every gate, and recording passes with the union off.
    uint32_t unionWalks = 0;
    double   marginSum = 0.0;
    uint64_t conditions = 0;
    uint64_t orders = 0;
    uint64_t unionLeaves = 0;
    uint64_t otherLeaves = 0;
    uint32_t unionRedone[kUnionRedoneCount] = {};
    uint32_t unionOff = 0;
    // Second eye: lists taken with conditions, their draws it does not draw, the order nodes whose
    // plane passed between the eyes with both reordered sides drawn (and both blended), and the
    // union lists verified.
    uint32_t takenConditioned = 0;
    uint64_t takenOff = 0;
    uint64_t takenDraws = 0;
    uint32_t ordersFlipped = 0;
    uint32_t ordersFlippedBlended = 0;
    uint32_t unionVerified = 0;
    float    separationMax = 0.0f;  // the largest separation of the eyes measured, metres
};

// A first-eye pass's lists and tapes, kept for the second eye's pass of the same slot.
struct Store {
    bool     valid = false;
    uint32_t frame = 0;
    uint32_t count = 0;
    std::vector<baked::CarDrawList> lists;
    std::vector<ShareTape>          tapes;
    std::vector<uint8_t>            taken;  // handed to the second eye (or walked by it) already
};

struct Pass {
    bool     recording = false;  // a first-eye pass: its walks record tapes
    bool     reusing = false;    // a second-eye pass with its slot's store of this frame
    uint32_t frame = 0;
    int      slot = 0;           // 0 the main pass, 1.. the mirror passes in the eye's order
};

Store                  g_stores[kSlots];
Pass                   g_pass;
std::vector<ShareTape> g_tapes;  // the current pass's, by arena slot
uint32_t               g_ordinalFrame = 0;
int                    g_ordinalEye = -1;
int                    g_mainOrdinal = 0;
int                    g_mirrorOrdinal = 0;
uint32_t               g_candidates = 0;
baked::CarDrawList*    g_verify = nullptr;
// The order nodes of the union list a verify walk is compared with (carShareVerifyOrders).
const std::vector<ShareOrder>* g_verifyOrders = nullptr;
Second                 g_second;

// The separation of the two eyes' cameras at a car's dispatch, per slot: the largest measured
// this second and in the last one. A union walk's margin follows it.
float g_separationNow[kSlots] = {};
float g_separationLast[kSlots] = {};

// The replayed frames: clip from local and the eye, by walk frame id.
float g_clips[kMaxFrames][16];
float g_eyes[kMaxFrames][3];
// The replayed tape's conditions for this eye, CarDrawList::conditionOn's layout, and its order
// nodes this eye is on the other side of (with both sides drawn; of them, both blended).
std::vector<uint8_t> g_conditionOn;
uint32_t             g_orderFlips[2] = {};

// GPL's vertex memo, as gplMemoSlot reads it: a vertex of the walk is GPL's when its record is
// the source of a slot below the pool index. A source in [low, high] is looked up among the
// walk's records (report 105 section 4.43); one that is not among them is counted.
ReplayResult scanPool(const ShareTape& tape, const ShareCall& call, Second& t) {
    if (tape.high < tape.low) return kReplayAgrees;
    int32_t used = at<int32_t>(call.poolIndex);
    if (used <= 0) return kReplayAgrees;
    t.poolEntries += static_cast<uint32_t>(used);
    ++t.poolScans;
    if (used > kMaxPoolScan) return kReplayPoolFull;
    bool inRange = false;
    for (int32_t i = 0; i < used; ++i) {
        uint32_t source = at<uint32_t>(call.poolSources + 4u * static_cast<uint32_t>(i));
        if (source < tape.low || source > tape.high) continue;
        if (shareReadsVertex(tape, source)) return kReplayGplMemo;
        inRange = true;
    }
    if (inRange) ++t.poolNarrowed;
    return kReplayAgrees;
}

// The second eye's replay of a first-eye tape: the reads GPL may have moved, then every decision
// from this eye's camera. Counts the decisions that differ, by kind, to the end of the tape.
ReplayResult replay(const ShareTape& tape, const ShareCall& call, uint32_t differ[3], Second& t) {
    for (const ShareRegister& r : tape.registers)
        if (floatBits(static_cast<float>(call.registers[r.index & 0x3FF])) != r.valueBits)
            return kReplayRegisters;
    size_t frames = tape.frames.size() + 1;
    if (frames > kMaxFrames) return kReplayFrames;
    for (const ShareFrame& f : tape.frames) {
        for (unsigned i = 0; i < 6; ++i)
            if (word(f.node, 1 + i) != f.words[i]) return kReplayPose;
        if (word(f.node, 8) != f.words[6]) return kReplayPose;
    }
    for (const ShareSprite& s : tape.sprites)
        if (spriteColour(s.node) != s.colour) return kReplaySprite;
    ReplayResult pool = scanPool(tape, call, t);
    if (pool != kReplayAgrees) return pool;

    std::memcpy(g_clips[0], call.clip, sizeof(g_clips[0]));
    std::memcpy(g_eyes[0], call.eye, sizeof(g_eyes[0]));
    for (size_t k = 0; k < tape.frames.size(); ++k) {
        const ShareFrame& f = tape.frames[k];
        if (f.parent > k) return kReplayFrames;
        enterPose(g_clips[f.parent], g_eyes[f.parent], f.pose, g_clips[k + 1], g_eyes[k + 1]);
    }
    // The union's conditions for this eye, parents before children; what this eye does not meet
    // it does not decide.
    g_conditionOn.assign(tape.conditions.size() + 1, 0);
    g_conditionOn[0] = 1;
    for (size_t k = 0; k < tape.conditions.size(); ++k) {
        const ShareCondition& c = tape.conditions[k];
        if (c.frame >= frames || c.parent > k || c.kind != kConditionSide) return kReplayFrames;
        g_conditionOn[k + 1] =
            g_conditionOn[c.parent] && frontSide(g_eyes[c.frame], c.plane) == c.front ? 1 : 0;
    }
    auto meets = [](uint16_t condition) {
        return condition < g_conditionOn.size() && g_conditionOn[condition] != 0;
    };
    // Where this eye keeps the first eye's order although its own walk would not: an order node
    // it meets, on the other side of the node's plane, with both reordered sides drawing.
    g_orderFlips[0] = g_orderFlips[1] = 0;
    for (const ShareOrder& o : tape.orders) {
        if (!o.bothDrawn || !meets(o.condition)) continue;
        if (o.frame >= frames) return kReplayFrames;
        if (frontSide(g_eyes[o.frame], o.plane) == o.front) continue;
        ++g_orderFlips[0];
        if (o.bothBlended) ++g_orderFlips[1];
    }
    for (const ShareGate& g : tape.gates) {
        if (!meets(g.condition)) continue;
        if (g.frame >= frames) return kReplayFrames;
        if (frontSide(g_eyes[g.frame], g.plane) != g.front) ++differ[0];
    }
    for (const ShareBox& b : tape.boxes) {
        if (!meets(b.condition)) continue;
        uint32_t common = 2047;
        for (unsigned corner = 0; corner < 8; ++corner) {
            if (b.corner[corner] >= frames) return kReplayFrames;
            float p[3];
            vertexAt(word(b.node, 1 + corner), p);
            common &= frustumOutcode(g_clips[b.corner[corner]], p) << 6;
        }
        if ((common != 0) != b.culled) ++differ[1];
    }
    for (const ShareSelector& s : tape.selectors) {
        if (!meets(s.condition)) continue;
        float value = 0.0f;
        if (s.kind == 0) {
            if (s.frame >= frames) return kReplayFrames;
            float p[3];
            vertexAt(s.param, p);
            value = clipW(g_clips[s.frame], p) * call.lodScale;
            if (value < 0.0f) value = 0.0f;
        } else {
            value = s.kind == 1 ? call.dlong : call.dlat;
        }
        if (selectorChoice(s.node, value) != s.chosen) ++differ[2];
    }
    return differ[0] || differ[1] || differ[2] ? kReplayDiffers : kReplayAgrees;
}

// The tape reads GPL's graph exactly where the first eye's walk read it this frame, but a fault
// ends the replay, not the game.
ReplayResult replayGuarded(const ShareTape& tape, const ShareCall& call, uint32_t differ[3],
                           Second& t) {
    __try {
        return replay(tape, call, differ, t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kReplayFault;
    }
}

// What the call reads at the car's dispatch, besides the decisions and the camera, against what
// the first eye read: the car's root, the pass's masks and lighting, the MATERIAL state (the root
// material of the two lists) and the bake.
bool sameInputs(const ShareTape& tape, const ShareCall& call, const baked::CarDrawList& fresh,
                const baked::CarDrawList& stored) {
    return tape.root == call.root && tape.handle == call.handle && tape.driver == call.driver &&
           tape.classVisible == call.classVisible && tape.lightingOn == call.lightingOn &&
           tape.textureList == call.textureList && tape.texturedMask == call.texturedMask &&
           call.planeMask == 0 && !call.cellPlanes &&
           tape.generation == carBakeGeneration() &&
           tape.rebaked == carBakeTotals().objectsRebaked && fresh.driver == stored.driver &&
           fresh.cockpit == stored.cockpit && fresh.mirror == stored.mirror && !stored.flagman &&
           !fresh.materials.empty() && !stored.materials.empty() &&
           sameMaterial(fresh.materials[0], stored.materials[0]);
}

// The first eye's list with this eye's camera fields: the header from `fresh` (begun for this
// call), every frame's eye from the replay.
void patchList(baked::CarDrawList& list, const baked::CarDrawList& fresh) {
    std::memcpy(list.rootModelView, fresh.rootModelView, sizeof(list.rootModelView));
    std::memcpy(list.projection, fresh.projection, sizeof(list.projection));
    std::memcpy(list.light, fresh.light, sizeof(list.light));
    list.walkSlot = fresh.walkSlot;
    for (size_t k = 0; k < list.frames.size() && k < kMaxFrames; ++k)
        std::memcpy(list.frames[k].eye, g_eyes[k], sizeof(list.frames[k].eye));
}

ShareOutcome take(const ShareCall& call, baked::CarDrawList& list, Second& t) {
    Store&   store = g_stores[g_pass.slot];
    uint32_t j = 0;
    while (j < store.count && (store.tapes[j].record != call.record || store.taken[j])) ++j;
    if (j >= store.count) {
        ++t.refused[kNotListed];
        return kShareWalk;
    }
    store.taken[j] = 1;
    const ShareTape&    tape = store.tapes[j];
    baked::CarDrawList& stored = store.lists[j];
    // Both cameras' eyes at the same car's dispatch, in the dispatch frame: the separation the
    // next frame's union margin follows (the poses are rigid, so it is the same in every frame).
    // Every tape of the first eye's pass, walked or skipped, holds that pass's eye.
    {
        float dx = call.eye[0] - tape.rootEye[0];
        float dy = call.eye[1] - tape.rootEye[1];
        float dz = call.eye[2] - tape.rootEye[2];
        float separation = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(separation)) {
            float& now = g_separationNow[g_pass.slot];
            now = std::max(now, separation);
            t.separationMax = std::max(t.separationMax, separation);
        }
    }
    if (tape.refusal != kShareOk) {
        ++t.refused[kFirstEye];
        return kShareWalk;
    }
    if (!call.poolIndex || !call.poolSources) {
        ++t.refused[kNoPool];
        return kShareWalk;
    }
    if (!sameInputs(tape, call, list, stored)) {
        ++t.refused[kInputs];
        return kShareWalk;
    }
    if (stored.frames.size() != tape.frames.size() + 1) {
        ++t.refused[kFrames];
        return kShareWalk;
    }
    uint32_t differ[3] = {};
    switch (replayGuarded(tape, call, differ, t)) {
    case kReplayAgrees: break;
    case kReplayDiffers: {
        ++t.reWalked;
        uint32_t total = 0;
        for (int kind = 0; kind < 3; ++kind) {
            if (differ[kind]) ++t.differLists[kind];
            t.differTotal[kind] += differ[kind];
            total += differ[kind];
        }
        ++t.differCount[total == 1 ? 0 : total < 4 ? 1 : total < 8 ? 2 : 3];
        return kShareWalk;
    }
    case kReplayRegisters: ++t.refused[kRegisters]; return kShareWalk;
    case kReplayPose: ++t.refused[kPose]; return kShareWalk;
    case kReplaySprite: ++t.refused[kSprite]; return kShareWalk;
    case kReplayGplMemo: ++t.refused[kGplMemo]; return kShareWalk;
    case kReplayPoolFull: ++t.refused[kPoolFull]; return kShareWalk;
    case kReplayFrames: ++t.refused[kFrames]; return kShareWalk;
    case kReplayFault: ++t.refused[kFault]; return kShareWalk;
    }
    patchList(stored, list);
    if (!tape.conditions.empty()) {
        stored.conditionOn = g_conditionOn;
        ++t.takenConditioned;
        t.takenDraws += stored.draws.size();
        for (const baked::CarDraw& draw : stored.draws)
            if (!baked::carDrawOn(stored, draw)) ++t.takenOff;
        t.ordersFlipped += g_orderFlips[0];
        t.ordersFlippedBlended += g_orderFlips[1];
    }
    if (++g_candidates % kShareVerifyEvery == 0) {
        g_verify = &stored;
        g_verifyOrders = tape.orders.empty() ? nullptr : &tape.orders;
        return kShareVerify;
    }
    std::swap(list, stored);
    ++t.reused;
    return kShareReused;
}

bool sameBits(const void* a, const void* b, size_t size) { return std::memcmp(a, b, size) == 0; }

// Field by field, floats by their bits. False with the first difference in `why`.
bool sameList(const baked::CarDrawList& a, const baked::CarDrawList& b, char* why, size_t size) {
    auto differs = [&](const char* what, size_t index, size_t of) {
        std::snprintf(why, size, "driver %d, %s %s: element %zu of %zu", b.driver,
                      b.mirror ? "mirror" : "main", what, index, of);
        return false;
    };
    if (a.driver != b.driver || a.cockpit != b.cockpit || a.mirror != b.mirror ||
        a.flagman != b.flagman)
        return differs("driver, cockpit, mirror or flagman", 0, 1);
    if (!sameBits(a.rootModelView, b.rootModelView, sizeof(a.rootModelView)))
        return differs("root model-view", 0, 1);
    if (!sameBits(a.projection, b.projection, sizeof(a.projection)))
        return differs("projection", 0, 1);
    if (!sameBits(a.light, b.light, sizeof(a.light))) return differs("light", 0, 1);
    if (a.walkSlot != b.walkSlot) return differs("walk slot", 0, 1);
    if (!sameBits(a.palette, b.palette, sizeof(a.palette))) return differs("palette", 0, 1);
    if (a.polygons != b.polygons || a.emptyLeaves != b.emptyLeaves ||
        a.dynamicLeaves != b.dynamicLeaves)
        return differs("leaf counts", 0, 1);
    if (a.frames.size() != b.frames.size()) return differs("frame count", 0, b.frames.size());
    for (size_t i = 0; i < a.frames.size(); ++i) {
        const baked::CarFrame& x = a.frames[i];
        const baked::CarFrame& y = b.frames[i];
        if (!sameBits(x.rootFromLocal, y.rootFromLocal, sizeof(x.rootFromLocal)))
            return differs("frame root from local", i, b.frames.size());
        if (!sameBits(x.eye, y.eye, sizeof(x.eye))) return differs("frame eye", i, b.frames.size());
        if (x.halfFrom != y.halfFrom) return differs("frame half vector", i, b.frames.size());
    }
    if (a.materials.size() != b.materials.size())
        return differs("material count", 0, b.materials.size());
    for (size_t i = 0; i < a.materials.size(); ++i)
        if (!sameMaterial(a.materials[i], b.materials[i]))
            return differs("material", i, b.materials.size());
    if (a.draws.size() != b.draws.size()) return differs("draw count", 0, b.draws.size());
    for (size_t i = 0; i < a.draws.size(); ++i) {
        const baked::CarDraw& x = a.draws[i];
        const baked::CarDraw& y = b.draws[i];
        if (x.object != y.object || x.firstIndex != y.firstIndex || x.indexCount != y.indexCount ||
            x.polygons != y.polygons || x.frame != y.frame || x.material != y.material ||
            x.colour != y.colour || x.flags != y.flags || x.sprite != y.sprite)
            return differs("draw", i, b.draws.size());
    }
    if (a.dynamicVertices.size() != b.dynamicVertices.size())
        return differs("skinned vertex count", 0, b.dynamicVertices.size());
    for (size_t i = 0; i < a.dynamicVertices.size(); ++i)
        if (!sameBits(&a.dynamicVertices[i], &b.dynamicVertices[i], sizeof(baked::CarVertex)))
            return differs("skinned vertex", i, b.dynamicVertices.size());
    if (a.dynamicIndices.size() != b.dynamicIndices.size() ||
        (!a.dynamicIndices.empty() &&
         !sameBits(a.dynamicIndices.data(), b.dynamicIndices.data(),
                   a.dynamicIndices.size() * sizeof(uint32_t))))
        return differs("skinned indices", 0, b.dynamicIndices.size());
    return true;
}

}  // namespace

// -- recording -----------------------------------------------------------------------------------

void shareFrame(ShareTape& tape, uintptr_t node, uint16_t parent, uint16_t id,
                const float pose[12]) {
    if (id != tape.frames.size() + 1 || parent > tape.frames.size()) {
        if (tape.refusal == kShareOk) tape.refusal = kShareFrames;
        return;
    }
    ShareFrame frame;
    frame.node = node;
    for (unsigned i = 0; i < 6; ++i) frame.words[i] = word(node, 1 + i);
    frame.words[6] = word(node, 8);
    std::memcpy(frame.pose, pose, sizeof(frame.pose));
    frame.parent = parent;
    tape.frames.push_back(frame);
}

void shareBox(ShareTape& tape, uintptr_t node, const uint16_t corner[8], bool culled,
              uint16_t condition) {
    ShareBox box;
    box.node = node;
    std::memcpy(box.corner, corner, sizeof(box.corner));
    box.condition = condition;
    box.culled = culled;
    tape.boxes.push_back(box);
}

void shareSelector(ShareTape& tape, uintptr_t node, uint32_t kind, uint32_t param, uint16_t frame,
                   float value, uint16_t condition, bool fromWrite) {
    if (kind == 4) {
        uint32_t index = param & 0x3FF;
        if (!fromWrite && !(tape.written[index >> 5] & (1u << (index & 31))))
            tape.registers.push_back(ShareRegister{index, floatBits(value)});
        return;
    }
    // Kind 3 (the clock) and anything past 4 are taken as 0 whatever the eye.
    if (kind > 2) return;
    ShareSelector selector;
    selector.node = node;
    selector.param = param;
    selector.frame = frame;
    selector.condition = condition;
    selector.kind = static_cast<uint8_t>(kind);
    selector.chosen = selectorChoice(node, value);
    tape.selectors.push_back(selector);
}

uint16_t shareCondition(ShareTape& tape, uintptr_t plane, uint16_t frame, uint16_t parent,
                        bool front, bool eyeFront) {
    if (tape.conditions.size() >= kMaxShareConditions || parent > tape.conditions.size()) return 0;
    ShareCondition condition;
    condition.plane = plane;
    condition.frame = frame;
    condition.parent = parent;
    condition.front = front;
    condition.holds = (parent == 0 || tape.conditions[parent - 1].holds) && front == eyeFront;
    tape.conditions.push_back(condition);
    return static_cast<uint16_t>(tape.conditions.size());
}

uint16_t shareBoxCondition(ShareTape& tape, size_t box, uint16_t parent, bool drawn) {
    if (tape.conditions.size() >= kMaxShareConditions || parent > tape.conditions.size() ||
        box >= tape.boxes.size())
        return 0;
    ShareCondition condition;
    condition.plane = box;
    condition.frame = 0;
    condition.parent = parent;
    condition.front = true;
    condition.holds = (parent == 0 || tape.conditions[parent - 1].holds) && drawn;
    condition.kind = kConditionBox;
    tape.conditions.push_back(condition);
    return static_cast<uint16_t>(tape.conditions.size());
}

uint16_t shareChildCondition(ShareTape& tape, uintptr_t child, uint16_t frame, uint16_t parent,
                             bool current) {
    if (tape.conditions.size() >= kMaxShareConditions || parent > tape.conditions.size())
        return 0;
    ShareCondition condition;
    condition.plane = child;
    condition.frame = frame;
    condition.parent = parent;
    condition.front = true;
    condition.holds = (parent == 0 || tape.conditions[parent - 1].holds) && current;
    condition.kind = kConditionChild;
    tape.conditions.push_back(condition);
    return static_cast<uint16_t>(tape.conditions.size());
}

void shareSprite(ShareTape& tape, uintptr_t node, uint32_t colour) {
    tape.sprites.push_back(ShareSprite{node, colour});
}

// -- the car hook --------------------------------------------------------------------------------

void carSharePassBegin(bool mirror, bool enabled, std::vector<baked::CarDrawList>& lists,
                       uint32_t count) {
    Second& t = g_second;
    if (g_pass.recording) {
        Store& store = g_stores[g_pass.slot];
        std::swap(store.lists, lists);
        std::swap(store.tapes, g_tapes);
        store.count = std::min<uint32_t>(
            count, static_cast<uint32_t>(std::min(store.lists.size(), store.tapes.size())));
        store.frame = g_pass.frame;
        store.valid = true;
        store.taken.assign(store.count, 0);
    }
    g_pass = Pass{};
    g_verify = nullptr;
    g_verifyOrders = nullptr;

    uint32_t frame = 0;
    int      eye = 0;
    if (!stereoEyePass(frame, eye)) {
        ++t.passesUnkeyed;
        return;
    }
    if (frame != g_ordinalFrame || eye != g_ordinalEye) {
        g_ordinalFrame = frame;
        g_ordinalEye = eye;
        g_mainOrdinal = 0;
        g_mirrorOrdinal = 0;
    }
    int ordinal = mirror ? g_mirrorOrdinal++ : g_mainOrdinal++;
    if (!enabled || !g_options.bakedCarShare || bakedPassCarsUnshared()) {
        ++t.passesOff;
        return;
    }
    if (eye < 0 || eye > 1 || (mirror ? ordinal >= kMaxMirrorSlots : ordinal != 0)) {
        ++t.passesUnkeyed;
        return;
    }
    g_pass.frame = frame;
    g_pass.slot = mirror ? 1 + ordinal : 0;
    if (eye == 0) {
        g_pass.recording = true;
        ++t.passesFirst;
        if (carShareUnionMargin() <= 0.0f) ++t.unionOff;
        return;
    }
    const Store& store = g_stores[g_pass.slot];
    g_pass.reusing = store.valid && store.frame == frame;
    if (g_pass.reusing)
        ++t.passesSecond;
    else
        ++t.passesAlone;
}

ShareOutcome carShareTake(const ShareCall& call, baked::CarDrawList& list) {
    if (!g_pass.reusing) return kShareWalk;
    Second& t = g_second;
    ++t.calls;
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    ShareOutcome outcome = take(call, list, t);
    QueryPerformanceCounter(&end);
    t.replayTicks += end.QuadPart - start.QuadPart;
    ++t.replays;
    return outcome;
}

ShareTape* carShareTapeBegin(const ShareCall& call, uint32_t slot) {
    if (!g_pass.recording) return nullptr;
    if (g_tapes.size() <= slot) g_tapes.resize(slot + 1);
    ShareTape& tape = g_tapes[slot];
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
    tape.margin = carShareUnionMargin();
    tape.conditions.clear();
    tape.orders.clear();
    tape.skins.clear();
    tape.skinFrames.clear();
    std::memcpy(tape.rootEye, call.eye, sizeof(tape.rootEye));
    return &tape;
}

void carShareTapeSkip(const ShareCall& call, uint32_t slot) {
    if (!g_pass.recording) return;
    if (g_tapes.size() <= slot) g_tapes.resize(slot + 1);
    ShareTape& tape = g_tapes[slot];
    tape.record = call.record;
    tape.refusal = kShareProgram;
    std::memcpy(tape.rootEye, call.eye, sizeof(tape.rootEye));
    tape.vertices.clear();
    tape.frames.clear();
    tape.gates.clear();
    tape.boxes.clear();
    tape.selectors.clear();
    tape.registers.clear();
    tape.sprites.clear();
    tape.conditions.clear();
    tape.orders.clear();
    ++g_second.tapes;
    ++g_second.tapeRefused[kShareProgram];
}

float carShareUnionMargin() {
    if (!g_pass.recording || !g_options.bakedCarUnion || bakedPassCarsUnconditioned()) return 0.0f;
    float separation = std::max(g_separationNow[g_pass.slot], g_separationLast[g_pass.slot]);
    if (separation <= 0.0f) return kDefaultUnionMargin;
    return std::min(1.25f * separation + 0.002f, kMaxUnionMargin);
}

void carShareUnionRedone(UnionRedone reason) {
    if (reason >= 0 && reason < kUnionRedoneCount) ++g_second.unionRedone[reason];
}

const std::vector<ShareOrder>* carShareVerifyOrders() { return g_verify ? g_verifyOrders : nullptr; }

void shareSealVertices(ShareTape& tape) {
    std::sort(tape.vertices.begin(), tape.vertices.end());
    tape.vertices.erase(std::unique(tape.vertices.begin(), tape.vertices.end()),
                        tape.vertices.end());
}

bool shareReadsVertex(const ShareTape& tape, uint32_t record) {
    if (tape.vertices.empty()) return true;
    return std::binary_search(tape.vertices.begin(), tape.vertices.end(), record);
}

void carShareTapeEnd(ShareTape& tape, ShareRefusal refusal, baked::CarDrawList& list) {
    if (tape.refusal == kShareOk) tape.refusal = refusal;
    shareSealVertices(tape);
    Second& t = g_second;
    if (tape.margin > 0.0f) {
        // The walking eye draws the sides it is on; the list keeps the others for other eyes.
        if (!tape.conditions.empty()) {
            list.conditionOn.assign(tape.conditions.size() + 1, 0);
            list.conditionOn[0] = 1;
            for (size_t k = 0; k < tape.conditions.size(); ++k)
                list.conditionOn[k + 1] = tape.conditions[k].holds ? 1 : 0;
        }
        ++t.unionWalks;
        t.marginSum += tape.margin;
        t.conditions += tape.conditions.size();
        t.orders += tape.orders.size();
        t.unionLeaves += list.polygons;
        for (const baked::CarDraw& draw : list.draws)
            if (!baked::carDrawOn(list, draw)) t.otherLeaves += draw.polygons;
    }
    ++t.tapes;
    if (tape.refusal < kShareRefusalCount) ++t.tapeRefused[tape.refusal];
    if (tape.refusal != kShareOk) return;
    t.gates += tape.gates.size();
    t.boxes += tape.boxes.size();
    t.selectors += tape.selectors.size();
    t.frames += tape.frames.size();
    t.registers += tape.registers.size();
    t.sprites += tape.sprites.size();
}

void carShareVerify(const baked::CarDrawList& walked, bool walkedWhole) {
    Second& t = g_second;
    const baked::CarDrawList* expected = g_verify;
    g_verify = nullptr;
    g_verifyOrders = nullptr;
    if (!expected) return;
    if (!walkedWhole) {
        ++t.verifyFaulted;
        return;
    }
    char why[sizeof(t.firstMismatch)];
    // A union list holds more than this eye draws: compared by what each draws (CarUnion.h).
    bool same = expected->conditionOn.empty() ? sameList(*expected, walked, why, sizeof(why))
                                              : carUnionSameDrawn(*expected, walked, why,
                                                                  sizeof(why));
    if (!expected->conditionOn.empty()) ++t.unionVerified;
    if (same) {
        ++t.verified;
        return;
    }
    if (!t.mismatched) std::memcpy(t.firstMismatch, why, sizeof(why));
    ++t.mismatched;
}

void carShareSecond() {
    Second& t = g_second;
    if (t.passesFirst || t.passesSecond || t.passesAlone || t.passesOff) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        double reWalked = t.reWalked ? static_cast<double>(t.reWalked) : 1.0;
        double replays = t.replays ? static_cast<double>(t.replays) : 1.0;
        uint32_t shareable = t.tapeRefused[kShareOk];
        double   tapes = shareable ? static_cast<double>(shareable) : 1.0;
        uint32_t notShared = 0;
        for (uint32_t n : t.refused) notShared += n;
        logf("baked cars: shared between eyes -- the second eye's first calls %u: REUSED %u, "
             "re-walked %u (a decision differed: lists with a gate %u, a box %u, a selector %u "
             "differing; per re-walked list %.2f gates, %.2f boxes, %.2f selectors; lists by "
             "decisions differing 1: %u, 2-3: %u, 4-7: %u, 8+: %u), verified %u, MISMATCH %u "
             "(must be 0), verify walks faulted %u; not shared %u: not listed by the first eye "
             "%u, its walk not shareable %u, pass inputs moved %u, registers moved %u, pose "
             "moved %u, helmet colour moved %u, GPL's vertex memo holds a vertex %u (scans whose "
             "slots in the walk's address range were none of its vertices, section 4.43: %u), "
             "GPL's memo too full to scan %u (%.0f entries a scan), GPL's memo not placed %u, "
             "frames %u, replay faulted %u; replay %.1f us a call",
             t.calls, t.reused, t.reWalked, t.differLists[0], t.differLists[1], t.differLists[2],
             t.differTotal[0] / reWalked, t.differTotal[1] / reWalked,
             t.differTotal[2] / reWalked, t.differCount[0], t.differCount[1], t.differCount[2],
             t.differCount[3], t.verified, t.mismatched, t.verifyFaulted, notShared,
             t.refused[kNotListed], t.refused[kFirstEye], t.refused[kInputs],
             t.refused[kRegisters], t.refused[kPose], t.refused[kSprite], t.refused[kGplMemo],
             t.poolNarrowed, t.refused[kPoolFull],
             t.poolScans ? static_cast<double>(t.poolEntries) / t.poolScans : 0.0,
             t.refused[kNoPool], t.refused[kFrames], t.refused[kFault],
             1e6 * static_cast<double>(t.replayTicks) / static_cast<double>(frequency.QuadPart) /
                 replays);
        logf("baked cars: shared between eyes, first eye -- %u tapes, %u shareable (not: walk "
             "faulted %u, list contract %u, GPL's memo before the walk %u, frames %u, refused "
             "leaves or overflow %u, cell planes %u, drawn from the car's kept list %u); per "
             "shareable tape %.0f gates, %.1f boxes, "
             "%.1f selectors, %.1f frames, %.1f register reads, %.1f sprites; passes: first eye "
             "%u, second eye with the first eye's lists %u, without %u, unkeyed %u, sharing off "
             "%u (bakedCarShare %d, debug view 16)",
             t.tapes, shareable, t.tapeRefused[kShareFaulted], t.tapeRefused[kShareContract],
             t.tapeRefused[kShareGplMemo], t.tapeRefused[kShareFrames],
             t.tapeRefused[kShareLeaves], t.tapeRefused[kSharePlanes],
             t.tapeRefused[kShareProgram], t.gates / tapes,
             t.boxes / tapes, t.selectors / tapes, t.frames / tapes, t.registers / tapes,
             t.sprites / tapes, t.passesFirst, t.passesSecond, t.passesAlone, t.passesUnkeyed,
             t.passesOff, g_options.bakedCarShare);
        if (t.mismatched)
            logf("baked cars: shared between eyes, first MISMATCH this second -- %s",
                 t.firstMismatch);
        double walks = t.unionWalks ? static_cast<double>(t.unionWalks) : 1.0;
        double taken = t.takenConditioned ? static_cast<double>(t.takenConditioned) : 1.0;
        logf("baked cars: eye-independent lists -- first eye: %u union walks, margin %.3f m (eye "
             "separation measured %.4f m), per list %.1f conditions, %.1f order nodes, %.1f of "
             "%.1f leaves only another eye draws (%.1f%%); walked again deciding every gate: "
             "memo %u, registers %u, conditions full %u; first-eye passes with the union off %u "
             "(bakedCarUnion %d, debug view 17); second eye: %u lists taken with conditions, "
             "%.1f of %.1f draws a list not its own; order nodes whose plane passed between the "
             "eyes with both sides drawn %u (both blended %u): there the second eye keeps the "
             "first eye's order; union lists verified %u (MISMATCH above)",
             t.unionWalks, t.marginSum / walks, t.separationMax, t.conditions / walks,
             t.orders / walks, t.otherLeaves / walks, t.unionLeaves / walks,
             t.unionLeaves ? 100.0 * static_cast<double>(t.otherLeaves) / t.unionLeaves : 0.0,
             t.unionRedone[kUnionMemo], t.unionRedone[kUnionRegister],
             t.unionRedone[kUnionConditions], t.unionOff, g_options.bakedCarUnion,
             t.takenConditioned, t.takenOff / taken, t.takenDraws / taken, t.ordersFlipped,
             t.ordersFlippedBlended, t.unionVerified);
    }
    for (int slot = 0; slot < kSlots; ++slot) {
        g_separationLast[slot] = g_separationNow[slot];
        g_separationNow[slot] = 0.0f;
    }
    g_second = Second{};
}

}  // namespace gplvr
