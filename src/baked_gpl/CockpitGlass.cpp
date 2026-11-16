#include "baked_gpl/CockpitGlass.h"

#include "baked/CarScene.h"

#include "Hooks.h"
#include "Log.h"
#include "MirrorConfig.h"
#include "MirrorUv.h"
#include "MirrorWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

// The 330P4's cockpit binds mirror.mip on 35 polygons, the most in either car set
// (MirrorGeometry.cpp); a walk that meets more than this refuses the cockpit's list.
constexpr uint32_t kMaxGlass = 128;

// cockpitGlassTurn's test that a polygon's frame is the car's axes moved: its plane within 3
// degrees of its glass's, and the move within a car's size.
constexpr float kTurnPlaneCosine = 0.9986f;
constexpr float kTurnFrameReach = 1.0f;
constexpr float kTurnDegree = 0.0174532925f;

struct Glass {
    uintptr_t handle = 0;
    uint32_t  generation = 0;
    uint32_t  count = 0;
    uintptr_t nodes[kMaxGlass] = {};
    bool      saidTurnRefused = false;
    bool      saidHoused = false;
};

Glass g_glass;

}  // namespace

void cockpitGlassFor(uintptr_t handle, uint32_t generation) {
    if (g_glass.handle == handle && g_glass.generation == generation) return;
    g_glass.handle = handle;
    g_glass.generation = generation;
    g_glass.count = 0;
}

bool cockpitGlassAdd(uintptr_t node) {
    uintptr_t* end = g_glass.nodes + g_glass.count;
    uintptr_t* place = std::lower_bound(g_glass.nodes, end, node);
    if (place != end && *place == node) return true;
    if (g_glass.count >= kMaxGlass) return false;
    std::memmove(place + 1, place, static_cast<size_t>(end - place) * sizeof(uintptr_t));
    *place = node;
    ++g_glass.count;
    return true;
}

const uintptr_t* cockpitGlassNodes(uint32_t& count) {
    count = g_glass.count;
    return g_glass.nodes;
}

bool cockpitGlassCorners(const baked::CarObject& object, const baked::CarLeaf& leaf,
                         uintptr_t node, baked::CarVertex* corners, uint32_t room,
                         uint32_t& count, uint32_t* baseOut) {
    const auto* words = reinterpret_cast<const uint32_t*>(node);
    count = words[3];
    // The fan's apex, the leaf's first index, is corner 1 (CarScene.cpp, appendGplFan).
    if (count < 3 || count > room || 3 * (count - 2) != leaf.indexCount ||
        leaf.firstIndex >= object.indices.size() || object.indices[leaf.firstIndex] == 0)
        return false;
    uint32_t base = object.indices[leaf.firstIndex] - 1;
    if (base + count > object.vertices.size()) return false;
    if (baseOut) *baseOut = base;
    const auto* uv = reinterpret_cast<const float*>(node + 16);
    for (uint32_t i = 0; i < count; ++i) {
        corners[i] = object.vertices[base + i];
        corners[i].uv[0] = uv[2 * i];
        corners[i].uv[1] = uv[2 * i + 1];
    }
    return true;
}

