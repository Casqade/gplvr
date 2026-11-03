#include "baked_gpl/CarShare.h"

#include "Stereo.h"

#include "baked/CarScene.h"
#include "baked_gpl/CarBake.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
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

// The replay's answer: the walk would take the same path, a decision differs, or an input the tape
// read moved (a register, a pose word or child pointer, a helmet colour, GPL's vertex memo holding
// one of the walk's vertices or too full to scan, a frame the tape cannot place, a fault).
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

// The separation of the two eyes' cameras at a car's dispatch, per slot: the largest measured
// this second and in the last one. A union walk's margin follows it.
float   g_separationNow[kSlots] = {};
float   g_separationLast[kSlots] = {};
int64_t g_separationSecond = 0;  // QueryPerformanceCounter at the start of `now`'s second

// The replayed frames: clip from local and the eye, by walk frame id.
float g_clips[kMaxFrames][16];
float g_eyes[kMaxFrames][3];
// The replayed tape's conditions for this eye, CarDrawList::conditionOn's layout.
std::vector<uint8_t> g_conditionOn;

// GPL's vertex memo, as gplMemoSlot reads it: a vertex of the walk is GPL's when its record is
// the source of a slot below the pool index. A source in [low, high] is looked up among the
// walk's records (report 105 section 4.43).
ReplayResult scanPool(const ShareTape& tape, const ShareCall& call) {
    if (tape.high < tape.low) return kReplayAgrees;
    int32_t used = at<int32_t>(call.poolIndex);
    if (used <= 0) return kReplayAgrees;
    if (used > kMaxPoolScan) return kReplayPoolFull;
    for (int32_t i = 0; i < used; ++i) {
        uint32_t source = at<uint32_t>(call.poolSources + 4u * static_cast<uint32_t>(i));
        if (source < tape.low || source > tape.high) continue;
        if (shareReadsVertex(tape, source)) return kReplayGplMemo;
    }
    return kReplayAgrees;
}

// The second eye's replay of a first-eye tape: the reads GPL may have moved, then every decision
// from this eye's camera, to the first that differs.
ReplayResult replay(const ShareTape& tape, const ShareCall& call) {
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
    ReplayResult pool = scanPool(tape, call);
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
    for (const ShareOrder& o : tape.orders)
        if (o.bothDrawn && meets(o.condition) && o.frame >= frames) return kReplayFrames;
    for (const ShareGate& g : tape.gates) {
        if (!meets(g.condition)) continue;
        if (g.frame >= frames) return kReplayFrames;
        if (frontSide(g_eyes[g.frame], g.plane) != g.front) return kReplayDiffers;
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
        if ((common != 0) != b.culled) return kReplayDiffers;
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
        if (selectorChoice(s.node, value) != s.chosen) return kReplayDiffers;
    }
    return kReplayAgrees;
}

// The tape reads GPL's graph exactly where the first eye's walk read it this frame, but a fault
// ends the replay, not the game.
ReplayResult replayGuarded(const ShareTape& tape, const ShareCall& call) {
    __try {
        return replay(tape, call);
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
           fresh.cockpit == stored.cockpit && fresh.overlay == stored.overlay &&
           fresh.mirror == stored.mirror && !stored.flagman &&
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

ShareOutcome take(const ShareCall& call, baked::CarDrawList& list) {
    Store&   store = g_stores[g_pass.slot];
    uint32_t j = 0;
    while (j < store.count && (store.tapes[j].record != call.record || store.taken[j])) ++j;
    if (j >= store.count) return kShareWalk;
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
        }
    }
    // The first eye's walk must have been shareable, GPL's vertex memo placed, the inputs the
    // same, and the replay must agree; anything else walks the car.
    if (tape.refusal != kShareOk || !call.poolIndex || !call.poolSources ||
        !sameInputs(tape, call, list, stored) || stored.frames.size() != tape.frames.size() + 1 ||
        replayGuarded(tape, call) != kReplayAgrees)
        return kShareWalk;
    patchList(stored, list);
    if (!tape.conditions.empty()) stored.conditionOn = g_conditionOn;
    std::swap(list, stored);
    return kShareReused;
}

}  // namespace

// -- recording -----------------------------------------------------------------------------------

void shareFrame(ShareTape& tape, uintptr_t node, const uint32_t words[7], uint16_t parent,
                uint16_t id, const float pose[12]) {
    if (id != tape.frames.size() + 1 || parent > tape.frames.size()) {
        if (tape.refusal == kShareOk) tape.refusal = kShareFrames;
        return;
    }
    ShareFrame frame;
    frame.node = node;
    std::memcpy(frame.words, words, sizeof(frame.words));
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
    // The separations measured this second become the last second's, which the margin keeps.
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (now.QuadPart - g_separationSecond >= frequency.QuadPart) {
        g_separationSecond = now.QuadPart;
        for (int slot = 0; slot < kSlots; ++slot) {
            g_separationLast[slot] = g_separationNow[slot];
            g_separationNow[slot] = 0.0f;
        }
    }
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

    uint32_t frame = 0;
    int      eye = 0;
    if (!stereoEyePass(frame, eye)) return;
    if (frame != g_ordinalFrame || eye != g_ordinalEye) {
        g_ordinalFrame = frame;
        g_ordinalEye = eye;
        g_mainOrdinal = 0;
        g_mirrorOrdinal = 0;
    }
    int ordinal = mirror ? g_mirrorOrdinal++ : g_mainOrdinal++;
    if (!enabled) return;
    if (eye < 0 || eye > 1 || (mirror ? ordinal >= kMaxMirrorSlots : ordinal != 0)) return;
    g_pass.frame = frame;
    g_pass.slot = mirror ? 1 + ordinal : 0;
    if (eye == 0) {
        g_pass.recording = true;
        return;
    }
    const Store& store = g_stores[g_pass.slot];
    g_pass.reusing = store.valid && store.frame == frame;
}

ShareOutcome carShareTake(const ShareCall& call, baked::CarDrawList& list) {
    if (!g_pass.reusing) return kShareWalk;
    return take(call, list);
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
}

float carShareUnionMargin() {
    if (!g_pass.recording) return 0.0f;
    float separation = std::max(g_separationNow[g_pass.slot], g_separationLast[g_pass.slot]);
    if (separation <= 0.0f) return kDefaultUnionMargin;
    return std::min(1.25f * separation + 0.002f, kMaxUnionMargin);
}

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
    // The walking eye draws the sides it is on; the list keeps the others for other eyes.
    if (tape.margin > 0.0f && !tape.conditions.empty()) {
        list.conditionOn.assign(tape.conditions.size() + 1, 0);
        list.conditionOn[0] = 1;
        for (size_t k = 0; k < tape.conditions.size(); ++k)
            list.conditionOn[k + 1] = tape.conditions[k].holds ? 1 : 0;
    }
}

}  // namespace gplvr
