#pragma once

// Two-sided panels: opposite faces of one item lying on each other (report 105 section 4.17).
//
// A track object's sign or banner is two polygons back to back, the advert facing the track and a
// plain back facing away, a few centimetres apart or none. GPL paints both and orders them by the
// splitting node they hang under: from either side of its plane it paints the face turned to the
// eye last (monza10k's nbank.3do: the back copy under BSP4 0x0AF18 word 4 before the advert under
// word 5 from the track side). The bake's overlay pairing orders the two of one item by their
// emission order, which is GPL's order from the FRONT side of every splitting node only, so from
// the other side the back face took the higher overlay level and its polygon offset carried it
// over the advert. Such a pair is drawn one-sided instead: each face only while it faces the eye,
// which is GPL's result from both sides. The overlay graph itself is not changed.

#include "baked/Scene.h"

#include <cstdint>
#include <vector>

namespace baked {

// Opposite faces of one item each covering at least this share of the other's projected area.
constexpr float kTwinCover = 0.95f;

// What the bake knows of one face of a candidate pair, taken before the triangles are laid out.
struct TwinFace {
    uint32_t item = 0;
    uint32_t leaf = 0;   // Scene::orderLeaves: where it hangs in the item's order tree
    uint32_t piece = 0;  // Scene::pieces: the terms it is drawn under
    uint32_t firstVertex = 0;
    uint32_t count = 0;
    float    normal[3] = {0.0f, 0.0f, 0.0f};  // world, as authored (Newell)
    bool     decal = false;
    bool     walkOrdered = false;  // painted in GPL's walk order (Baker::finish), not by depth
    bool     cut = false;          // replaced by the pieces of the decal cut or the skirt sink
};

// `faces` holds the candidate pairs as consecutive entries, the face emitted earlier first. Adds
// a span to scene.oneSided for every face of a pair GPL orders by the eye's side of a splitting
// node in their own plane, painting the face turned to the eye last, and counts every pair left as
// before by its reason (OverlayStats::twin*).
void resolveTwoSided(Scene& scene, const std::vector<TwinFace>& faces);

}  // namespace baked