namespace {

// The housed glass (MirrorWindow.h): its corners stay where the model put them and are flagged for
// the shader, and MirrorWindow is given, in the polygon's frame, the aimed glass's centre `pivot`
// and its texture coordinate on the aimed plane at scale 1. That coordinate is the model glass's
// own -- an affine fit of the corners' UVs over the plane -- carried by the aim's turn:
// u(x) = u0 + B . (turn^-1 (x - pivot) + pivot - mean). MirrorWindow fits it to each eye's pane.
template <typename Turn>
void housedGlass(int glass, const float pivot[3], const float newell[3], const float mean[3],
                 const Turn& turn, baked::CarVertex* corners, uint32_t count,
                 const float shift[3]) {
    float n[3] = {newell[0], newell[1], newell[2]};
    float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length <= 0.0f) return;
    for (float& c : n) c /= length;
    // In-plane axes, and the corners' UVs fitted over them about the mean.
    float edge[3] = {n[1], -n[0], 0.0f};
    float edgeLength = std::sqrt(edge[0] * edge[0] + edge[1] * edge[1]);
    if (edgeLength < 1e-4f) {
        edge[0] = 0.0f;
        edge[1] = n[2];
        edge[2] = -n[1];
        edgeLength = std::sqrt(edge[1] * edge[1] + edge[2] * edge[2]);
    }
    for (float& c : edge) c /= edgeLength;
    float up[3] = {n[1] * edge[2] - n[2] * edge[1], n[2] * edge[0] - n[0] * edge[2],
                   n[0] * edge[1] - n[1] * edge[0]};
    float uMean[2] = {};
    for (uint32_t i = 0; i < count; ++i)
        for (int c = 0; c < 2; ++c) uMean[c] += corners[i].uv[c] / static_cast<float>(count);
    float xx = 0.0f, xy = 0.0f, yy = 0.0f, xu[2] = {}, yu[2] = {};
    for (uint32_t i = 0; i < count; ++i) {
        const float* p = corners[i].position;
        float        d[3] = {p[0] - mean[0], p[1] - mean[1], p[2] - mean[2]};
        float        x = d[0] * edge[0] + d[1] * edge[1] + d[2] * edge[2];
        float        y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        xx += x * x;
        xy += x * y;
        yy += y * y;
        for (int c = 0; c < 2; ++c) {
            xu[c] += x * (corners[i].uv[c] - uMean[c]);
            yu[c] += y * (corners[i].uv[c] - uMean[c]);
        }
    }
    float det = xx * yy - xy * xy;
    if (std::fabs(det) < 1e-12f) return;
    float turned[2][3];
    float atPivot[2];
    for (int c = 0; c < 2; ++c) {
        float a = (yy * xu[c] - xy * yu[c]) / det;
        float b = (xx * yu[c] - xy * xu[c]) / det;
        float gradient[3] = {a * edge[0] + b * up[0], a * edge[1] + b * up[1],
                             a * edge[2] + b * up[2]};
        atPivot[c] = uMean[c] + gradient[0] * (pivot[0] - mean[0]) +
                     gradient[1] * (pivot[1] - mean[1]) + gradient[2] * (pivot[2] - mean[2]);
        turn(gradient, turned[c]);
    }
    float centre[3] = {pivot[0] - shift[0], pivot[1] - shift[1], pivot[2] - shift[2]};
    mirrorWindowGlassSet(glass, centre, pivot, turned, atPivot);
    for (uint32_t i = 0; i < count; ++i)
        corners[i].flags = static_cast<uint16_t>(
            (corners[i].flags & ~baked::kCarLeafWindowGlassMask) | baked::kCarLeafWindow |
            (glass << baked::kCarLeafWindowGlassShift));
    if (!g_glass.saidHoused) {
        g_glass.saidHoused = true;
        logf("mirror in housing: the cockpit's glass %d is drawn where the model has it, textured "
             "per eye from its aimed plane; its polygon's frame sits %+.3f %+.3f %+.3f m from car "
             "axes (logged once)",
             glass, shift[0], shift[1], shift[2]);
    }
}

}  // namespace

void cockpitGlassTurn(uintptr_t node, baked::CarVertex* corners, uint32_t count) {
    if (!mirrorExact() || count < 3) return;
    int   glass = -1;
    float centre[3], normal[3], polyMean[3];
    if (!mirrorUvGlassOf(node, glass, centre, normal, polyMean)) return;
    // Flagged unaimed too: its pane's field is fitted to each eye.
    if (glass >= kMaxWindowGlasses) return;
    MirrorAim aim = mirrorAimFor(glass);

    // The polygon's own plane and centre, in its frame, against its glass's in car axes.
    float mean[3] = {}, newell[3] = {};
    for (uint32_t i = 0; i < count; ++i) {
        const float* a = corners[i].position;
        const float* b = corners[(i + 1) % count].position;
        for (int k = 0; k < 3; ++k) mean[k] += a[k] / static_cast<float>(count);
        newell[0] += (a[1] - b[1]) * (a[2] + b[2]);
        newell[1] += (a[2] - b[2]) * (a[0] + b[0]);
        newell[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    float length = std::sqrt(newell[0] * newell[0] + newell[1] * newell[1] + newell[2] * newell[2]);
    float along = length > 0.0f ? std::fabs(newell[0] * normal[0] + newell[1] * normal[1] +
                                            newell[2] * normal[2]) / length
                                : 0.0f;
    // Where the polygon's frame sits in car axes: the same corners' mean in both. The glass's
    // centre carried into that frame is the pivot.
    float shift[3] = {mean[0] - polyMean[0], mean[1] - polyMean[1], mean[2] - polyMean[2]};
    float apart = std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
    if (along < kTurnPlaneCosine || apart > kTurnFrameReach) {
        if (!g_glass.saidTurnRefused) {
            g_glass.saidTurnRefused = true;
            logf("mirror exact: glass %d's polygon %08X is not in car axes (its plane %.1f deg "
                 "from the glass's, its frame %.3f m from them) -- drawn where the model put it, "
                 "so its aim is not exact (logged once)",
                 glass, static_cast<unsigned>(node),
                 std::acos(along > 1.0f ? 1.0f : along) * 57.2957795f, apart);
        }
        return;
    }
    for (int k = 0; k < 3; ++k) centre[k] += shift[k];

    // The aim's turn, as MirrorShared.cpp's mirrorGlassView turns the normal.
    float cy = std::cos(aim.yaw * kTurnDegree), sy = std::sin(aim.yaw * kTurnDegree);
    float cp = std::cos(aim.pitch * kTurnDegree), sp = std::sin(aim.pitch * kTurnDegree);
    auto turn = [&](const float in[3], float out[3]) {
        float x = in[0] * cy - in[1] * sy;
        float y = in[0] * sy + in[1] * cy;
        out[0] = x * cp + in[2] * sp;
        out[1] = y;
        out[2] = -x * sp + in[2] * cp;
    };
    housedGlass(glass, centre, newell, mean, turn, corners, count, shift);
}

}  // namespace gplvr
