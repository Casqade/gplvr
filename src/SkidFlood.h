#pragma once

// GPL's skid-mark flood marks the render tree's cells with the render walk's drawn bit.
//
// gpl_carUpdateWheelSpinAndTyreSmoke (sim thread) -> sub_4ABB60 -> sub_483380 projects a skid
// mark: sub_483550 floods the cells around the wheel through their neighbour links, marking each
// with `lock or [cell], 80000000h` so it is visited once, and sub_4834E0 takes the marks off again.
// Bit 31 of a node's type word is also gpl_nodeDrawAndMark's "drawn this pass" mark, which every
// dispatch in the render walk tests (gpl_nodeList, the cell's items, the segment walk). A skid flood
// that overlaps an eye's walk makes that walk skip every cell it has marked so far: the player's car
// and anything near it vanish from that eye for one frame (flat mode has the same race against its
// one walk). Caught 2026-10-17 by two instruments since removed (report 92): a walk trace saw a list
// skip the car's cell, and a hardware watchpoint saw 0x483592 set bit 31 on it and 0x483504 clear
// it.
//
// The fix moves the flood to bit 28, which nothing else in the exe uses on a node: the three
// 80000000h immediates in the two functions (the test at 0x483576, the mark at 0x483582 and the
// clear's mask at 0x4834ED, whose `not` makes the AND mask) become 10000000h. The flood still sees
// its own marks, and the walk no longer sees them.

namespace gplvr {

// At startup, in the front end, before any skid flood can run. Verifies all three instructions,
// patches all or nothing, logs either way. VR and flat mode alike.
void skidFloodInstall();

}  // namespace gplvr
