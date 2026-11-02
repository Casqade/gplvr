#include "baked_gpl/CarPassView.h"

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

struct Cull {
    bool     off = false;  // a check found a list: off until the car set changes
    uint32_t offGeneration = 0;
    uint32_t culled = 0;  // culled calls, for kCullCheckEvery
    bool     pending = false;
};

Cull g_cull;

PassView g_view;

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
    g_cull.pending = false;
}

const PassView& carPassView() { return g_view; }

CullVerdict carCullTest(const ShareCall& call) {
    Cull& c = g_cull;
    if (c.off && c.offGeneration != carBakeGeneration()) c.off = false;
    if (c.off) return kCullGoOn;
    // A call the test cannot answer (no root MATERIAL or root box, GPL's memo holding a corner, a
    // fault) goes on to its kept list.
    if (rootTestGuarded(call) != kRootCulled) return kCullGoOn;
    if (++c.culled % kCullCheckEvery == 0) {
        c.pending = true;
        return kCullCheck;
    }
    return kCullSkip;
}

void carCullTakeBegin() { g_cull.pending = false; }

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
    if (!drawn) return;
    logf("baked cars: a mirror car its root box culls drew %u draws for driver %d's check walk -- "
         "the skip is off until the car set changes",
         drawn, list.driver);
    c.off = true;
    c.offGeneration = carBakeGeneration();
}

}  // namespace gplvr
