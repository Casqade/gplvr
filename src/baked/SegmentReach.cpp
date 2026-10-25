#include "baked/SegmentReach.h"

#include <algorithm>
#include <utility>

namespace baked {
namespace {

// gpl_sceneRender 0x487902: the bound a track without a far-distance table is drawn to, ahead
// and behind (the immediate 0x112A880 dlong, 914.4 m).
constexpr int64_t kDefaultReach = 0x112A880;

}  // namespace

bool SegmentReach::build(const Model3do& model, std::string& error) {
    spans_.clear();
    lap_ = 0;
    reach_ = 0;
    known_ = false;

    // gpl_sceneRender 0x487692: a root GROUP of two children holds the segment table in word 2
    // and the far-distance table in word 3; any other root is the segment table itself.
    uint32_t root = model.root();
    if (root >= model.primSize() || model.type(root) != kNodeGroup) {
        error = "the root is not a GROUP";
        return false;
    }
    uint32_t table = root;
    uint32_t farTable = kNoChild;
    if (model.word(root, 1) == 2) {
        table = model.word(root, 2);
        farTable = model.word(root, 3);
    }
    if (table >= model.primSize() || model.type(table) != kNodeGroup) {
        error = "the segment table is not a GROUP";
        return false;
    }
    // Children: word 2 the dlong lookup, words 3 .. count the segments, word count + 1 a
    // per-segment IDXLIST (gpl_worldRenderSegmentRange: a1 + 12 + 4 * index, count - 2 of them).
    uint32_t count = model.word(table, 1);
    if (count < 3 || !model.wordInside(table, count + 1)) {
        error = "the segment table has " + std::to_string(count) + " children";
        return false;
    }
    int64_t start = 0;
    for (uint32_t index = 0; index + 2 < count; ++index) {
        uint32_t segment = model.word(table, 3 + index);
        uint32_t metadata = segment < model.primSize() ? model.word(segment, 9) : kNoChild;
        if (segment >= model.primSize() || model.type(segment) != kNodeGroup ||
            model.word(segment, 1) != 8 || metadata >= model.primSize() ||
            model.type(metadata) != kNodeIdxList || !model.wordInside(metadata, 2)) {
            error = "segment " + std::to_string(index) + " is not a GROUP of 8 with its IDXLIST";
            spans_.clear();
            return false;
        }
        auto end = static_cast<int64_t>(model.word(metadata, 2));
        if (end < start) {
            error = "segment " + std::to_string(index) + " ends before the previous one";
            spans_.clear();
            return false;
        }
        spans_[segment] = Span{start, end};
        start = end;
    }
    lap_ = start;
    if (lap_ <= 0) {
        error = "the lap has no length";
        spans_.clear();
        return false;
    }

    // The far-distance table: word 1 the number of data words, four per entry, from word 2.
    // Every bound the walk takes, ahead or behind and in any heading band, is one of them.
    if (farTable == kNoChild) {
        reach_ = kDefaultReach;
    } else {
        uint32_t words = farTable < model.primSize() ? model.word(farTable, 1) : 0;
        if (farTable >= model.primSize() || model.type(farTable) != kNodeIdxList || words == 0 ||
            words % 4 != 0 || !model.wordInside(farTable, 1 + words)) {
            error = "the far-distance table does not read";
            spans_.clear();
            return false;
        }
        for (uint32_t i = 0; i < words; ++i)
            reach_ = std::max(reach_, static_cast<int64_t>(model.word(farTable, 2 + i)));
    }
    reach_ = std::min(reach_, lap_ / 2);  // the walk's own clamp
    known_ = true;
    return true;
}

bool SegmentReach::apart(uint32_t segmentA, uint32_t segmentB) const {
    if (!known_ || segmentA == segmentB || segmentA == kNoSegment || segmentB == kNoSegment)
        return false;
    auto a = spans_.find(segmentA);
    auto b = spans_.find(segmentB);
    if (a == spans_.end() || b == spans_.end()) return true;
    Span first = a->second;
    Span second = b->second;
    if (first.start > second.start) std::swap(first, second);
    int64_t forward = std::max<int64_t>(0, second.start - first.end);
    int64_t around = std::max<int64_t>(0, first.start + lap_ - second.end);
    return std::min(forward, around) > 2 * reach_;
}

}  // namespace baked
