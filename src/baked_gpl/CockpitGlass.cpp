#include "baked_gpl/CockpitGlass.h"

#include "baked/CarScene.h"

#include <algorithm>
#include <cstring>

namespace gplvr {
namespace {

// The 330P4's cockpit binds mirror.mip on 35 polygons, the most in either car set
// (MirrorGeometry.cpp); a walk that meets more than this refuses the cockpit's list.
constexpr uint32_t kMaxGlass = 128;

struct Glass {
    uintptr_t handle = 0;
    uint32_t  generation = 0;
    uint32_t  count = 0;
    uintptr_t nodes[kMaxGlass] = {};
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
                         uint32_t& count) {
    const auto* words = reinterpret_cast<const uint32_t*>(node);
    count = words[3];
    // The fan's apex, the leaf's first index, is corner 1 (CarScene.cpp, appendGplFan).
    if (count < 3 || count > room || 3 * (count - 2) != leaf.indexCount ||
        leaf.firstIndex >= object.indices.size() || object.indices[leaf.firstIndex] == 0)
        return false;
    uint32_t base = object.indices[leaf.firstIndex] - 1;
    if (base + count > object.vertices.size()) return false;
    const auto* uv = reinterpret_cast<const float*>(node + 16);
    for (uint32_t i = 0; i < count; ++i) {
        corners[i] = object.vertices[base + i];
        corners[i].uv[0] = uv[2 * i];
        corners[i].uv[1] = uv[2 * i + 1];
    }
    return true;
}

}  // namespace gplvr
