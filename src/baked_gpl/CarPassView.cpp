#include "baked_gpl/CarPassView.h"

#include "Hooks.h"
#include "Log.h"
#include "Stereo.h"

#include "baked/CarScene.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarMemo.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

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

// enterPose's clip: the XFORM's child frame's clip from local.
void clipUnder(const float parentClip[16], const float m[12], float clip[16]) {
    for (int row = 0; row < 4; ++row) {
        const float* p = parentClip + row * 4;
        for (int column = 0; column < 4; ++column) {
            float sum = p[0] * m[column] + p[1] * m[4 + column] + p[2] * m[8 + column];
            if (column == 3) sum += p[3];
            clip[row * 4 + column] = sum;
        }
    }
}

// gpl_vertexTransformCached 0x4CFBB0's outcode, as CarWalk's frustumOutcode.
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

// A node's type as CarWalk's visit reads it: -1 for a negative type word (gpl_nullNode).
int nodeType(uintptr_t node) {
    auto typeWord = at<int32_t>(node);
    return typeWord < 0 ? -1 : static_cast<int>(typeWord & 0xFF);
}

uintptr_t rootBoxOf(uintptr_t handle) {
    int type = nodeType(handle);
    if (type != 0x0D && type != 0x13 && type != 0x16) return 0;
    uintptr_t box = word(handle, 8);
    return nodeType(box) == 0x19 ? box : 0;
}

// -- the root box against the pass's view ---------------------------------------------------------

enum RootTest : int {
    kRootInView,      // the root box is not culled
    kRootCulled,      // every corner off one side of the view: the list would be empty
    kRootNoMaterial,  // the root CarWalk starts from is not a MATERIAL
    kRootNoBox,       // the handle is not an XFORM whose child is a BOXCULL
    kRootGplMemo,     // GPL's vertex memo holds a corner: the walk would take its outcode
    kRootFault,       // a read faulted
    kRootTestCount
};

// What CarWalk's walk of this call meets first: the root MATERIAL, which visits the handle; the
// handle's XFORM, whose clip is the root clip under its pose; its child, the BOXCULL, whose eight
// corners are transformed in that frame and culled when all share an outcode bit (0x4D7210; the
// cell planes are off in a list's walk). No vertex is memoised before them in the walk, so a corner
// takes GPL's memo only where GPL holds it.
RootTest rootTest(const ShareCall& call) {
    if (nodeType(call.root) != 0x05) return kRootNoMaterial;
    uintptr_t box = rootBoxOf(call.handle);
    if (!box) return kRootNoBox;
    float pose[12];
    poseMatrix(call.handle, pose);
    float clip[16];
    clipUnder(call.clip, pose, clip);
    uint32_t common = 2047;
    uint32_t stale = 0;
    for (unsigned corner = 0; corner < 8; ++corner) {
        uintptr_t vertex = word(box, 1 + corner);
        if (carMemoGplSlot(vertex, stale)) return kRootGplMemo;
        float p[3] = {at<float>(vertex + 4), at<float>(vertex + 8), at<float>(vertex + 12)};
        common &= frustumOutcode(clip, p) << 6;
    }
    return common ? kRootCulled : kRootInView;
}

RootTest rootTestGuarded(const ShareCall& call) {
    __try {
        return rootTest(call);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kRootFault;
    }
}

