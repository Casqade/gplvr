#pragma once

// The baked renderer's track: which file GPL loaded, baked once on a worker thread into a
// baked::Scene (src/baked). docs/audit-2026-09-18/105-baked-renderer-rewrite.md.
//
// GPL keeps no file name with the track it loads, so the file is found by its shape: every
// tracks\<dir>\<dir>.3do whose PRIM size and vertex count match the object GPL draws is a
// candidate, and the first whose baked vertices also match GPL's first and last 64 is the track.

namespace baked {
struct Scene;
}

namespace gplvr {

// Once per main walk, before it, with the scene parameters of the pass (p[0] is the track
// object). Starts a bake when the object is new, and logs the bake's counts when it lands.
// Does nothing unless bakedRenderer is on.
void trackBakeMainWalk(void* sceneParams);

// The scene for the track GPL is drawing, or null while it is baking or when it failed.
const baked::Scene* trackBakeScene();

// The track's PRIM payload as GPL holds it, which the scene's item keys are offsets into. False
// when there is no scene.
bool trackBakePrim(const unsigned char*& prim, unsigned& size);

}  // namespace gplvr
