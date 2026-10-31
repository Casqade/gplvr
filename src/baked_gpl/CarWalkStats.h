#pragma once

// The car walk's counters (CarWalk.cpp) and its once-a-second report (CarWalkReport.cpp): what
// each walk met, what became of its leaves, what GPL's memos skinned, and the per-second sums the
// `baked cars:` lines print.

#include <cstdint>

namespace gplvr {
namespace carwalk {

constexpr uint32_t kMaxListsPerPass = 64;  // one per car; GPL has at most 20 cars
constexpr uint32_t kProfileEvery = 16;     // one walk in this many is timed by parts
constexpr uint16_t kMaxWalkFrames = 256;
constexpr uint32_t kMaxSkinCorners = 256;

// What one walk met, by kind. `polygons` is what GPL's counter moves by for the same draw: one per
// polygon node and one per sprite, whose quad goes through the same emitter (gpl_spriteDraw ->
// gpl_polyKernelTexturedShaded / gpl_polyEmitTexturedUV).
struct WalkCounts {
    uint32_t polygons = 0;
    uint32_t sprites = 0;
    uint32_t lines = 0;  // gpl_lineDraw3d, not counted by GPL's polygon counter
    uint32_t controls = 0;
    uint32_t gates = 0;
    uint32_t selectors = 0;
    uint32_t setRegisters = 0;
    uint32_t transforms = 0;
    uint32_t boxCulled = 0;
    uint32_t sphereCulled = 0;
    uint32_t classHidden = 0;
    uint32_t indexOutOfRange = 0;  // op-5 selector reads past its cases: GPL reads garbage there
    uint32_t clockSelectors = 0;   // kind 3, taken as case value 0
    uint32_t unknownTypes = 0;
    uint32_t depthLimit = 0;
    uint32_t byType[0x24] = {};
};

// Stage 2: what became of each leaf the walk reached.
struct EmitCounts {
    uint32_t leaves = 0;         // appended to the draw list
    uint32_t noObject = 0;       // no loaded object holds the node, or its bake failed
    uint32_t notLeaf = 0;        // the object's bake has no leaf at that offset
    uint32_t typeDiffers = 0;    // the leaf there was baked from another node type
    uint32_t overflow = 0;       // a frame or material past 65535, drawn under its parent's
    uint32_t slotNoList = 0;     // a slot MATERIAL with no texture list published above it
    uint32_t unresolved = 0;     // a MATERIAL bound a texture whose file was not found
};

// Skinning (report 105 section 4.4), per walk: what GPL's vertex and normal memos changed.
struct SkinCounts {
    uint32_t leaves = 0;           // polygons drawn from corners memoised in another frame
    uint32_t byType[0x24] = {};
    uint32_t corners = 0;          // corners memoised in another frame of this walk
    uint32_t liveCorners = 0;      // corners GPL had memoised before this walk (dispatch frame)
    uint32_t normals = 0;          // normals lit in another frame
    uint32_t liveNormals = 0;      // normals GPL had lit before this walk (taken as this frame's)
    uint32_t dynamicVertices = 0;
    uint32_t vertexOverflow = 0;   // the memo table was full: transformed in place, unmemoised
    uint32_t normalOverflow = 0;
    uint32_t frameOverflow = 0;    // past kMaxWalkFrames: the frame's vertices are not memoised
    uint32_t staleWords = 0;       // a record's memo word that is not a live slot of GPL's pool
    uint32_t tooManyCorners = 0;   // a polygon over kMaxSkinCorners: not checked
    uint32_t noBakedCorners = 0;   // a skinned polygon whose baked corners were not found
};

// A sampled walk's parts, in TSC ticks: corners and control-node vertices through the memo,
// leaf emission (of it, building skinned leaves and finding the leaf's object), and the whole.
struct WalkTicks {
    uint64_t memo = 0;
    uint64_t emit = 0;
    uint64_t skinned = 0;
    uint64_t object = 0;
    uint64_t total = 0;
};

// Stage 2b, per second: the main-pass car calls, and what became of the ones gplvr draws.
struct OursSecond {
    uint32_t mainInside = 0;   // main-pass car calls (cockpit apart) inside a baked pass
    uint32_t mainOutside = 0;  // and outside one: GPL drew them whole
    uint32_t calls = 0;        // calls for cars gplvr draws
    uint32_t lists = 0;        // lists made: one per car per pass
    uint32_t deduped = 0;      // calls for a car already listed this pass (another cell)
    uint32_t arenaFull = 0;    // a car past kMaxListsPerPass: GPL drew it whole
    uint32_t passedThrough = 0;  // GPL's own draws handed over unwalked (no verification at 3)
    uint32_t passedCockpit = 0;
    uint32_t passedMirror = 0;
    uint32_t passedOther = 0;    // main-pass calls outside a baked pass, or no car renderer
    uint32_t profiled = 0;       // walks timed by parts (WalkTicks)
    WalkTicks ticks;
    uint32_t walkFailed = 0;   // the walk faulted: no list, GPL draws the car's later calls whole
    uint32_t bodyLost = 0;     // of them, the call whose body was already skipped: a car missing
    uint32_t leftOut = 0;      // debug view 6 left GPL's draw out
    uint32_t matched = 0;      // lists whose leaves equal the walk's polygons
    uint32_t bodyCalls = 0;    // GPL's draw called with the body to be skipped
    uint64_t leaves = 0;
    uint64_t walked = 0;
    uint64_t draws = 0;
    uint64_t gplPolygons = 0;  // what GPL's counter moved by on those calls, shadow apart: 0
    int64_t  walkTicks = 0;
    EmitCounts emit;
    SkinCounts skin;
    bool     differed = false;
    int      driver = 0;
    uint32_t firstLeaves = 0;
    uint32_t firstWalked = 0;
};

// Stage 2, per second: the draw lists and how their leaves compared with the walk's count.
struct EmitSecond {
    uint32_t draws = 0;
    uint32_t matched = 0;
    uint64_t leaves = 0;  // appended, with or without geometry
    uint64_t walked = 0;  // the walk's polygon count for the same draws
    uint64_t ranges = 0;
    uint64_t frames = 0;
    uint64_t materials = 0;
    uint64_t emptyLeaves = 0;
    EmitCounts counts;
    // The first draw this second whose list did not cover the walk.
    bool       differed = false;
    int        driver = 0;
    bool       cockpit = false;
    bool       mirror = false;
    uint32_t   firstLeaves = 0;
    uint32_t   firstWalked = 0;
    EmitCounts first;
};

// Per second: what the car draws were and how the walk compared with GPL's count.
struct Second {
    uint32_t draws = 0;
    uint32_t mainDraws = 0;
    uint32_t mirrorDraws = 0;
    uint32_t cockpitDraws = 0;
    uint32_t matched = 0;
    uint32_t cockpitMatched = 0;
    uint32_t walkFailed = 0;  // the walk faulted on a read; the draw went on as GPL's
    uint32_t leftOut = 0;     // debug view 6 left GPL's draw out, so there is no count to compare
    uint64_t ours = 0;
    uint64_t gpl = 0;
    uint64_t shadow = 0;
    int64_t  walkTicks = 0;
    WalkCounts sums;
    // The first draw this second whose counts differed.
    bool     differed = false;
    int      driver = 0;
    void*    instance = nullptr;
    bool     cockpit = false;
    bool     mirror = false;
    uint32_t planeMask = 0;
    WalkCounts first;
    uint32_t   firstGpl = 0;
    EmitSecond emit;
    OursSecond cars;
    // The same for the mirror passes (bakedMirrors = 1): its `main` counters are mirror-pass calls.
    OursSecond mirrorCars;
    SkinCounts skin;  // the stage-1 walks (mirrors, cockpit, stages 1 and 2)
};

// Once a second (the window starting at `windowStart`, QueryPerformanceCounter ticks): the
// `baked cars:` lines of `second`, then CarShare's and CarProgram's, and `second` starts again.
// True when it reported; the caller then starts its own per-second counts (the body skips)
// again. `stage3`: bakedCars = 3 is on.
bool reportSecond(Second& second, int64_t& windowStart, bool stage3, uint32_t bodySkipped,
                  uint32_t bodySkippedMirror);

// One walk's counts added into a sum.
void addCounts(WalkCounts& sum, const WalkCounts& add);
void addEmitCounts(EmitCounts& sum, const EmitCounts& add);
void addSkinCounts(SkinCounts& sum, const SkinCounts& add);

}  // namespace carwalk
}  // namespace gplvr
