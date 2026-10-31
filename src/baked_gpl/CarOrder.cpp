#include "baked_gpl/CarOrder.h"

#include "Hooks.h"
#include "Log.h"

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>

namespace gplvr {
namespace {

// More condition bits than this differing from every memo with runs: the order is emitted whole
// (report 105 section 4.53; the probe's heaviest seconds differed from the nearest memo in 42-125,
// section 4.52.1).
constexpr uint32_t kOrderPatchMaxBits = 512;
// One patched order in kPatchCheckEvery is emitted again whole and compared (section 4.53).
constexpr uint32_t kPatchCheckEvery = 64;
// A patch's position in its base memo's order for a node the base did not visit.
constexpr uint32_t kNoBase = 0xFFFFFFFFu;

uint32_t g_orderKeys = 0;   // the last OrderMemo::key handed out
uint32_t g_swapped = 0;     // order nodes put the other way round by the emission running
uint32_t g_orderNodes = 0;  // order nodes emitRange visited (section 4.38)
// The order's output, one index a draw of the largest program met; it only grows (report 105
// section 4.36). The patch check's whole emission goes to its own.
std::vector<uint32_t> g_orderScratch;
std::vector<uint32_t> g_orderCheck;
// Per order node of the program being patched: g_markEpoch where a changed bit reaches it, so no
// patch clears the marks of the last (section 4.53).
std::vector<uint32_t> g_mark;
uint32_t              g_markEpoch = 0;
// The condition ids that differ from the memo being compared, and from the nearest one so far.
uint32_t g_changed[2][kOrderPatchMaxBits];
uint32_t g_patches = 0;  // orders patched, for kPatchCheckEvery

bool on(const OrderSource& s, uint16_t condition) {
    return condition < s.bitCount && s.bits[condition] != 0;
}

// The draws [from, to) this eye draws, written by index at `out`, which returns past the last:
// every index is written and the end moves on by the draw's condition bit, so an unpredictable
// condition costs no branch. Each draw is visited once over an emission, so a buffer of the
// program's draw count holds every write (report 105 section 4.36).
uint32_t* copyDraws(const OrderSource& s, uint32_t from, uint32_t to, uint32_t* out) {
    if (to > s.draws) to = s.draws;
    const uint16_t* conditions = s.drawConditions;
    const uint8_t*  bits = s.bits;
    uint32_t        count = s.bitCount;
    for (uint32_t i = from; i < to; ++i) {
        uint16_t condition = conditions[i];
        *out = i;
        out += condition < count ? bits[condition] : 0u;
    }
    return out;
}

// The indices of the draws [begin, end) this eye draws, with the order nodes [orderBegin,
// orderEnd) inside them, each node's halves in this eye's order (ShareOrder); written at `out`,
// which returns past the last.
uint32_t* emitRange(const OrderSource& s, uint32_t begin, uint32_t end, uint32_t orderBegin,
                    uint32_t orderEnd, uint32_t* out) {
    const ShareOrder* orders = s.nodes;
    uint32_t          from = begin;
    uint32_t          k = orderBegin;
    while (k < orderEnd) {
        const ShareOrder& o = orders[k];
        ++g_orderNodes;
        out = copyDraws(s, from, o.begin, out);
        if (on(s, o.condition)) {
            if (on(s, o.onFront) == o.front) {
                out = emitRange(s, o.begin, o.split, k + 1, o.splitOrder, out);
                out = emitRange(s, o.split, o.end, o.splitOrder, o.nested, out);
            } else {
                out = emitRange(s, o.split, o.end, o.splitOrder, o.nested, out);
                out = emitRange(s, o.begin, o.split, k + 1, o.splitOrder, out);
                ++g_swapped;
            }
        }
        from = o.end;
        k = o.nested;
    }
    return copyDraws(s, from, end, out);
}

// emitRange, also writing each visited node's run into `runs` (section 4.53), its start counted
// from `parent`, the start of the enclosing node's run (of the order, at the top).
uint32_t* emitRuns(const OrderSource& s, uint32_t begin, uint32_t end, uint32_t orderBegin,
                   uint32_t orderEnd, uint32_t* out, OrderRun* runs, const uint32_t* parent) {
    const ShareOrder* orders = s.nodes;
    uint32_t          from = begin;
    uint32_t          k = orderBegin;
    while (k < orderEnd) {
        const ShareOrder& o = orders[k];
        ++g_orderNodes;
        out = copyDraws(s, from, o.begin, out);
        uint32_t* start = out;
        uint32_t  swapsBefore = g_swapped;
        if (on(s, o.condition)) {
            if (on(s, o.onFront) == o.front) {
                out = emitRuns(s, o.begin, o.split, k + 1, o.splitOrder, out, runs, start);
                out = emitRuns(s, o.split, o.end, o.splitOrder, o.nested, out, runs, start);
            } else {
                out = emitRuns(s, o.split, o.end, o.splitOrder, o.nested, out, runs, start);
                out = emitRuns(s, o.begin, o.split, k + 1, o.splitOrder, out, runs, start);
                ++g_swapped;
            }
        }
        runs[k] = OrderRun{static_cast<uint16_t>(start - parent),
                           static_cast<uint16_t>(out - start),
                           static_cast<uint16_t>(g_swapped - swapsBefore)};
        from = o.end;
        k = o.nested;
    }
    return copyDraws(s, from, end, out);
}

// -- the patch (report 105 section 4.53) --------------------------------------------------------

// The condition ids where the bits `a` and `b` (n bytes each, 0 or 1) differ, into `list`, sixteen
// bytes a step; past `cap` ids it stops and returns cap + 1.
uint32_t differingBits(const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t* list,
                       uint32_t cap) {
    uint32_t count = 0;
    uint32_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        auto equal = static_cast<unsigned long>(_mm_movemask_epi8(_mm_cmpeq_epi8(x, y)));
        auto differ = ~equal & 0xFFFFu;
        while (differ) {
            unsigned long bit;
            _BitScanForward(&bit, differ);
            if (count == cap) return cap + 1;
            list[count++] = i + bit;
            differ &= differ - 1;
        }
    }
    for (; i < n; ++i)
        if (a[i] != b[i]) {
            if (count == cap) return cap + 1;
            list[count++] = i;
        }
    return count;
}

// Marks `node` and its enclosing nodes as reached by a changed bit, up to one already marked.
void markUp(const OrderIndex& index, uint16_t node, uint32_t epoch, uint32_t& marked) {
    while (node != kNoOrderNode && g_mark[node] != epoch) {
        g_mark[node] = epoch;
        ++marked;
        node = index.nodeParent[node];
    }
}

// One patch of an order from its base memo.
struct Patch {
    const OrderSource* s = nullptr;
    const uint32_t*    baseOrder = nullptr;
    uint32_t           baseSize = 0;
    const OrderRun*    baseRuns = nullptr;
    const uint8_t*     baseBits = nullptr;  // s->bitCount of them
    OrderRun*          runs = nullptr;      // of the memo being filled
    uint32_t           epoch = 0;           // g_mark's for this patch
    const uint32_t*    limit = nullptr;     // one past the output buffer
    uint32_t           visited = 0;
    uint32_t           copiedRuns = 0;
    uint32_t           copiedDraws = 0;
    bool               failed = false;
};

// emitRange's walk, where an order node that is on, that no changed bit reaches and that the base
// visited is the base's run copied whole with its nested nodes' runs; its nested nodes are not
// visited. `parent` is the start of the enclosing node's run in the output and `baseAt` in the
// base's order, kNoBase where the base did not visit the nodes [orderBegin, orderEnd) (an
// enclosing node was off in it). The draws between nodes are copied by their bits, as emitRange
// does. A run that would pass the base's order or the output buffer sets `failed`.
uint32_t* patchRange(Patch& c, uint32_t begin, uint32_t end, uint32_t orderBegin,
                     uint32_t orderEnd, uint32_t* out, const uint32_t* parent, uint32_t baseAt) {
    const OrderSource& s = *c.s;
    uint32_t           from = begin;
    uint32_t           k = orderBegin;
    while (k < orderEnd) {
        const ShareOrder& o = s.nodes[k];
        ++c.visited;
        if (o.begin - from > static_cast<uint32_t>(c.limit - out)) {
            c.failed = true;
            return out;
        }
        out = copyDraws(s, from, o.begin, out);
        uint32_t* start = out;
        OrderRun& run = c.runs[k];
        run.start = static_cast<uint16_t>(start - parent);
        run.length = 0;
        run.swaps = 0;
        if (on(s, o.condition)) {
            if (g_mark[k] != c.epoch && baseAt != kNoBase) {
                // Its condition is the same in the base, which therefore emitted this run.
                const OrderRun& was = c.baseRuns[k];
                uint32_t        at = baseAt + was.start;
                if (at > c.baseSize || was.length > c.baseSize - at ||
                    was.length > static_cast<uint32_t>(c.limit - out)) {
                    c.failed = true;
                    return out;
                }
                std::memcpy(out, c.baseOrder + at, was.length * sizeof(uint32_t));
                out += was.length;
                run.length = was.length;
                run.swaps = was.swaps;
                g_swapped += was.swaps;
                if (o.nested > k + 1)
                    std::memcpy(c.runs + k + 1, c.baseRuns + k + 1,
                                (o.nested - k - 1) * sizeof(OrderRun));
                ++c.copiedRuns;
                c.copiedDraws += was.length;
            } else {
                uint32_t swapsBefore = g_swapped;
                bool     baseOn = o.condition < s.bitCount && c.baseBits[o.condition] != 0;
                uint32_t in = baseAt != kNoBase && baseOn ? baseAt + c.baseRuns[k].start : kNoBase;
                if (on(s, o.onFront) == o.front) {
                    out = patchRange(c, o.begin, o.split, k + 1, o.splitOrder, out, start, in);
                    if (!c.failed)
                        out = patchRange(c, o.split, o.end, o.splitOrder, o.nested, out, start, in);
                } else {
                    out = patchRange(c, o.split, o.end, o.splitOrder, o.nested, out, start, in);
                    if (!c.failed)
                        out = patchRange(c, o.begin, o.split, k + 1, o.splitOrder, out, start, in);
                    ++g_swapped;
                }
                if (c.failed) return out;
                run.length = static_cast<uint16_t>(out - start);
                run.swaps = static_cast<uint16_t>(g_swapped - swapsBefore);
            }
        }
        from = o.end;
        k = o.nested;
    }
    if (end - from > static_cast<uint32_t>(c.limit - out)) {
        c.failed = true;
        return out;
    }
    return copyDraws(s, from, end, out);
}

// The memo least recently used, other than `except`.
OrderMemo* leastRecent(OrderState& state, const OrderMemo* except) {
    OrderMemo* memo = nullptr;
    for (OrderMemo& m : state.memos)
        if (&m != except && (!memo || m.lastUsed < memo->lastUsed)) memo = &m;
    return memo;
}

// A memo filled with `drawOrder`, these bits' order, and its counts from the packed polygons.
void fillMemo(OrderMemo& memo, const OrderSource& s, const std::vector<uint32_t>& drawOrder,
              uint32_t swapped, bool runs, uint32_t now, OrderSecond& t) {
    if (memo.key) {
        ++t.refilled;
        if (!memo.hits) ++t.unused;
    }
    memo.on.assign(s.bits, s.bits + s.bitCount);
    memo.order = drawOrder;
    memo.hasRuns = runs;
    memo.swapped = swapped;
    memo.hits = 0;
    if (++g_orderKeys == 0) ++g_orderKeys;
    memo.key = g_orderKeys;
    uint32_t polygons = 0;
    uint32_t dynamicLeaves = 0;
    for (uint32_t index : drawOrder) {
        uint32_t entry = index < s.draws ? s.drawPolygons[index] : 0u;
        uint32_t count = entry & ~kDynamicDraw;
        polygons += count;
        dynamicLeaves += entry & kDynamicDraw ? count : 0u;
    }
    memo.polygons = polygons;
    memo.dynamicLeaves = dynamicLeaves;
    memo.lastUsed = now;
}

// This miss's order patched from the memo with runs nearest in condition bits, into `drawOrder`,
// and a memo other than the base filled with it: -1, or the OrderWhole why the caller emits it
// whole. The order buffer holds s.draws + 1.
int patchOrder(const OrderSource& s, OrderState& state, uint32_t now,
               std::vector<uint32_t>& drawOrder, OrderTaken& taken, OrderSecond& t) {
    uint64_t          findStart = __rdtsc();
    const OrderIndex& index = state.index;
    if (index.drawsFirst.size() != s.bitCount + 1u || index.nodesFirst.size() != s.bitCount + 1u ||
        index.drawNode.size() != s.draws || index.nodeParent.size() != s.nodeCount)
        return kWholeSizes;
    // The most recently used memo with runs is compared first: it is likely the nearest, and each
    // other memo's comparison stops once it is no nearer.
    int recent = -1;
    for (int j = 0; j < kOrderMemos; ++j) {
        const OrderMemo& m = state.memos[j];
        if (m.key && m.hasRuns && (recent < 0 || m.lastUsed > state.memos[recent].lastUsed))
            recent = j;
    }
    if (recent < 0) return kWholeNoBase;
    const OrderMemo* base = nullptr;
    uint32_t         bits = 0;
    int              list = 0;  // g_changed[list]: the base's changed ids
    bool             sized = false;
    for (int n = 0; n < kOrderMemos; ++n) {
        int              j = n == 0 ? recent : n - 1 < recent ? n - 1 : n;
        const OrderMemo& m = state.memos[j];
        if (!m.key || !m.hasRuns || m.on.size() != s.bitCount || m.runs.size() != s.nodeCount)
            continue;
        sized = true;
        if (base && bits == 0) break;
        uint32_t cap = base ? bits - 1 : kOrderPatchMaxBits;
        uint32_t found = differingBits(m.on.data(), s.bits, s.bitCount, g_changed[1 - list], cap);
        if (found > cap) continue;
        base = &m;
        bits = found;
        list = 1 - list;
    }
    if (!base) return sized ? kWholeTooMany : kWholeSizes;
    // Every order node a changed bit reaches: the innermost node around each draw under it and
    // each node it is the condition or front side of, and the nodes around those.
    if (g_mark.size() < s.nodeCount) g_mark.resize(s.nodeCount, 0u);
    if (++g_markEpoch == 0) {
        std::fill(g_mark.begin(), g_mark.end(), 0u);
        g_markEpoch = 1;
    }
    uint32_t marked = 0;
    for (uint32_t i = 0; i < bits; ++i) {
        uint32_t condition = g_changed[list][i];
        for (uint32_t e = index.drawsFirst[condition]; e < index.drawsFirst[condition + 1]; ++e)
            markUp(index, index.drawNode[index.draws[e]], g_markEpoch, marked);
        for (uint32_t e = index.nodesFirst[condition]; e < index.nodesFirst[condition + 1]; ++e)
            markUp(index, index.nodes[e], g_markEpoch, marked);
    }
    OrderMemo* fill = leastRecent(state, base);
    fill->runs.resize(s.nodeCount);
    fill->hasRuns = false;
    Patch c;
    c.s = &s;
    c.baseOrder = base->order.data();
    c.baseSize = static_cast<uint32_t>(base->order.size());
    c.baseRuns = base->runs.data();
    c.baseBits = base->on.data();
    c.runs = fill->runs.data();
    c.epoch = g_markEpoch;
    uint32_t* first = g_orderScratch.data();
    c.limit = first + g_orderScratch.size();
    g_swapped = 0;
    uint64_t  emitStart = __rdtsc();
    uint32_t* last = patchRange(c, 0, s.draws, 0, s.nodeCount, first, first, 0);
    uint64_t  emitEnd = __rdtsc();
    if (c.failed) return kWholeBounds;
    drawOrder.assign(first, last);
    uint64_t copyEnd = __rdtsc();
    uint32_t swapped = g_swapped;
    bool     patched = true;
    if (++g_patches % kPatchCheckEvery == 0) {
        ++t.checks;
        if (g_orderCheck.size() < s.draws + 1u) g_orderCheck.resize(s.draws + 1u);
        g_swapped = 0;
        uint32_t* checkFirst = g_orderCheck.data();
        uint32_t* checkLast = emitRange(s, 0, s.draws, 0, s.nodeCount, checkFirst);
        auto      length = static_cast<size_t>(checkLast - checkFirst);
        if (g_swapped != swapped || length != drawOrder.size() ||
            (length && std::memcmp(checkFirst, drawOrder.data(), length * sizeof(uint32_t)) != 0)) {
            // The whole emission's order is used, and this program's are emitted whole until it is
            // rebuilt.
            ++t.checksDiffered;
            ++t.whole[kWholeCheck];
            drawOrder.assign(checkFirst, checkLast);
            swapped = g_swapped;
            patched = false;
            state.patchOff = true;
        }
        taken.checkTicks = __rdtsc() - copyEnd;
    }
    uint64_t fillStart = __rdtsc();
    fillMemo(*fill, s, drawOrder, swapped, patched, now, t);
    taken.memo = fill;
    taken.swapped = swapped;
    if (!patched) return -1;
    ++t.patched;
    t.patchBits += bits;
    t.patchMarked += marked;
    t.patchVisited += c.visited;
    t.patchNodes += s.nodeCount;
    t.patchRuns += c.copiedRuns;
    t.patchCopied += c.copiedDraws;
    t.patchDrawn += drawOrder.size();
    if (base == &state.memos[recent]) ++t.patchRecent;
    t.patchFindTicks += emitStart - findStart;
    t.patchEmitTicks += emitEnd - emitStart;
    t.patchFillTicks += (copyEnd - emitEnd) + (__rdtsc() - fillStart);
    return -1;
}

// The draws and order nodes under [begin, end) and [orderBegin, orderEnd): each draw's innermost
// enclosing node and each node's enclosing one is `parent`, or a node met inside it.
void indexRange(OrderIndex& index, const ShareOrder* nodes, uint32_t begin, uint32_t end,
                uint32_t orderBegin, uint32_t orderEnd, uint16_t parent) {
    uint32_t from = begin;
    uint32_t k = orderBegin;
    while (k < orderEnd) {
        const ShareOrder& o = nodes[k];
        for (uint32_t i = from; i < o.begin; ++i) index.drawNode[i] = parent;
        index.nodeParent[k] = parent;
        indexRange(index, nodes, o.begin, o.split, k + 1, o.splitOrder, static_cast<uint16_t>(k));
        indexRange(index, nodes, o.split, o.end, o.splitOrder, o.nested, static_cast<uint16_t>(k));
        from = o.end;
        k = o.nested;
    }
    for (uint32_t i = from; i < end; ++i) index.drawNode[i] = parent;
}

}  // namespace

