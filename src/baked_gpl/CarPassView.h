#pragma once

// What the kept car lists (CarProgram.h) know of the camera a car call is drawn from, report 105
// section 4.54:
// - the pass's glass and eye: a mirror pass's glass is Stereo.cpp's pane whose camera the pass
//   renders from (-1: none of them, i.e. flat mode's single mirror or a camera Stereo.cpp did not
//   place), its eye the stereo eye loop's (-1 outside it); `bakedMirrorCarKey` keys a mirror car's
//   programs by them;
// - the car's root box against that view (`bakedMirrorCarCull`). Every car model's `car` XFORM has
//   one child, a BOXCULL around the whole model (tools/3do/cartree.py, all ten cars18 teams), so a
//   call whose root box GPL's BOXCULL test culls draws nothing: in a plain walk the box returns, in
//   a kept list's walk it is a configuration box (it has every SETREG of the model below it) or a
//   box condition off for that eye, and a reuse replays the same outcodes. Such a mirror call is
//   not listed at all. One culled call in kCullCheckEvery is walked as before and listed, and its
//   list must come out empty; one that does not turns the skip off until the car set changes;
// - whether the two eyes of a frame took the same program for a car (per glass in the mirror
//   passes): the counter that says whether keying by eye would pay.

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

// A car's root box: the BOXCULL that is the `car` (or `cockpit`) XFORM's only child; 0 when the
// handle is not an XFORM or its child is not a BOXCULL, or a read faults.
uintptr_t carRootBox(uintptr_t handle);

// For a mirror pass's car call before its kept list is tried (counted by why):
enum CullVerdict {
    kCullGoOn,   // in view, or not tested: the call goes on to its kept list
    kCullSkip,   // its list would be empty: listed nothing
    kCullCheck,  // culled, and the one in kCullCheckEvery walked as before (no kept list is tried,
                 // so it builds no empty program); carCullListed then sees its list
};
CullVerdict carCullTest(const ShareCall& call);
// At every car call's carProgramTake, before carCullTest: a check the last call left pending was
// never listed (its walk faulted or was refused), counted.
void carCullTakeBegin();
// From carProgramListed (CarWalk's drawOurs), with each list a car call listed this pass.
void carCullListed(const baked::CarDrawList& list);

// What a car call (not the cockpit's) came to, for the eyes' agreement: a kept program taken
// (`config`, `lod`: its configuration's hashes, report 105 section 4.31), a build asked for, walked
// as before, or skipped off the view.
enum EyeCall : uint8_t { kEyeTook, kEyeBuilt, kEyeWalked, kEyeSkipped };
void carEyePairNote(uintptr_t record, EyeCall call, uint32_t config, uint32_t lod);

// CarProgram.cpp's census of its Cars, for the key's line.
struct CarKeyCensus {
    int      option = 0;        // bakedMirrorCarKey
    uint32_t mainCars = 0;      // Cars of main-pass cars (by record)
    uint32_t cockpitCars = 0;
    uint32_t mirrorRecord = 0;  // mirror Cars keyed by record alone
    uint32_t mirrorGlass = 0;   // by record and glass
    uint32_t mirrorEye = 0;     // by record, glass and eye
    uint32_t maxCars = 0;       // kMaxCars
    uint32_t noPane = 0;        // mirror first calls keyed by record: the glass not known
    uint32_t noEye = 0;         // keyed by record and glass: the eye not known (option 2)
};

// Once a second from carProgramSecond: the key's and the cull's lines, then the counts start again.
void carPassViewSecond(const CarKeyCensus& census);

}  // namespace gplvr