uintptr_t rootBoxGuarded(uintptr_t handle) {
    __try {
        return rootBoxOf(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct CullSecond {
    uint32_t tested = 0;  // = skipped + inView + checks
    uint32_t skipped = 0;
    uint32_t inView = 0;
    uint32_t checks = 0;
    uint32_t checksEmpty = 0;
    uint32_t checksFull = 0;  // must be 0
    uint32_t checksLost = 0;  // a check's call was never listed
    uint32_t untested[kRootTestCount] = {};
    uint32_t optionOff = 0;
    uint32_t turnedOff = 0;  // calls while a failed check holds the skip off
    int      fullDriver = -1;
    uint32_t fullDraws = 0;
};

struct Cull {
    bool       off = false;  // a check found a list: off until the car set changes
    uint32_t   offGeneration = 0;
    uint32_t   culled = 0;  // culled calls, for kCullCheckEvery
    bool       pending = false;
    CullSecond second;
};

Cull g_cull;

// -- the two eyes of a frame ----------------------------------------------------------------------

// A car's first-eye call this frame, by (record, mirror, glass), waiting for the second eye's.
struct PairMark {
    uintptr_t record = 0;
    uint32_t  frame = 0;
    uint32_t  config = 0;
    uint32_t  lod = 0;
    int8_t    pane = -1;
    bool      mirror = false;
    bool      held = false;
    uint8_t   call = kEyeWalked;
};
constexpr uint32_t kPairSlots = 1024;
constexpr uint32_t kPairProbe = 8;

enum PairResult : int {
    kPairSame,       // both took a program, of one configuration and LOD
    kPairDifferent,  // both took a program, of different configurations or LODs
    kPairBuilt,      // either asked for a build
    kPairBothOff,    // both skipped off the view
    kPairOneOff,     // one skipped off the view
    kPairWalked,     // either was walked as before
    kPairResultCount
};

struct PairSecond {
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t unpaired = 0;  // a second-eye call with no first-eye call of the car this frame
    // Second-eye calls walked as before, which CarWalk then offers to CarShare (the first eye's
    // list of this pass kind).
    uint32_t secondWalked = 0;
    uint32_t result[kPairResultCount] = {};
};

PairMark   g_marks[kPairSlots];
PairSecond g_pairs[2];         // [mirror]
uint32_t   g_pairsNoEye = 0;   // calls outside the stereo eye loop
uint32_t   g_pairsNoRoom = 0;  // first-eye calls no slot of the probe could hold

PassView g_view;

PairResult pairResult(const PairMark& first, EyeCall second, uint32_t config, uint32_t lod) {
    auto a = static_cast<EyeCall>(first.call);
    if (a == kEyeBuilt || second == kEyeBuilt) return kPairBuilt;
    if (a == kEyeSkipped && second == kEyeSkipped) return kPairBothOff;
    if (a == kEyeSkipped || second == kEyeSkipped) return kPairOneOff;
    if (a == kEyeWalked || second == kEyeWalked) return kPairWalked;
    return first.config == config && first.lod == lod ? kPairSame : kPairDifferent;
}

}  // namespace

void carPassViewBegin(bool mirror, int pane) {
    PassView& v = g_view;
    v.mirror = mirror;
    v.pane = mirror ? pane : -1;
    uint32_t frame = 0;
    int      eye = -1;
    if (stereoEyePass(frame, eye)) {
        v.eye = eye;
        v.frame = frame;
    } else {
        v.eye = -1;
        v.frame = 0;
    }
    if (g_cull.pending) {
        g_cull.pending = false;
        ++g_cull.second.checksLost;
    }
}

const PassView& carPassView() { return g_view; }

uintptr_t carRootBox(uintptr_t handle) { return handle ? rootBoxGuarded(handle) : 0; }

CullVerdict carCullTest(const ShareCall& call) {
    Cull&       c = g_cull;
    CullSecond& s = c.second;
    if (!g_options.bakedMirrorCarCull) {
        ++s.optionOff;
        return kCullGoOn;
    }
    if (c.off && c.offGeneration != carBakeGeneration()) c.off = false;
    if (c.off) {
        ++s.turnedOff;
        return kCullGoOn;
    }
    RootTest test = rootTestGuarded(call);
    if (test != kRootInView && test != kRootCulled) {
        ++s.untested[test];
        return kCullGoOn;
    }
    ++s.tested;
    if (test == kRootInView) {
        ++s.inView;
        return kCullGoOn;
    }
    if (++c.culled % kCullCheckEvery == 0) {
        ++s.checks;
        c.pending = true;
        return kCullCheck;
    }
    ++s.skipped;
    return kCullSkip;
}

void carCullTakeBegin() {
    if (!g_cull.pending) return;
    g_cull.pending = false;
    ++g_cull.second.checksLost;
}

void carCullListed(const baked::CarDrawList& list) {
    Cull& c = g_cull;
    if (!c.pending) return;
    c.pending = false;
    uint32_t drawn = 0;
    if (list.contentKey) {
        drawn = static_cast<uint32_t>(list.drawOrder.size());
    } else {
        for (const baked::CarDraw& draw : list.draws)
            if (baked::carDrawOn(list, draw)) ++drawn;
    }
    if (!drawn) {
        ++c.second.checksEmpty;
        return;
    }
    if (!c.second.checksFull) {
        c.second.fullDriver = list.driver;
        c.second.fullDraws = drawn;
    }
    ++c.second.checksFull;
    c.off = true;
    c.offGeneration = carBakeGeneration();
}

void carEyePairNote(uintptr_t record, EyeCall call, uint32_t config, uint32_t lod) {
    const PassView& v = g_view;
    if (v.eye < 0 || v.eye > 1) {
        ++g_pairsNoEye;
        return;
    }
    PairSecond& t = g_pairs[v.mirror ? 1 : 0];
    auto        pane = static_cast<int8_t>(v.pane);
    uint32_t    hash = static_cast<uint32_t>(record >> 4) * 2654435761u ^
                    static_cast<uint32_t>(v.pane + 1) * 40503u ^ (v.mirror ? 1u : 0u);
    // A slot of another car may be taken over once its frame is two or more behind.
    PairMark* found = nullptr;
    PairMark* vacant = nullptr;
    for (uint32_t k = 0; k < kPairProbe && !found; ++k) {
        PairMark& m = g_marks[(hash + k) & (kPairSlots - 1)];
        if (m.record == record && m.pane == pane && m.mirror == v.mirror)
            found = &m;
        else if (!vacant && (!m.record || v.frame - m.frame >= 2))
            vacant = &m;
    }
    if (v.eye == 1) {
        ++t.second;
        if (call == kEyeWalked) ++t.secondWalked;
        if (!found || !found->held || found->frame != v.frame) {
            ++t.unpaired;
            return;
        }
        ++t.result[pairResult(*found, call, config, lod)];
        found->held = false;
        return;
    }
    ++t.first;
    PairMark* m = found ? found : vacant;
    if (!m) {
        ++g_pairsNoRoom;
        return;
    }
    m->record = record;
    m->pane = pane;
    m->mirror = v.mirror;
    m->frame = v.frame;
    m->held = true;
    m->call = call;
    m->config = config;
    m->lod = lod;
}

void carPassViewSecond(const CarKeyCensus& k) {
    const PairSecond& m = g_pairs[0];
    const PairSecond& r = g_pairs[1];
    uint32_t held = k.mainCars + k.cockpitCars + k.mirrorRecord + k.mirrorGlass + k.mirrorEye;
    logf("baked cars: Cars keyed by glass and eye (report 105 section 4.54, bakedMirrorCarKey %d) "
         "-- Cars held %u of %u: main %u, the cockpit %u, mirror by record %u, by record and glass "
         "%u, by record, glass and eye %u; mirror first calls keyed by record for want of the "
         "glass %u, by record and glass for want of the eye %u; the two eyes' calls of a car in a "
         "frame (same: the second eye took a program of the configuration and LOD the first eye "
         "took) -- main passes: first eye %u, second eye %u: same %u, different %u, one built %u, "
         "one walked as before %u, no first-eye call %u; second-eye calls walked as before, so "
         "offered the first eye's list (`shared between eyes`) %u; mirror passes, per glass: "
         "first eye %u, second eye %u: same %u, different %u, one built %u, both off the view %u, "
         "one off the view %u, one walked as before %u, no first-eye call %u; second-eye calls "
         "walked as before %u; calls outside the eye loop %u, first-eye calls with no room to "
         "note %u",
         k.option, held, k.maxCars, k.mainCars, k.cockpitCars, k.mirrorRecord, k.mirrorGlass,
         k.mirrorEye, k.noPane, k.noEye, m.first, m.second, m.result[kPairSame],
         m.result[kPairDifferent], m.result[kPairBuilt], m.result[kPairWalked], m.unpaired,
         m.secondWalked, r.first, r.second, r.result[kPairSame], r.result[kPairDifferent],
         r.result[kPairBuilt], r.result[kPairBothOff], r.result[kPairOneOff],
         r.result[kPairWalked], r.unpaired, r.secondWalked, g_pairsNoEye, g_pairsNoRoom);
    const CullSecond& c = g_cull.second;
    uint32_t untested = c.optionOff + c.turnedOff;
    for (uint32_t n : c.untested) untested += n;
    if (c.tested || untested) {
        logf("baked cars: mirror cars off the view (report 105 section 4.54, bakedMirrorCarCull "
             "%d) -- first calls in mirror passes tested %u: skipped %u (the root box culls: the "
             "list would be empty), in view %u, culled but listed as a check %u (1 in %u): empty "
             "%u, NOT empty %u (must be 0; one turns the skip off until the car set changes: %s), "
             "checks never listed %u; not tested: bakedMirrorCarCull 0 %u, the skip off after a "
             "check %u, the root not a MATERIAL %u, no root box %u, GPL's vertex memo holds a "
             "corner %u, a read faulted %u",
             g_options.bakedMirrorCarCull, c.tested, c.skipped, c.inView, c.checks,
             kCullCheckEvery, c.checksEmpty, c.checksFull, g_cull.off ? "OFF now" : "on",
             c.checksLost, c.optionOff, c.turnedOff, c.untested[kRootNoMaterial],
             c.untested[kRootNoBox], c.untested[kRootGplMemo], c.untested[kRootFault]);
        if (c.checksFull)
            logf("baked cars: mirror cars off the view, the first check NOT empty this second -- "
                 "driver %d, %u draws for the pass's eye",
                 c.fullDriver, c.fullDraws);
    }
    g_pairs[0] = PairSecond{};
    g_pairs[1] = PairSecond{};
    g_pairsNoEye = 0;
    g_pairsNoRoom = 0;
    g_cull.second = CullSecond{};
}

}  // namespace gplvr
