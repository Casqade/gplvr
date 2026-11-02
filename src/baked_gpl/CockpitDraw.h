#pragma once

// GPL's own draw of the cockpit while gplvr draws it over the eye image (report 105 section 4.50,
// lever B of cockpit step 3). In step 2 (section 4.49) the node filter
// hands GPL no leaf of the cockpit, yet gpl_carRecordDraw still walks its control graph on every
// call (~1.6 an eye), ~72 us each. What that walk leaves behind for later readers (IDA, comment
// at 0x486E9C): the pose again, which gplvr's hook has already run for the record; and the
// cockpit's SETREG writes into the main register file, which persists across passes (report 104:
// r3 r4 r5 r14). The MATERIAL handlers restore their state, the XFORMs 0x0D / 0x13 pop the
// matrix and the vertex and normal memos back, and no leaf, line or polygon reaches GPL.
//
// So the draw is skipped and its register writes are reproduced: the values it leaves are an
// image learned from GPL's own draw, written into the file at each skipped call. A check call
// runs GPL's draw and compares the file it leaves with the file a skip would have left (the file
// before, with the image written over it): the first call after the cockpit or the car set
// changed, one call in kCheckEvery, and every call for a while after a check that differed, drew
// anything or overflowed the image. The first check of each kind that failed is logged.

#include <cstdint>

namespace gplvr {

enum CockpitGplDraw : int {
    kCockpitGplRun,    // GPL's draw runs as in step 2
    kCockpitGplSkip,   // skipped: the image's registers were written into the file
    kCockpitGplCheck,  // GPL's draw runs as a check: cockpitGplDrawChecked after it
};

// At a cockpit call whose list this pass is step 2's: `handle` the cockpit's XFORM (I+152),
// `generation` the car bake's, `registers` the pass's register file (gpl_backgroundTable).
int cockpitGplDrawAsk(uintptr_t handle, uint32_t generation, uintptr_t registers);

// After the check draw: leaves and lines the node filter handed GPL, and what GPL's polygon
// counter moved by (all three 0 in step 2).
void cockpitGplDrawChecked(uintptr_t registers, uint32_t passed, uint32_t lines,
                           uint32_t polygons);

}  // namespace gplvr
