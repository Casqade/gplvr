#pragma once

// What the kept car lists (CarProgram.h) know of the camera a car call is drawn from, report 105
// section 4.54:
// - the pass's glass and eye: a mirror pass's glass is Stereo.cpp's pane whose camera the pass
//   renders from (-1: none of them, i.e. flat mode's single mirror or a camera Stereo.cpp did not
//   place), its eye the stereo eye loop's (-1 outside it); a mirror car's programs are keyed by
//   the glass;
// - the car's root box against that view. Every car model's `car` XFORM has
//   one child, a BOXCULL around the whole model (tools/3do/cartree.py, all ten cars18 teams), so a
//   call whose root box GPL's BOXCULL test culls draws nothing: in a plain walk the box returns, in
//   a kept list's walk it is a configuration box (it has every SETREG of the model below it) or a
//   box condition off for that eye, and a reuse replays the same outcodes. Such a mirror call is
//   not listed at all. One culled call in kCullCheckEvery is walked as before and listed, and its
//   list must come out empty; one that does not turns the skip off until the car set changes.

#include "baked_gpl/CarShare.h"

#include <cstdint>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

constexpr uint32_t kCullCheckEvery = 64;

struct PassView {
    bool     mirror = false;
    int      pane = -1;  // a mirror pass's glass, -1 not known
    int      eye = -1;   // the stereo eye, -1 outside the eye loop
    uint32_t frame = 0;  // the stereo frame (stereoEyePass), with `eye`
};

// From carProgramPassBegin, every baked pass: `pane` as BakedPass found it (stereoMirrorPaneOf).
void            carPassViewBegin(bool mirror, int pane);
const PassView& carPassView();

// For a mirror pass's car call before its kept list is tried:
enum CullVerdict {
    kCullGoOn,   // in view, or not tested: the call goes on to its kept list
    kCullSkip,   // its list would be empty: listed nothing
    kCullCheck,  // culled, and the one in kCullCheckEvery walked as before (no kept list is tried,
                 // so it builds no empty program); carCullListed then sees its list
};
CullVerdict carCullTest(const ShareCall& call);
// At every car call's carProgramTake, before carCullTest: a check the last call left pending was
// never listed (its walk faulted or was refused) and is dropped.
void carCullTakeBegin();
// From carProgramListed (CarWalk's drawOurs), with each list a car call listed this pass.
void carCullListed(const baked::CarDrawList& list);

}  // namespace gplvr
