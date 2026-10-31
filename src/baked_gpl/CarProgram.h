#pragma once

// Car lists kept across frames and passes (docs/audit-2026-09-18/105-baked-renderer-rewrite.md
// section 4.23, `bakedCarProgram`). Stage 1 (CarShare.h, section 4.22) lets the second eye of a
// frame reuse the first eye's list; the first eye still walks every car every frame. Here a car's
// list is walked once into a PROGRAM that holds for any eye and any pose of the same selection:
// - every gate is taken both ways under a condition (or, for a car whose memo the union cannot
//   follow, every gate within kProgramMargin of the eye, and every gate inside the four wheel
//   XFORMs, whose frames turn between frames); every BOXCULL's subtree is walked under a box
//   condition;
// - every BSP4 / BSP3_ASYM taken both ways keeps its two halves sealed (CarDrawList::sealed), so
//   each pass puts them in its own eye's order: GPL's order exactly, unlike stage 1;
// - the tape keeps every XFORM the walk entered, every decision, every register it read from the
//   pass's file, every sprite colour and the recipe of every skinned leaf.
// Each later car call in a baked main pass -- either eye, any frame -- re-reads the XFORM words
// GPL's pose wrote, re-makes every frame (clip, eye, root from local, half-vector frame) with the
// walk's own expressions, evaluates the conditions for its eye, replays the decided gates and the
// selectors, re-makes the skinned corners, and assembles the eye's list: the drawn draws in the
// eye's order. Anything the program did not cover -- a selection (LOD) that changed, a register,
// a child pointer (a tyre swap, a lost wheel), the helmet colour, the pass's inputs, a decided gate
// that turned -- rebuilds it; anything it cannot follow -- GPL's own vertex memo holding one of
// its vertices -- walks the car as before. One reuse in kProgramVerifyEvery is walked anyway and
// compared run by run (CarUnion.h).
//
// Per configuration (report 105 section 4.24, `bakedCarConfigs`): a program's walk decides the
// configuration gates (CarConfig.h) for its eye instead of taking them both ways, so the sector
// registers of the stock-derived models are one value within it; a car keeps up to
// kConfigsPerCar programs and each call takes the first whose replay agrees -- a configuration
// gate on another side, another LOD or tyre object makes it try the next before it builds. The
// programs are the car's own, not shared with the other cars of its team: the list's materials
// come from each driver's texture list (the root object R+52, report 103 section 3), whose
// repeats decide where the walk starts a new material, and its helmet sprites carry each driver's
// colour.
//
// Main passes, VR and flat mode; and with bakedCarProgramMirrors the mirror passes (section 4.42),
// whose programs are a car's own apart from its main passes' (the mirror's registers, LOD scale and
// camera); with bakedMirrorCarKey (section 4.54) a mirror car has a Car per glass (1) or per glass
// and eye (2), each with its own programs. The passes a program's age and a car's retries are
// counted in are the main passes.
//
// The player's cockpit (section 4.50, `bakedCockpitProgram`) keeps programs of its own in the main
// passes whose cockpit list holds the mirror glass (step 2, section 4.49): each assembly takes the
// glass's UVs from GPL's PRIM again, and a MISMATCH turns its programs off until the car set
// changes. Its counts are apart from the cars'.

#include "baked_gpl/CarShare.h"

#include <cstdint>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

constexpr uint32_t kProgramVerifyEvery = 64;
// The margin of a program whose every-gate walk the memo or the registers could not follow
// (cars18's stock-derived teams): the Python model (tools/3do/carprogram.py) found it free of
// hazards on most of them, its list ~1.1-1.4x one eye's, kept ~10-80 frames in racing motion.
constexpr float kProgramMargin = 0.3f;

enum ProgramOutcome {
    kProgramWalk,    // walk the car as before (stage 1, CarShare.h)
    kProgramReused,  // `list` holds the program's list for this call's eye and pose
    kProgramVerify,  // walk the car without a tape and hand the list to carProgramVerify
    kProgramBuild,   // walk the car into carProgramTapeBegin's tape and hand it to ...TapeEnd
    kProgramEmpty,   // a mirror car whose root box the view culls: list nothing (CarPassView.h)
};

// From carWalkInstall once stage 2b is on: logs whether the program runs.
void carProgramInstall();

// From carWalkPassBegin. `enabled`: stage 2b is on. `pane`: a mirror pass's glass, -1 not known
// (report 105 section 4.54, bakedMirrorCarKey).
void carProgramPassBegin(bool mirror, bool enabled, int pane);

// A car's first call in a baked main or mirror pass, `list` begun for it (header, root frame, root
// material). Leaves `list` alone unless the outcome is kProgramReused. In a mirror pass a car whose
// list would be empty comes back kProgramEmpty before any kept list is tried (bakedMirrorCarCull).
ProgramOutcome carProgramTake(const ShareCall& call, baked::CarDrawList& list);

// From CarWalk's drawOurs, with each list a car call listed (kept, taken or walked): a culled call
// let through as a check of the skip must have come out empty (CarPassView.h).
void carProgramListed(const baked::CarDrawList& list);

// For kProgramBuild: the tape the walk records, and its margin (0: every gate both ways).
ShareTape* carProgramTapeBegin(const ShareCall& call, float& margin);

// After that walk: `hazard` the first UnionRedone reason it met (-1 none), `refusal` what
// CarWalk's shareRefusal says of the list, `ticks` its time (QueryPerformanceCounter), `learned`
// the configuration gates its hazards taught CarConfig (new ones) and `unlearned` its hazards that
// named none, by UnionRedone (kUnionRedoneCount entries). True when the program keeps a copy of
// `list`; `list` then holds the walking eye's conditions. False: the caller walks the car as
// before.
bool carProgramTapeEnd(ShareTape& tape, int hazard, ShareRefusal refusal, baked::CarDrawList& list,
                       int64_t ticks, uint32_t learned, const uint32_t* unlearned);

// After the walk kProgramVerify asked for.
void carProgramVerify(const baked::CarDrawList& walked, bool walkedWhole, int64_t ticks);

// Once a second, from CarWalk's per-second report.
void carProgramSecond();

}  // namespace gplvr
