#pragma once

// The contract of eye-independent car lists (docs/audit-2026-09-18/105-baked-renderer-rewrite.md
// section 4.22): a list the first eye's union walk built, with its conditions evaluated for the
// second eye (CarDrawList::conditionOn), must draw exactly what the second eye's own walk draws
// when that walk keeps the first eye's order at the order nodes the union took both ways
// (CarShare.h, ShareOrder). The two lists differ in layout -- the union list holds the other
// sides' frames, materials, skinned corners and draws, and a run the second eye's walk merged may
// be split around a draw of another side -- so they are compared by what is drawn: the draws each
// eye draws, merged into runs as its own list would have merged them (baked::carDrawContinues,
// which the renderer ranks by), run by run, each with its geometry, the contents of its frame and
// of the frame its half vector comes from, its material, colour, sprite and leaf count, floats by
// their bits.

#include <cstddef>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

// True when `a` and `b` draw the same for the eyes their conditions were evaluated for; false
// with the first difference in `why`.
bool carUnionSameDrawn(const baked::CarDrawList& a, const baked::CarDrawList& b, char* why,
                       size_t size);

}  // namespace gplvr
