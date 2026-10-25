#include "baked/TwoSided.h"

#include "baked/Piff.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace baked {
namespace {

// The splitting node's plane and the faces' within 25 degrees (kOverlayCosine).
constexpr float    kTwinPlaneCosine = 0.9f;
constexpr uint32_t kNoParent = 0xFFFFFFFFu;

struct Link {
    uint32_t node = kNoParent;
    uint32_t word = 0;  // the parent's word the child hangs under
};

// True when no pass draws both: one gate taken on both its sides, or one selector's cases that
// never meet.
bool termsExclude(const Scene& scene, uint32_t pieceA, uint32_t pieceB) {
    if (pieceA >= scene.pieces.size() || pieceB >= scene.pieces.size()) return false;
    const Piece& a = scene.pieces[pieceA];
    const Piece& b = scene.pieces[pieceB];
    for (uint32_t i = 0; i < a.termCount; ++i) {
        const Term& ta = scene.terms[a.firstTerm + i];
        for (uint32_t j = 0; j < b.termCount; ++j) {
            const Term& tb = scene.terms[b.firstTerm + j];
            if (ta.kind != tb.kind || ta.index != tb.index) continue;
            if (ta.kind == kTermGate && ta.mask != tb.mask) return true;
            if (ta.kind == kTermSelector &&
                (ta.caseBase != tb.caseBase || (ta.mask & tb.mask) == 0))
                return true;
        }
    }
    return false;
}

}  // namespace

// Where a splitting node's child, held in word `wordIndex`, comes in the order GPL draws that
// node's children with the eye on the given side. -1 when it is not drawn from that side.
// PROVEN from the node handlers: SIDEPICK 0x4D77C0, BSP4 0x4D76B0, BSP3_ASYM 0x4D75A0,
// BSP3 0x4D7480, BSP2 0x4D73A0.
int32_t orderDrawRank(uint8_t type, bool front, uint32_t wordIndex) {
    if (wordIndex < 2 || wordIndex > 5) return -1;
    size_t slot = wordIndex - 2;
    // rows: words 2, 3, 4, 5
    static const int8_t kSidePick[2][4] = {{0, -1, -1, -1}, {-1, 0, -1, -1}};
    static const int8_t kBsp4[2][4] = {{2, 1, -1, 0}, {0, -1, 1, 2}};
    static const int8_t kBsp3Asym[2][4] = {{2, 1, 0, -1}, {0, -1, 1, -1}};
    static const int8_t kBsp3[2][4] = {{2, 1, 0, -1}, {0, 1, 2, -1}};
    static const int8_t kBsp2[2][4] = {{1, 0, -1, -1}, {0, 1, -1, -1}};
    size_t side = front ? 0 : 1;
    switch (type) {
    case kNodeSidePick: return kSidePick[side][slot];
    case kNodeBsp4: return kBsp4[side][slot];
    case kNodeBsp3Asym: return kBsp3Asym[side][slot];
    case kNodeBsp3: return kBsp3[side][slot];
    case kNodeBsp2: return kBsp2[side][slot];
    default: return -1;
    }
}

