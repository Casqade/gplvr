#pragma once

// The baked renderer's track: which file GPL loaded, baked once on a worker thread into a
// baked::Scene (src/baked). docs/audit-2026-09-18/105-baked-renderer-rewrite.md.
//
// GPL keeps no file name with the track it loads, so the file is found by its shape: every
// tracks\<dir>\<dir>.3do whose PRIM size and vertex count match the object GPL draws is a
// candidate, and the first whose baked vertices also match GPL's first and last 64 is the track.

#include <cstddef>
#include <cstdint>

namespace baked {
struct Scene;
}

namespace gplvr {

// Once per main walk, before it, with the scene parameters of the pass (p[0] is the track
// object). Starts a bake when the object is new, and logs the bake's counts when it lands.
// Does nothing unless rendererType() is on.
void trackBakeMainWalk(void* sceneParams);

// The scene for the track GPL is drawing, or null while it is baking or when it failed.
const baked::Scene* trackBakeScene();

// Why the track GPL is drawing has no scene -- the bake's own words, e.g. "out of memory -- too
// large for this process" -- or null while it is baking or baked. Without a scene every pass is
// GPL's: the track, the cars, the cockpit, the flagmen and the mirrors alike.
const char* trackBakeFailure();

// Bumped each time a scene lands. A new scene can be allocated where the freed one was, so a
// holder tells them apart by this, never by the pointer.
uint32_t trackBakeSceneGeneration();

// The track's PRIM payload as GPL holds it, which the scene's item keys are offsets into. False
// when there is no scene.
bool trackBakePrim(const unsigned char*& prim, unsigned& size);

// The scene's vertices and indices are read only by the renderer's geometry upload. Once that
// has run they are freed: the largest thing the scene holds, kept for nothing but a new GL
// context. Does nothing when there is no scene or they are already gone.
void trackBakeDropGeometry();

// Whether the scene still holds its vertices and indices.
bool trackBakeHoldsGeometry();

// For a new GL context after trackBakeDropGeometry: the scene is dropped and the same track is
// baked again on the worker at the next main walk, as after a track load. The game draws the
// track meanwhile.
void trackBakeRebake();

// The process's free address space now (GlobalMemoryStatusEx's ullAvailVirtual); 0 when the call
// fails.
uint64_t trackBakeFreeAddressSpace();

// A bake job has started and not finished.
bool trackBakeRunning();

}  // namespace gplvr
