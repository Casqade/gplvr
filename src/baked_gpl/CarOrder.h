#pragma once

// A kept car program's draw order for one eye (CarDrawList::drawOrder; report 105 sections
// 4.29.1, 4.36, 4.38 and 4.53): the program's draws its condition bits select, each order node's
// halves in the eye's order. An assembly takes it from one of the program's memos when the bits
// made it before; a miss patches it from the memo nearest in bits (bakedOrderPatch) or emits it
// whole (emitRange), and fills a memo with it. CarProgram.cpp holds the programs; this file holds
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
    uint32_t              swapped = 0;
    uint32_t              key = 0;       // CarDrawList::orderKey; 0: empty
    uint32_t              lastUsed = 0;  // the assembly's `now`
    uint32_t              hits = 0;      // assemblies it served since it was filled (section 4.38)
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

// Why a memo miss's order was emitted whole (report 105 section 4.53).
enum OrderWhole : int {
    kWholeVerify,     // a verify walk's list: emitted whole, fills no memo
    kWholeOptionOff,  // bakedOrderPatch = 0
    kWholeNoIndex,    // the program is past the index's 16-bit ids
    kWholePatchOff,   // a patch check differed for this program since it was kept
    kWholeNoBase,     // no memo holds its runs yet
    kWholeSizes,      // the index, or every memo with runs, is of other condition or node counts
    kWholeTooMany,    // more than kOrderPatchMaxBits bits differ from every memo with runs
    kWholeBounds,     // a run past the base's order or the buffer (must be 0)
    kWholeCheck,      // the patch check differed: the whole order is used (must be 0)
    kOrderWholeCount
};

// The order's counts over a second (Second::order in CarProgram.cpp).
struct OrderSecond {
    uint32_t memoised = 0;  // assemblies whose order came from a memo (section 4.29.1)
    // Memo misses (verify walks' included) and their ticks, the patch checks' excluded (4.35).
    uint64_t missTicks = 0;
    uint32_t misses = 0;
    // Section 4.38, over the whole emissions that fill a memo: their ticks emitting (emitRange),
    // copying the order into the list and filling the memo; the program's draws walked, order
    // nodes visited and draws emitted. Memos refilled, and of them those that had served no
    // assembly since they were filled.
    uint64_t emitTicks = 0;
    uint64_t copyTicks = 0;
    uint64_t fillTicks = 0;
    uint32_t timed = 0;
    uint64_t draws = 0;
    uint64_t nodes = 0;
    uint64_t emitted = 0;
    uint32_t refilled = 0;
    uint32_t unused = 0;
    // Section 4.53. Misses patched, and the others by OrderWhole; per patched miss the condition
    // bits that differed from its base memo, the order nodes marked as reached by them, the nodes
    // visited and the program's, runs copied whole and the draws in them, the draws drawn, bases
    // that were the program's most recently used memo; the ticks finding the base and its changed
    // bits (with the marking), patching, and copying into the list with the memo's fill. Patched
    // orders emitted again whole and compared, and those that differed (must be 0). Programs kept
    // without an index.
    uint32_t patched = 0;
    uint32_t whole[kOrderWholeCount] = {};
    uint64_t patchBits = 0;
    uint64_t patchMarked = 0;
    uint64_t patchVisited = 0;
    uint64_t patchNodes = 0;
    uint64_t patchRuns = 0;
    uint64_t patchCopied = 0;
    uint64_t patchDrawn = 0;
    uint32_t patchRecent = 0;
    uint64_t patchFindTicks = 0;
    uint64_t patchEmitTicks = 0;
    uint64_t patchFillTicks = 0;
    uint32_t checks = 0;
    uint32_t checksDiffered = 0;
    uint32_t indexRefused = 0;
};

// What orderTake returns: the memo the order came from or filled (nullptr for a verify walk's),
// the order nodes it put the other way round from the build, and the ticks of a patch check's
// whole emission, which the caller keeps out of its own timing.
struct OrderTaken {
    OrderMemo* memo = nullptr;
    uint32_t   swapped = 0;
    uint64_t   checkTicks = 0;
};

// This eye's drawOrder into `drawOrder` (report 105 sections 4.29.1, 4.53): a memo's whose bits
// equal `s.bits`; else patched from the nearest memo or emitted whole, and the least recently
// used memo filled with it (`now` marks a memo's use). A verify walk's (`verify`) is emitted whole
// and fills none.
OrderTaken orderTake(const OrderSource& s, OrderState& state, bool verify, uint32_t now,
                     std::vector<uint32_t>& drawOrder, OrderSecond& t);

// Whether the order nodes nest as emitRange walks them: each inside its parent's half, in order.
bool ordersNest(const std::vector<ShareOrder>& orders, uint32_t begin, uint32_t end,
                uint32_t orderBegin, uint32_t orderEnd, int depth);

// The patch's index of a program being kept (section 4.53); false (and `index` empty) when the
// program is past its 16-bit ids. The order nodes must nest (ordersNest).
bool orderIndexBuild(OrderIndex& index, const OrderSource& s);
size_t orderIndexBytes(const OrderIndex& index);

// The patch's line (report 105 section 4.53), once a second after `a reuse by parts`; `perUs` the
// TSC's ticks a microsecond as logReuseParts calibrated them.
void orderLogPatch(const OrderSecond& t, double perUs);

}  // namespace gplvr
