#pragma once

// Where this executable keeps the globals that src/ names by their address in the reference link.
//
// Several executables of GPL 1.2.0.2 exist and more can be made. Two links of the same build are
// known -- the retail gplc67.exe every address in src/ was read off, and the plain link the 2010
// beta/demo ships -- and on top of either, GEM+ and GPLPatchDLL rewrite bytes in place, add
// sections, and are free to produce a file nothing here has seen. Their CODE is the same file in
// both links: no function moved, grew or shrank, 10,387 identical FPO records. What differs is
// where the tail of `.data` sits, in bands, 8 or 16 bytes apart.
//
// That makes a stock-byte check on a hook site insufficient on its own. The bytes match in both
// links, so the check passes and the patch then uses a global belonging to the other one. Two
// separate questions have to be answered, and this module answers the first:
//
//   1. WHERE IS THE GLOBAL? -- rebase(). Read off the running image, below.
//   2. IS THIS THE CODE WE STUDIED? -- each site's own business, and the only thing that can
//      catch a byte patch. rebaseOperands() exists so a site can ask that question about code
//      that names a rebased global, which is otherwise unaskable: the global's address is part
//      of the bytes being compared.
//
// HOW THE DELTAS ARE DERIVED. Each band carries anchors: an instruction in vanilla `.text` whose
// imm32 operand names a known global in that band. The opcode bytes ahead of the operand are
// verified, the operand is read, and the band's delta is that value minus the reference link's.
// Nothing is matched against a list of known executables, so a link whose data sits somewhere
// new resolves on its own as long as its code is GPL's.
//
// Each band has two anchors naming different globals and they must agree. A band whose anchors
// disagree has a layout this module's band map does not describe, and it resolves to nothing
// rather than to the first answer. One further anchor sits BELOW the window, where no link has
// ever moved anything, and its delta must come out zero; if it does not, the picture of the
// image is wrong everywhere and every address is refused.
//
// WHAT CANNOT BE DERIVED: the band boundaries. They are measured from the two known links -- all
// 25,962 differing `.text` operand dwords, zero counterexamples -- and an image whose data is
// cut at different points would be misread near an edge, which is what the per-band agreement
// check is for. The edges are also not points: the highest operand at one delta and the lowest
// at the next leave unnamed bytes between them, and an address falling in such a gap is refused
// rather than assigned to a neighbour. Eight bytes at 0x0054C5B8 are refused outright: between
// the two known links that object changes place rather than sliding, so no delta covers it.
//
// A refusal is always 0, and 0 means do not install. There is no second-best address.

#include <cstddef>
#include <cstdint>

namespace gplvr {

// Verify the anchors and derive each band's delta. Called from DllMain before anything installs,
// and logs one line saying what it found. Idempotent.
void dataRebaseResolve();

// This executable's address for a global that src/ names by its reference-link address, or 0 when
// it cannot be placed. A caller that treats 0 as an address writes into whatever this image keeps
// there, which no stock-byte guard can see.
uintptr_t rebase(uintptr_t referenceVa);

// The four deltas as "+0/-8/-16/-8", with "refused" or "conflict" in place of a band that did not
// resolve, for another site's log line.
const char* rebaseSummary();

// Rewrite every little-endian imm32 equal to `from` with `to`, and say how many there were.
// A site comparing stock bytes that name a rebased global builds its expected bytes with this
// and checks the count against what the template is known to contain, so a template that has
// been edited out of step with the comment above it refuses instead of silently matching only
// the reference link.
unsigned rebaseOperands(unsigned char* bytes, size_t length, uintptr_t from, uintptr_t to);

}  // namespace gplvr
