#pragma once

// The car walk's per-walk counts (CarWalk.cpp): what became of each leaf the walk reached and
// what GPL's memos skinned. A list stands for the second eye or a kept program only when they
// say it covers the walk (CarWalk.cpp, shareRefusal).

#include <cstdint>

namespace gplvr {
namespace carwalk {

constexpr uint32_t kMaxListsPerPass = 64;  // one per car; GPL has at most 20 cars
constexpr uint16_t kMaxWalkFrames = 256;
constexpr uint32_t kMaxSkinCorners = 256;

// What became of each leaf the walk reached.
struct EmitCounts {
    uint32_t leaves = 0;         // appended to the draw list
    uint32_t noObject = 0;       // no loaded object holds the node, or its bake failed
    uint32_t notLeaf = 0;        // the object's bake has no leaf at that offset
    uint32_t typeDiffers = 0;    // the leaf there was baked from another node type
    uint32_t overflow = 0;       // a frame or material past 65535, drawn under its parent's
    uint32_t slotNoList = 0;     // a slot MATERIAL with no texture list published above it
    uint32_t unresolved = 0;     // a MATERIAL bound a texture whose file was not found
    // The cockpit's mirror glass (report 105 section 4.48): polygons under a MATERIAL binding
    // mirror.mip, left out of the list for GPL to draw, and those the glass set had no room for.
    uint32_t glass = 0;
    uint32_t glassOverflow = 0;
    // Step 2 (section 4.49): glass appended to the list with the UVs GPL holds live, and glass
    // refused (skinned, or its baked corners not found), which refuses the list.
    uint32_t glassListed = 0;
    uint32_t glassRefused = 0;
};

// Skinning (report 105 section 4.4), per walk: what GPL's vertex and normal memos changed.
struct SkinCounts {
    uint32_t corners = 0;          // corners memoised in another frame of this walk
    uint32_t liveCorners = 0;      // corners GPL had memoised before this walk (dispatch frame)
    uint32_t normals = 0;          // normals lit in another frame
    uint32_t liveNormals = 0;      // normals GPL had lit before this walk (taken as this frame's)
    uint32_t vertexOverflow = 0;   // the memo table was full: transformed in place, unmemoised
    uint32_t normalOverflow = 0;
    uint32_t frameOverflow = 0;    // past kMaxWalkFrames: the frame's vertices are not memoised
    uint32_t staleWords = 0;       // a record's memo word that is not a live slot of GPL's pool
    uint32_t tooManyCorners = 0;   // a polygon over kMaxSkinCorners: not checked
    uint32_t noBakedCorners = 0;   // a skinned polygon whose baked corners were not found
};

// Why a cockpit call was drawn whole by GPL (report 105 section 4.48).
enum CockpitWhole : int {
    kCockpitNotBaked,     // outside a baked main pass whose cars the car renderer draws
    kCockpitNotMainFile,  // not the main register file (GPL enqueues it for main passes only)
    kCockpitFilter,       // the node filter does not see the lit polygon types (not stock)
    kCockpitNoMirrorMip,  // the car set's mirror.mip handle is not known, so the glass is not
    kCockpitArenaFull,    // past kMaxListsPerPass lists this pass
    kCockpitFaulted,      // this pass's walk of it faulted
    kCockpitRefused,      // its list did not hold every polygon walked but the glass
    kCockpitWholeKinds
};

}  // namespace carwalk
}  // namespace gplvr
