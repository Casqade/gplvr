#pragma once

// Which of a track's segments GPL's segment walk can draw in one pass (report 105 section 4.16).
//
// gpl_worldRenderSegmentRange 0x485960 draws the segments whose dlong span meets a window around
// the camera's dlong: ahead and behind out to bounds from the track's far-distance table (root
// GROUP word 3, read by gpl_sceneRender 0x487EAC..0x487F4E), each clamped to half the lap, with
// the full- and mid-detail bands inside the same bounds. A segment's span ends at word 2 of the
// IDXLIST that is its own word 9 (gpl_dlongToSegmentIndex 0x485FA0) and starts where the
// previous one in the segment table ends. Two segments further apart along the lap than the
// longest window are never on screen together, whatever the camera does: Monza 10k lays its
// road course and its banking over one another, half a lap apart.
//
// The bound assumes the walk is centred on the camera's own dlong (every view but a narrow-field
// TV camera, which centres the ahead half on the viewed car, 0x487E71).

#include "baked/Piff.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace baked {

class SegmentReach {
public:
    // Reads the segment table and the far-distance table. False, with the reason, when either
    // does not read; apart() then answers false for every pair.
    bool build(const Model3do& model, std::string& error);

    bool     known() const { return known_; }
    uint32_t segments() const { return static_cast<uint32_t>(spans_.size()); }
    double   lapMetres() const { return lap_ / kDlongPerMetre; }
    double   reachMetres() const { return reach_ / kDlongPerMetre; }  // ahead or behind, at most

    // True when GPL never draws the two segment GROUPs in one pass: both are in the table and
    // the gap between them along the lap, the shorter way round, is longer than the window. A
    // segment GROUP that is not in the table is never drawn by the segment walk at all, so it is
    // apart from every other segment. kNoSegment (an item under no segment) is never apart.
    bool apart(uint32_t segmentA, uint32_t segmentB) const;

    static constexpr double   kDlongPerMetre = 19685.0;  // gpl_dlongUnitsPerMetre
    static constexpr uint32_t kNoSegment = 0xFFFFFFFFu;

private:
    struct Span {
        int64_t start = 0;
        int64_t end = 0;
    };
    std::unordered_map<uint32_t, Span> spans_;  // segment GROUP offset -> its dlong span
    int64_t                            lap_ = 0;
    int64_t                            reach_ = 0;
    bool                               known_ = false;
};

}  // namespace baked
