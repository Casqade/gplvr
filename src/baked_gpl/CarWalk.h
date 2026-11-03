#pragma once

// Baked cars (docs/audit-2026-09-18/105-baked-renderer-rewrite.md sections 4-4.3): GPL draws every
// car through one vtable slot, gpl_carRecordDraw 0x486E50 (report 103 section 11), which goes
// through gplvr. Each call poses the car as GPL does; in a baked main or mirror pass the car's first
// call walks the posed node graph in GPL's memory with GPL's own decisions -- gates against the eye
// in each frame, SETREG into a copy of the pass's register file, LOD and register selectors, the
// class-visible mask of each MATERIAL, BOXCULL and SPHERECULL against the frustum -- into ONE draw
// list per car per pass, from objects CarBake.h bakes out of GPL's memory, merged into ranges under
// one frame (the poses from the car's root) and one MATERIAL state each. The walk runs without the
// cell's clip planes (a car in N cells is called N times), and its list is kept in a per-pass arena
// that CarRenderer draws at the end of the pass. GPL's draw of the car still runs on every call, but
// its body dispatch (the call at 0x48703F) is skipped, so only the pose, the helmet tint and the
// blob shadow remain GPL's. Any car call outside a baked pass of its kind goes to GPL whole.

#include "baked_gpl/CarShare.h"

#include <cstdint>
#include <vector>

namespace baked {
struct CarDrawList;
}

namespace gplvr {

// -- builds on the worker (CarBuildWorker.h, report 105 section 4.61) -----------------------------

// A node gpl_carModelPoseFromRecord or gpl_carRecordDraw rewrites for each car (report 103
// section 1: the team instance's XFORMs, their child pointers, the helmet BILLBOARDs' colour),
// words 0-8 as they were right after GPL's draw of the car the build is for.
struct CarPosedNode {
    uintptr_t node = 0;
    uint32_t  words[9] = {};
};
constexpr uint32_t kCarPosedNodes = 32;

// Everything a program build reads that GPL or gplvr may change before the worker runs it: the
// call (ShareCall's values, the register file's copy), the list as listOurs began it, the margin,
// the instance's wheels and their models, the posed nodes, and the bake it was asked under.
// GPL's graph is read live (static between reloads; CarBuildWorker.h's gate covers the reloads),
// GPL's vertex and normal memos are taken as empty (each take's scanPool reads the live one).
struct CarBuildSnapshot {
    uintptr_t record = 0;
    uintptr_t root = 0;
    uintptr_t handle = 0;
    uintptr_t instance = 0;
    int       driver = 0;
    float     clip[16] = {};
    float     eye[3] = {};
    float     lodScale = 1.0f;
    float     dlong = 0.0f;
    float     dlat = 0.0f;
    uint32_t  classVisible = 0;
    bool      lightingOn = false;
    uint32_t  textureList = 0;
    uint32_t  texturedMask = 0;
    int32_t   registers[1024] = {};
    // The list's header and root material (beginList).
    float     rootModelView[16] = {};
    float     projection[16] = {};
    float     light[3] = {};
    bool      mirror = false;
    uint32_t  walkSlot = 0;
    int32_t   surfaceClass = -1;
    uint8_t   textured = 0;
    int32_t   specularExponent = 0;
    float     specular = 0.0f;
    float     margin = 0.0f;  // of the program asked for; 0: every gate both ways
    uintptr_t wheels[4] = {};
    uintptr_t wheelModels[4][3] = {};
    uint32_t  posedCount = 0;
    CarPosedNode posed[kCarPosedNodes];
    uint32_t  generation = 0;  // carBakeGeneration
    uint32_t  rebaked = 0;     // carBakeTotals().objectsRebaked
    uint32_t  epoch = 0;       // carBuildWorkerEpoch
};

// What a build on the worker could not do there; the car's next build is the render thread's.
enum CarDetachedNeed : int {
    kDetachedOk,
    kDetachedObject,     // a leaf of an object CarBake has not baked yet
    kDetachedTexture,    // a texture handle CarBake has not resolved yet
    kDetachedScan,       // a gate or box CarConfig's scan has not answered yet
    kDetachedCancelled,  // the epoch moved, or the bake was dropped while the worker was out
    kDetachedNeeds
};

struct CarDetachedWalk {
    bool         walked = false;  // false: it faulted
    int          hazard = -1;     // the first UnionRedone, -1 none
    ShareRefusal refusal = kShareOk;
    uint32_t     learned = 0;     // configuration gates its hazards named that were new
    int          need = kDetachedOk;
    uintptr_t    needAt = 0;      // the node or handle
    // The gates its hazards named, for carConfigLearn on the render thread.
    std::vector<uintptr_t>* learnedGates = nullptr;
};

// Render thread, at the request: the instance's wheels and models and its posed nodes into
// `snapshot`. False when one could not be read.
bool carWalkSnapshotPose(uintptr_t instance, CarBuildSnapshot& snapshot);

// The worker, inside carBuildEnter: a kept list's walk of `snapshot` into `tape` (begun by the
// caller) and `list`, as listOurs walks a build, with the worker's own Walk, memos and texture
// cache. CarBake and CarConfig are read, never written; the posed nodes come from the snapshot.
void carWalkDetached(const CarBuildSnapshot& snapshot, ShareTape& tape, baked::CarDrawList& list,
                     CarDetachedWalk& out);

// Called once, on the render thread before a main walk, when the baked renderer is on. Every check
// comes before the first patch; logs what it installed or why not (GPL then draws every car).
void carWalkInstall();

// bakedPassBegin opens a new pass's arena (`mirror`: a mirror pass); bakedPassEnd reads its lists,
// one per car drawn by gplvr, in the order GPL first called each car. In VR the first eye's lists
// are kept for the second eye's pass of the same kind (CarShare.h), and a car's kept list
// (CarProgram.h) stands in for its walk where it holds. `pane`: a mirror pass's glass
// (stereoMirrorPaneOf, -1 not known), which keys its cars' kept lists (report 105 section 4.54).
void carWalkPassBegin(bool mirror, int pane);
const baked::CarDrawList* carWalkPassLists(uint32_t& count);

}  // namespace gplvr