OrderTaken orderTake(const OrderSource& s, OrderState& state, bool verify, uint32_t now,
                     std::vector<uint32_t>& drawOrder, OrderSecond& t) {
    OrderTaken taken;
    uint64_t   start = __rdtsc();
    OrderMemo* memo = nullptr;
    if (!verify)
        for (OrderMemo& m : state.memos)
            if (m.key && m.on.size() == s.bitCount &&
                std::memcmp(m.on.data(), s.bits, s.bitCount) == 0)
                memo = &m;
    if (memo) {
        ++t.memoised;
        ++memo->hits;
        drawOrder = memo->order;
        memo->lastUsed = now;
        taken.memo = memo;
        taken.swapped = memo->swapped;
        return taken;
    }
    if (g_orderScratch.size() < s.draws + 1u) g_orderScratch.resize(s.draws + 1u);
    int whole = verify                       ? kWholeVerify
                : !g_options.bakedOrderPatch ? kWholeOptionOff
                : !state.index.built         ? kWholeNoIndex
                : state.patchOff             ? kWholePatchOff
                                             : -1;
    // A program the patch serves keeps runs in every memo it fills, whole or patched.
    bool runs = whole < 0;
    if (runs) whole = patchOrder(s, state, now, drawOrder, taken, t);
    if (whole >= 0) {
        ++t.whole[whole];
        g_swapped = 0;
        g_orderNodes = 0;
        OrderMemo* fill = verify ? nullptr : leastRecent(state, nullptr);
        uint32_t*  first = g_orderScratch.data();
        uint64_t   emitStart = __rdtsc();
        uint32_t*  last;
        if (fill && runs) {
            fill->runs.resize(s.nodeCount);
            last = emitRuns(s, 0, s.draws, 0, s.nodeCount, first, fill->runs.data(), first);
        } else {
            last = emitRange(s, 0, s.draws, 0, s.nodeCount, first);
        }
        uint64_t emitEnd = __rdtsc();
        drawOrder.assign(first, last);
        taken.swapped = g_swapped;
        if (fill) {
            uint64_t copyEnd = __rdtsc();
            fillMemo(*fill, s, drawOrder, g_swapped, runs, now, t);
            t.emitTicks += emitEnd - emitStart;
            t.copyTicks += copyEnd - emitEnd;
            t.fillTicks += __rdtsc() - copyEnd;
            ++t.timed;
            t.draws += s.draws;
            t.nodes += g_orderNodes;
            t.emitted += drawOrder.size();
            taken.memo = fill;
        }
    }
    t.missTicks += __rdtsc() - start - taken.checkTicks;
    ++t.misses;
    return taken;
}