void resolveTwoSided(Scene& scene, const std::vector<TwinFace>& faces) {
    OverlayStats& stats = scene.stats.overlay;
    stats.twinPairs = static_cast<uint32_t>(faces.size() / 2);
    if (faces.size() < 2) return;

    // Each order node's and leaf's parent, from the flattened trees of every item.
    std::vector<Link> nodeParent(scene.orderNodes.size());
    std::vector<Link> leafParent(scene.orderLeaves.size());
    for (uint32_t n = 0; n < scene.orderNodes.size(); ++n) {
        const OrderNode& node = scene.orderNodes[n];
        for (uint32_t c = 0; c < node.childCount; ++c) {
            const OrderChild& child = scene.orderChildren[node.firstChild + c];
            uint32_t          ref = child.ref & ~kOrderLeafBit;
            std::vector<Link>& parents = (child.ref & kOrderLeafBit) ? leafParent : nodeParent;
            if (ref < parents.size()) parents[ref] = Link{n, child.word};
        }
    }

    std::vector<Link> chain;
    for (size_t k = 0; k + 1 < faces.size(); k += 2) {
        const TwinFace& a = faces[k];
        const TwinFace& b = faces[k + 1];
        if (a.walkOrdered || b.walkOrdered) {
            ++stats.twinWalkOrdered;
            continue;
        }
        if (a.decal || b.decal) {
            ++stats.twinDecal;
            continue;
        }
        if (a.cut || b.cut) {
            ++stats.twinCut;
            continue;
        }
        if (termsExclude(scene, a.piece, b.piece)) {
            ++stats.twinNever;
            continue;
        }

        // The lowest order node above both, and the word each hangs under there. One leaf, the
        // item's root, a SORTGROUP or one word of a splitting node: GPL's order of the two is
        // the same from everywhere.
        uint32_t lowest = kNoParent;
        uint32_t wordA = 0;
        uint32_t wordB = 0;
        if (a.leaf != b.leaf && a.leaf < leafParent.size() && b.leaf < leafParent.size()) {
            chain.clear();
            for (Link link = leafParent[a.leaf]; link.node != kNoParent;
                 link = nodeParent[link.node])
                chain.push_back(link);
            for (Link link = leafParent[b.leaf]; link.node != kNoParent && lowest == kNoParent;
                 link = nodeParent[link.node]) {
                for (const Link& up : chain)
                    if (up.node == link.node) {
                        lowest = link.node;
                        wordA = up.word;
                        wordB = link.word;
                        break;
                    }
            }
        }
        uint8_t type = lowest == kNoParent ? 0 : scene.orderNodes[lowest].type;
        bool    splitting = type == kNodeBsp4 || type == kNodeBsp3Asym || type == kNodeBsp3 ||
                         type == kNodeBsp2;
        if (!splitting || wordA == wordB) {
            ++stats.twinFixed;
            continue;
        }

        // The splitting plane in world space (Gate::plane is in item space), against the faces.
        uint32_t gate = scene.orderNodes[lowest].gate;
        float    plane[3] = {0.0f, 0.0f, 0.0f};
        if (gate < scene.gates.size() && a.item < scene.items.size()) {
            const float* m = scene.items[a.item].itemFromWorld;
            const float* p = scene.gates[gate].plane;
            for (int j = 0; j < 3; ++j) plane[j] = m[j] * p[0] + m[4 + j] * p[1] + m[8 + j] * p[2];
        }
        float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        float agree = length > 0.0f ? (a.normal[0] * plane[0] + a.normal[1] * plane[1] +
                                       a.normal[2] * plane[2]) / length
                                    : 0.0f;
        if (std::abs(agree) < kTwinPlaneCosine) {
            ++stats.twinOffPlane;
            continue;
        }

        // From each side that draws both: the face GPL paints last, against the face turned to an
        // eye on that side (a's normal along the plane's means a faces its front side).
        bool together = false;
        bool away = false;
        for (bool front : {true, false}) {
            int32_t rankA = orderDrawRank(type, front, wordA);
            int32_t rankB = orderDrawRank(type, front, wordB);
            if (rankA < 0 || rankB < 0 || rankA == rankB) continue;
            together = true;
            bool laterIsA = rankA > rankB;
            bool facingIsA = (agree > 0.0f) == front;
            if (laterIsA != facingIsA) away = true;
        }
        if (!together) {
            ++stats.twinNever;
            continue;
        }
        if (away) {
            ++stats.twinAway;
            continue;
        }
        ++stats.twinOneSided;
        for (const TwinFace* face : {&a, &b}) {
            OneSidedSpan span;
            span.firstVertex = face->firstVertex;
            span.count = static_cast<uint16_t>(face->count);
            span.flipped = face->normal[2] < 0.0f ? 1 : 0;
            scene.oneSided.push_back(span);
        }
    }

    std::sort(scene.oneSided.begin(), scene.oneSided.end(),
              [](const OneSidedSpan& x, const OneSidedSpan& y) {
                  return x.firstVertex < y.firstVertex;
              });
    scene.oneSided.erase(std::unique(scene.oneSided.begin(), scene.oneSided.end(),
                                     [](const OneSidedSpan& x, const OneSidedSpan& y) {
                                         return x.firstVertex == y.firstVertex;
                                     }),
                         scene.oneSided.end());
    stats.twinPolygons = static_cast<uint32_t>(scene.oneSided.size());
}

}  // namespace baked
