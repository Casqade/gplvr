#pragma once

// Car lists kept across frames and passes (docs/audit-2026-09-18/105-baked-renderer-rewrite.md
// section 4.23). CarShare.h (section 4.22) lets the second eye of a frame reuse the first eye's
// list; here a car's list is walked once into a PROGRAM that holds for any eye and any pose of the
// same selection:
// - every gate is taken both ways under a condition (or, for a car whose memo the union cannot
//   follow, every gate within kProgramMargin of the eye, and every gate inside the four wheel
//   XFORMs, whose frames turn between frames); every BOXCULL's subtree is walked under a box
//   condition;
// - every BSP4 / BSP3_ASYM taken both ways keeps its two halves sealed (CarDrawList::sealed), so
//   each pass puts them in its own eye's order: GPL's order exactly;
// - the tape keeps every XFORM the walk entered, every decision, every register it read from the
//   pass's file, every sprite colour and the recipe of every skinned leaf.
// Each later car call in a baked pass -- either eye, any frame -- re-reads the XFORM words GPL's
// pose wrote, re-makes every frame (clip, eye, root from local, half-vector frame) with the walk's
// own expressions, evaluates the conditions for its eye, replays the decided gates and the
// selectors, re-makes the skinned corners, and assembles the eye's list: the drawn draws in the
// eye's order. Anything the program did not cover -- a selection (LOD) that changed, a register,
// a child pointer (a tyre swap, a lost wheel), the helmet colour, the pass's inputs, a decided gate
// that turned -- rebuilds it; anything it cannot follow -- GPL's own vertex memo holding one of
// its vertices -- walks the car as before.
//
// Per configuration (report 105 section 4.24): a program's walk decides the configuration gates
// (CarConfig.h) for its eye instead of taking them both ways, so the sector registers of the
// stock-derived models are one value within it; a car keeps up to kConfigsPerCar programs and each
// call takes the first whose replay agrees -- a configuration gate on another side, another LOD or
// tyre object makes it try the next before it builds. The programs are the car's own, not shared
// with the other cars of its team: the list's materials come from each driver's texture list (the
// root object R+52, report 103 section 3), whose repeats decide where the walk starts a new
// material, and its helmet sprites carry each driver's colour.
//
// Main passes, VR and flat mode, and the mirror passes (section 4.42), whose programs are a car's
// own apart from its main passes' (the mirror's registers, LOD scale and camera): a mirror car has
// a Car per glass (section 4.54), each with its own programs. The passes a program's age and a
// car's retries are counted in are the main passes.
//
// The player's cockpit (section 4.50) keeps programs of its own in the main passes whose cockpit
// list holds the mirror glass (step 2, section 4.49): each assembly takes the glass's UVs from
// GPL's PRIM again; one reuse in kCockpitVerifyEvery is walked anyway and compared run by run
// (CarUnion.h), and a MISMATCH turns its programs off until the car set changes.

#include "baked_gpl/CarShare.h"

#include <cstdint>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

// The margin of a program whose every-gate walk the memo or the registers could not follow
// (cars18's stock-derived teams): the Python model (tools/3do/carprogram.py) found it free of
// hazards on most of them, its list ~1.1-1.4x one eye's, kept ~10-80 frames in racing motion.
constexpr float kProgramMargin = 0.3f;

enum ProgramOutcome {
    kProgramWalk,    // walk the car as before (CarShare.h)
    kProgramReused,  // `list` holds the program's list for this call's eye and pose
    kProgramVerify,  // the cockpit: walk it without a tape and hand the list to carProgramVerify
    kProgramBuild,   // walk the car into carProgramTapeBegin's tape and hand it to ...TapeEnd
    kProgramEmpty,   // a mirror car whose root box the view culls: list nothing (CarPassView.h)
};

// From carWalkInstall once the car hook is in: logs what the program does.
void carProgramInstall();

// From carWalkPassBegin. `enabled`: the car hook is installed. `pane`: a mirror pass's glass, -1
// not known (report 105 section 4.54).
void carProgramPassBegin(bool mirror, bool enabled, int pane);

// Every kept program and Car dropped, as if no car had been seen: from BakedPass when
// bakedRenderer comes back on after passes GPL drew alone, whose frames no program followed.
void carProgramDropAll();

// A car's first call in a baked main or mirror pass, `list` begun for it (header, root frame, root
// material). Leaves `list` alone unless the outcome is kProgramReused. In a mirror pass a car whose
// list would be empty comes back kProgramEmpty before any kept list is tried.
ProgramOutcome carProgramTake(const ShareCall& call, baked::CarDrawList& list);

// From CarWalk's drawOurs, with each list a car call listed (kept, taken or walked): a culled call
// let through as a check of the skip must have come out empty (CarPassView.h).
void carProgramListed(const baked::CarDrawList& list);

// For kProgramBuild: the tape the walk records, and its margin (0: every gate both ways).
ShareTape* carProgramTapeBegin(const ShareCall& call, float& margin);

// After that walk: `hazard` the first UnionRedone reason it met (-1 none), `refusal` what
// CarWalk's shareRefusal says of the list, `learned` the configuration gates its hazards taught
// CarConfig (new ones). True when the program keeps a copy of `list`; `list` then holds the
// walking eye's conditions. False: the caller walks the car as before.
bool carProgramTapeEnd(ShareTape& tape, int hazard, ShareRefusal refusal, baked::CarDrawList& list,
                       uint32_t learned);

// After the walk kProgramVerify asked for.
void carProgramVerify(const baked::CarDrawList& walked, bool walkedWhole);

}  // namespace gplvr