bool ordersNest(const std::vector<ShareOrder>& orders, uint32_t begin, uint32_t end,
                uint32_t orderBegin, uint32_t orderEnd, int depth) {
    if (depth > 4096) return false;
    uint32_t from = begin;
    uint32_t k = orderBegin;
    while (k < orderEnd) {
        const ShareOrder& o = orders[k];
        if (o.begin < from || o.split < o.begin || o.end < o.split || o.end > end ||
            o.splitOrder <= k || o.nested < o.splitOrder || o.nested > orderEnd)
            return false;
        if (!ordersNest(orders, o.begin, o.split, k + 1, o.splitOrder, depth + 1) ||
            !ordersNest(orders, o.split, o.end, o.splitOrder, o.nested, depth + 1))
            return false;
        from = o.end;
        k = o.nested;
    }
    return true;
}

bool orderIndexBuild(OrderIndex& index, const OrderSource& s) {
    index = OrderIndex{};
    if (s.draws > 0xFFFFu || s.nodeCount >= kNoOrderNode || s.bitCount > 0x10000u) return false;
    uint32_t nodeEntries = 0;
    for (uint32_t k = 0; k < s.nodeCount; ++k) {
        const ShareOrder& o = s.nodes[k];
        nodeEntries += o.condition < s.bitCount ? 1u : 0u;
        nodeEntries += o.onFront != o.condition && o.onFront < s.bitCount ? 1u : 0u;
    }
    if (nodeEntries > 0xFFFFu) return false;
    // Each condition's entries counted at [c + 1], summed into first offsets, then placed.
    index.drawsFirst.assign(s.bitCount + 1u, 0);
    index.nodesFirst.assign(s.bitCount + 1u, 0);
    for (uint32_t i = 0; i < s.draws; ++i)
        if (s.drawConditions[i] < s.bitCount) ++index.drawsFirst[s.drawConditions[i] + 1u];
    for (uint32_t k = 0; k < s.nodeCount; ++k) {
        const ShareOrder& o = s.nodes[k];
        if (o.condition < s.bitCount) ++index.nodesFirst[o.condition + 1u];
        if (o.onFront != o.condition && o.onFront < s.bitCount) ++index.nodesFirst[o.onFront + 1u];
    }
    for (uint32_t c = 0; c < s.bitCount; ++c) {
        index.drawsFirst[c + 1] =
            static_cast<uint16_t>(index.drawsFirst[c + 1] + index.drawsFirst[c]);
        index.nodesFirst[c + 1] =
            static_cast<uint16_t>(index.nodesFirst[c + 1] + index.nodesFirst[c]);
    }
    index.draws.resize(index.drawsFirst[s.bitCount]);
    index.nodes.resize(index.nodesFirst[s.bitCount]);
    std::vector<uint16_t> next(index.drawsFirst.begin(), index.drawsFirst.end() - 1);
    for (uint32_t i = 0; i < s.draws; ++i)
        if (s.drawConditions[i] < s.bitCount)
            index.draws[next[s.drawConditions[i]]++] = static_cast<uint16_t>(i);
    next.assign(index.nodesFirst.begin(), index.nodesFirst.end() - 1);
    for (uint32_t k = 0; k < s.nodeCount; ++k) {
        const ShareOrder& o = s.nodes[k];
        if (o.condition < s.bitCount) index.nodes[next[o.condition]++] = static_cast<uint16_t>(k);
        if (o.onFront != o.condition && o.onFront < s.bitCount)
            index.nodes[next[o.onFront]++] = static_cast<uint16_t>(k);
    }
    index.drawNode.assign(s.draws, kNoOrderNode);
    index.nodeParent.assign(s.nodeCount, kNoOrderNode);
    indexRange(index, s.nodes, 0, s.draws, 0, s.nodeCount, kNoOrderNode);
    index.built = true;
    return true;
}

