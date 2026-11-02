#pragma once

// A kept car program's draw order for one eye (CarDrawList::drawOrder; report 105 sections
// 4.29.1, 4.36, 4.38 and 4.53): the program's draws its condition bits select, each order node's
// halves in the eye's order. An assembly takes it from one of the program's memos when the bits
// made it before; a miss patches it from the memo nearest in bits or emits it whole (emitRange),
// and fills a memo with it. CarProgram.cpp holds the programs; this file holds
// their memos, the patch's index and the emission.

#include "baked_gpl/CarShare.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gplvr {

// Section 4.35.1: with 2, a third of reuses emitted their order afresh (section 4.36).
constexpr int      kOrderMemos = 4;
constexpr uint32_t kDynamicDraw = 0x80000000u;  // in OrderSource::drawPolygons

// Where an order node's emitted run lies in a memo's order (report 105 section 4.53): `start`
// draws past the start of its enclosing node's run (of the order, for a top-level node), `length`
// draws, holding `swaps` nodes put the other way round (itself included). Written for every node
// the emission visited, i.e. every node whose enclosing nodes were all on; a node whose own
// condition was off emitted nothing (length 0). Relative starts let a run copied whole carry its
// nested nodes' entries unchanged.
struct OrderRun {
    uint16_t start;
    uint16_t length;
    uint16_t swaps;
};

// The draws an eye draws under one set of condition bits, as an assembly emitted or patched them
// (report 105 section 4.29.1).
struct OrderMemo {
    std::vector<uint8_t>  on;  // the condition bits (CarDrawList::conditionOn's layout)
    std::vector<uint32_t> order;
    // Per order node, its run in `order` (section 4.53), when `hasRuns`: filled by an emission
    // or patch of a program the patch serves.
    std::vector<OrderRun> runs;
    bool                  hasRuns = false;
    uint32_t              polygons = 0;
    uint32_t              dynamicLeaves = 0;
    uint32_t              key = 0;       // CarDrawList::orderKey; 0: empty
    uint32_t              lastUsed = 0;  // the assembly's `now`
};

// What the patch reads of a kept program to find the order nodes a changed condition bit reaches
// (report 105 section 4.53), built when the program is kept. Per condition id (the bits' index),
// the draws whose condition it is and the order nodes whose `condition` or `onFront` it is, as
// offsets [first[c], first[c + 1]) into one array; per draw, its innermost enclosing order node;
// per order node, its enclosing one (kNoOrderNode: none). 16-bit throughout: a program with more
// draws, order nodes or node entries than that keeps no index and emits its orders whole.
constexpr uint16_t kNoOrderNode = 0xFFFF;
struct OrderIndex {
    std::vector<uint16_t> drawsFirst;
    std::vector<uint16_t> draws;
    std::vector<uint16_t> nodesFirst;
    std::vector<uint16_t> nodes;
    std::vector<uint16_t> drawNode;
    std::vector<uint16_t> nodeParent;
    bool                  built = false;
};

// A kept program's order state: its memos, the patch's index, and whether a patch check found a
// difference since it was kept (its orders are then emitted whole until it is rebuilt).
struct OrderState {
    OrderMemo  memos[kOrderMemos];
    OrderIndex index;
    bool       patchOff = false;
};

// What an emission reads: the program's order nodes (ShareTape::orders), its draws' conditions and
// packed polygon counts (Program::drawConditions, drawPolygons; one a draw), and this eye's
// condition bits.
struct OrderSource {
    const ShareOrder* nodes;
    uint32_t          nodeCount;
    const uint16_t*   drawConditions;
    uint32_t          draws;
    const uint32_t*   drawPolygons;
    const uint8_t*    bits;
    uint32_t          bitCount;
};

// This eye's drawOrder into `drawOrder` (report 105 sections 4.29.1, 4.53): a memo's whose bits
// equal `s.bits`; else patched from the nearest memo or emitted whole, and the least recently
// used memo filled with it (`now` marks a memo's use). A verify walk's (`verify`) is emitted whole
// and fills none. The memo the order came from or filled; nullptr for a verify walk's.
OrderMemo* orderTake(const OrderSource& s, OrderState& state, bool verify, uint32_t now,
                     std::vector<uint32_t>& drawOrder);

// Whether the order nodes nest as emitRange walks them: each inside its parent's half, in order.
bool ordersNest(const std::vector<ShareOrder>& orders, uint32_t begin, uint32_t end,
                uint32_t orderBegin, uint32_t orderEnd, int depth);

// The patch's index of a program being kept (section 4.53); false (and `index` empty) when the
// program is past its 16-bit ids. The order nodes must nest (ordersNest).
bool orderIndexBuild(OrderIndex& index, const OrderSource& s);
size_t orderIndexBytes(const OrderIndex& index);

}  // namespace gplvr
