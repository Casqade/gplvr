#pragma once

// The cockpit's mirror glass (report 105 section 4.48): the polygon nodes of the player's cockpit
// under a MATERIAL binding the car set's mirror.mip. gplvr draws the cockpit from a list, but not
// these: MirrorUv rewrites their UVs in GPL's live PRIM (one region of mirror.mip per glass),
// roglv2 samples mirror.mip with t flipped, and the copy-out writes it every pass -- none of which
// a baked list carries. So the cockpit's walks leave them out of its list and add them here, and
// GPL's own draw of the cockpit runs with the node filter handing GPL these leaves alone
// (bakedCockpitGlassOnly), clipped per cell as before. Step 2 (section 4.49) lists them instead,
// with their live UVs (cockpitGlassCorners); the set then serves the passes that fall back to
// step 1.
//
// The set is kept across passes, since the second eye may take the first eye's list without a
// walk, and grows with every walk. A walk is of the whole cockpit where GPL's draws are per cell,
// and a second eye takes the first eye's list only when its replay found every decision the same
// (CarShare.h), so the set holds every glass node GPL's draws of a pass reach.

#include <cstdint>

namespace baked {
struct CarLeaf;
struct CarObject;
struct CarVertex;
}  // namespace baked

namespace gplvr {

// Starts the set again when the cockpit's handle or the car bake's generation is not the one it
// was filled for.
void cockpitGlassFor(uintptr_t handle, uint32_t generation);

// Adds a glass node; false when the set has no room for it.
bool cockpitGlassAdd(uintptr_t node);

// The set, ascending, for the node filter's binary search.
const uintptr_t* cockpitGlassNodes(uint32_t& count);

// Step 2 (report 105 section 4.49): the cockpit's list holds the glass, drawn over the eye image
// with a copy of mirror.mip's live texture. A glass leaf's corners are its baked ones (`leaf` of
// `object`, consecutive from corner 0 in node order) with the UVs GPL holds now at polygon node
// `node` -- words 4.., u and v per corner, as MirrorUv rewrites them per glass -- into `corners`
// (room for `room`). False, with nothing written, when the node's corner count is not the
// leaf's or its baked corners are not found.
bool cockpitGlassCorners(const baked::CarObject& object, const baked::CarLeaf& leaf,
                         uintptr_t node, baked::CarVertex* corners, uint32_t room,
                         uint32_t& count);

}  // namespace gplvr