size_t orderIndexBytes(const OrderIndex& index) {
    return (index.drawsFirst.capacity() + index.draws.capacity() + index.nodesFirst.capacity() +
            index.nodes.capacity() + index.drawNode.capacity() + index.nodeParent.capacity()) *
           sizeof(uint16_t);
}

void orderLogPatch(const OrderSecond& t, double perUs) {
    if (!t.misses && !t.indexRefused) return;
    uint32_t whole = 0;
    for (uint32_t n : t.whole) whole += n;
    double n = t.patched ? static_cast<double>(t.patched) : 1.0;
    double timed = t.timed ? static_cast<double>(t.timed) : 1.0;
    double find = static_cast<double>(t.patchFindTicks) / n / perUs;
    double emit = static_cast<double>(t.patchEmitTicks) / n / perUs;
    double fill = static_cast<double>(t.patchFillTicks) / n / perUs;
    double wholeUs = static_cast<double>(t.emitTicks + t.copyTicks + t.fillTicks) / timed / perUs;
    double share = t.patchNodes ? 100.0 * static_cast<double>(t.patchVisited) /
                                      static_cast<double>(t.patchNodes)
                                : 0.0;
    logf("baked cars: the draw order patched from the nearest memo (report 105 section 4.53, "
         "bakedOrderPatch %d) -- memo misses %u: patched %u, emitted whole %u (a verify walk's %u, "
         "bakedOrderPatch 0 %u, program past the index's 16-bit ids %u, patch off after a check "
         "differed %u, no memo with runs yet %u, no memo of the program's sizes %u, more than %u "
         "bits changed %u, a run out of bounds %u (must be 0), the check differed %u (must be "
         "0)); per patched miss: condition bits changed %.1f, order nodes they reach %.1f, order "
         "nodes visited %.0f of %.0f (%.1f %%), runs copied whole %.1f holding %.0f of %.0f draws "
         "drawn, the base the most recently used memo %.0f %%; a patched miss %.2f us = the base "
         "and its changed bits %.2f + the patch %.2f + its copy and the memo's fill %.2f, a whole "
         "emission %.2f us (emitRange, copy and fill, section 4.38); patched orders emitted again "
         "whole %u (1 in %u), differing %u (must be 0); programs kept without an index %u",
         g_options.bakedOrderPatch, t.misses, t.patched, whole, t.whole[kWholeVerify],
         t.whole[kWholeOptionOff], t.whole[kWholeNoIndex], t.whole[kWholePatchOff],
         t.whole[kWholeNoBase], t.whole[kWholeSizes], kOrderPatchMaxBits, t.whole[kWholeTooMany],
         t.whole[kWholeBounds], t.whole[kWholeCheck], static_cast<double>(t.patchBits) / n,
         static_cast<double>(t.patchMarked) / n, static_cast<double>(t.patchVisited) / n,
         static_cast<double>(t.patchNodes) / n, share, static_cast<double>(t.patchRuns) / n,
         static_cast<double>(t.patchCopied) / n, static_cast<double>(t.patchDrawn) / n,
         100.0 * static_cast<double>(t.patchRecent) / n, find + emit + fill, find, emit, fill,
         wholeUs, t.checks, kPatchCheckEvery, t.checksDiffered, t.indexRefused);
}

}  // namespace gplvr
