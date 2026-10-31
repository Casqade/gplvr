#pragma once

// Configuration gates of the per-configuration car programs (report 105 section 4.24,
// `bakedCarConfigs`). A kept list's walk (CarProgram.h) takes every gate both ways; the
// stock-derived models (stock cars67, Beta, cars18 c01 c02 c04-c08) cannot be kept so: their
// sector SIDEPICKs write registers that selectors read outside the side that wrote them, and
// GPL's memo follows the per-sector copies of the rear structure (report 104 sections 1 and 3).
// A program walk therefore DECIDES, for its own eye, every gate that
// - holds a SETREG somewhere below it, over every child and selector case (found once per node
//   by a scan of GPL's graph and kept until the car set changes), or
// - a hazard of an earlier program walk named: a register or memo entry written under its side
//   and read where that side does not hold, or written in one half of it (an order node) and
//   read in the other half or after it.
// The same holds for BOXCULLs (first run, section 4.24.1): a register written inside a box -- the
// r12/r13 wheel-side blocks, the derived models' box around the sector machinery -- is read
// outside it, so a box with a SETREG below it, or one a hazard named, is decided too (drawn or
// culled for the building eye) instead of being walked under a box condition.
// The program then holds for every eye on the same sides of the decided gates (and the same
// culling of the decided boxes) it met; the car's other configurations get programs of their own
// (CarProgram.cpp). Whether a node is decided never decides correctness: the walk still refuses a
// list it cannot keep, and the replay still checks every decision.

#include <cstdint>

namespace gplvr {

// Whether a program walk decides gate `node` (SIDEGATE, SIDEPICK, BSP4, BSP3_ASYM) or BOXCULL
// `node` for its eye. Reads GPL's graph below the node the first time it is asked in a car set; a
// scan that cannot answer (a fault, a graph deeper than the walk's limit, a full memo) leaves the
// node undecided -- taken both ways, as in section 4.23 -- and is counted (`unanswered`).
bool carConfigDecides(uintptr_t node);

// A program walk's hazard named gate `node`: later program walks decide it. 1 when it is new to
// the set, 0 when it was there already, -1 when the set is full or the node is 0.
int carConfigLearn(uintptr_t node);

// Counts since the car set last changed (the gates are its nodes), and this second's.
struct ConfigCounts {
    uint32_t scanned = 0;      // gates and boxes the register scan answered
    uint32_t writers = 0;      // of them, with a SETREG below
    uint32_t nodes = 0;        // control nodes the scan memoised
    uint32_t tableKiB = 0;     // the memo's size
    uint32_t learned = 0;      // gates and boxes hazards named
    uint32_t learnedNow = 0;   // this second
    // This second: asks the scan could not answer (left undecided), and why: a fault, past the
    // depth limit, the memo full at its largest.
    uint32_t unanswered = 0;
    uint32_t scanFaults = 0;
    uint32_t scanDeep = 0;
    uint32_t scanFull = 0;
    uint32_t learnFull = 0;    // this second: a hazard named a node the full set could not take
    uint32_t generations = 0;  // car sets seen
};
ConfigCounts carConfigCounts();
// Once a second, after the counts were read: this second's are cleared.
void carConfigSecond();

}  // namespace gplvr
