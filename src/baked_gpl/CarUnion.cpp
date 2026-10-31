#include "baked_gpl/CarUnion.h"

#include "baked/CarScene.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

bool sameBits(const void* a, const void* b, size_t size) { return std::memcmp(a, b, size) == 0; }

uint32_t floatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// One run of the draws an eye draws: draws [first, end) of `drawn`.
struct Run {
    size_t first;
    size_t end;
};

// The draws `list` draws for its eye, and their runs.
void runsOf(const baked::CarDrawList& list, std::vector<uint32_t>& drawn, std::vector<Run>& runs) {
    drawn.clear();
    runs.clear();
    for (size_t i = 0; i < list.draws.size(); ++i) {
        const baked::CarDraw& draw = list.draws[i];
        if (!baked::carDrawOn(list, draw)) continue;
        if (drawn.empty() || !baked::carDrawContinues(list.draws[drawn.back()], draw))
            runs.push_back(Run{drawn.size(), drawn.size() + 1});
        else
            runs.back().end = drawn.size() + 1;
        drawn.push_back(static_cast<uint32_t>(i));
    }
}

bool sameFrame(const baked::CarDrawList& a, uint16_t fa, const baked::CarDrawList& b,
               uint16_t fb) {
    if (fa >= a.frames.size() || fb >= b.frames.size()) return false;
    const baked::CarFrame& x = a.frames[fa];
    const baked::CarFrame& y = b.frames[fb];
    return sameBits(x.rootFromLocal, y.rootFromLocal, sizeof(x.rootFromLocal)) &&
           sameBits(x.eye, y.eye, sizeof(x.eye));
}

bool sameMaterial(const baked::CarMaterial& a, const baked::CarMaterial& b) {
    return a.textureHandle == b.textureHandle && a.texture == b.texture &&
           a.surfaceClass == b.surfaceClass && a.textured == b.textured &&
           a.specularExponent == b.specularExponent &&
           floatBits(a.specular) == floatBits(b.specular) && a.liveMirror == b.liveMirror;
}

// The skinned corners a run draws, index by index, against the other run's.
bool sameSkinned(const baked::CarDrawList& a, const std::vector<uint32_t>& da, const Run& ra,
                 const baked::CarDrawList& b, const std::vector<uint32_t>& db, const Run& rb) {
    size_t ia = ra.first, ib = rb.first;
    uint32_t ka = 0, kb = 0;  // the position inside the current draw of each run
    while (ia < ra.end && ib < rb.end) {
        const baked::CarDraw& x = a.draws[da[ia]];
        const baked::CarDraw& y = b.draws[db[ib]];
        if (ka >= x.indexCount) {
            ++ia;
            ka = 0;
            continue;
        }
        if (kb >= y.indexCount) {
            ++ib;
            kb = 0;
            continue;
        }
        size_t xi = static_cast<size_t>(x.firstIndex) + ka;
        size_t yi = static_cast<size_t>(y.firstIndex) + kb;
        if (xi >= a.dynamicIndices.size() || yi >= b.dynamicIndices.size()) return false;
        uint32_t xv = a.dynamicIndices[xi];
        uint32_t yv = b.dynamicIndices[yi];
        if (xv >= a.dynamicVertices.size() || yv >= b.dynamicVertices.size()) return false;
        if (!sameBits(&a.dynamicVertices[xv], &b.dynamicVertices[yv], sizeof(baked::CarVertex)))
            return false;
        ++ka;
        ++kb;
    }
    // Whatever is left on either side must be empty draws.
    for (; ia < ra.end; ++ia, ka = 0)
        if (ka < a.draws[da[ia]].indexCount) return false;
    for (; ib < rb.end; ++ib, kb = 0)
        if (kb < b.draws[db[ib]].indexCount) return false;
    return true;
}

}  // namespace

bool carUnionSameDrawn(const baked::CarDrawList& a, const baked::CarDrawList& b, char* why,
                       size_t size) {
    auto differs = [&](const char* what, size_t index, size_t of) {
        std::snprintf(why, size, "driver %d, %s, union list %s: run %zu of %zu", b.driver,
                      b.mirror ? "mirror" : "main", what, index, of);
        return false;
    };
    if (a.driver != b.driver || a.cockpit != b.cockpit || a.overlay != b.overlay ||
        a.mirror != b.mirror || a.flagman != b.flagman)
        return differs("driver, cockpit, overlay, mirror or flagman", 0, 1);
    if (!sameBits(a.rootModelView, b.rootModelView, sizeof(a.rootModelView)))
        return differs("root model-view", 0, 1);
    if (!sameBits(a.projection, b.projection, sizeof(a.projection)))
        return differs("projection", 0, 1);
    if (!sameBits(a.light, b.light, sizeof(a.light))) return differs("light", 0, 1);
    if (a.walkSlot != b.walkSlot) return differs("walk slot", 0, 1);

    static std::vector<uint32_t> drawnA, drawnB;
    static std::vector<Run>      runsA, runsB;
    runsOf(a, drawnA, runsA);
    runsOf(b, drawnB, runsB);
    if (runsA.size() != runsB.size()) return differs("run count", runsA.size(), runsB.size());
    for (size_t r = 0; r < runsA.size(); ++r) {
        const Run& ra = runsA[r];
        const Run& rb = runsB[r];
        const baked::CarDraw& x = a.draws[drawnA[ra.first]];
        const baked::CarDraw& y = b.draws[drawnB[rb.first]];
        uint32_t polygonsA = 0, polygonsB = 0, indicesA = 0, indicesB = 0;
        uint16_t flagsA = 0, flagsB = 0;
        for (size_t i = ra.first; i < ra.end; ++i) {
            polygonsA += a.draws[drawnA[i]].polygons;
            indicesA += a.draws[drawnA[i]].indexCount;
            flagsA |= a.draws[drawnA[i]].flags;
        }
        for (size_t i = rb.first; i < rb.end; ++i) {
            polygonsB += b.draws[drawnB[i]].polygons;
            indicesB += b.draws[drawnB[i]].indexCount;
            flagsB |= b.draws[drawnB[i]].flags;
        }
        if (polygonsA != polygonsB || indicesA != indicesB || flagsA != flagsB)
            return differs("leaves, indices or flags", r, runsB.size());
        if (x.colour != y.colour || x.sprite != y.sprite)
            return differs("colour or sprite", r, runsB.size());
        bool dynamic = x.object == baked::kDynamicCarObject;
        if (dynamic != (y.object == baked::kDynamicCarObject))
            return differs("object", r, runsB.size());
        if (dynamic) {
            if (!sameSkinned(a, drawnA, ra, b, drawnB, rb))
                return differs("skinned corners", r, runsB.size());
        } else if (x.object != y.object || x.firstIndex != y.firstIndex) {
            return differs("object or index range", r, runsB.size());
        }
        if (!sameFrame(a, x.frame, b, y.frame)) return differs("frame", r, runsB.size());
        uint16_t halfA = x.frame < a.frames.size() ? a.frames[x.frame].halfFrom : 0;
        uint16_t halfB = y.frame < b.frames.size() ? b.frames[y.frame].halfFrom : 0;
        if (!sameFrame(a, halfA, b, halfB))
            return differs("frame of the half vector", r, runsB.size());
        if (x.material >= a.materials.size() || y.material >= b.materials.size() ||
            !sameMaterial(a.materials[x.material], b.materials[y.material]))
            return differs("material", r, runsB.size());
    }
    return true;
}

}  // namespace gplvr
