#pragma once

// Baked cars, R2 stage 2 (docs/audit-2026-09-18/105-baked-renderer-rewrite.md section 4.2): the
// car objects baked from GPL's own memory, one object at a time, the first time the car walk
// (CarWalk.cpp) reaches a leaf of it.
//
// Objects are found through GPL's resource cache (gpl_resourceCacheAcquire 0x4CA420), which
// lists every loaded .3do with its Obj3d: a node belongs to the object whose PRIM holds its
// address. That covers what the walk crosses without a SUBOBJECT -- the swapped-in `car` handle of
// the team model, the tyre object roots a wheel XFORM points at. Texture and sprite handles are
// turned back into file names through the same cache, and the files are read from the car set's
// folders the way GPL's loader found them (CarBake.cpp, resolveFolder).

#include "baked/CarScene.h"

#include <cstdint>

namespace gplvr {

// Binds the game addresses the bake reads, through rebase(). False, with a log line, when one is
// refused; nothing below may be called then.
bool carBakeBind();

// The baked object whose PRIM holds `node`, baked on first sight: its CarScene::objects index,
// and in `primAddress` / `primSize` where that PRIM lives in GPL's memory. -1 when no loaded
// object holds the node or its bake failed; both are counted.
int32_t carBakeObjectOf(uintptr_t node, uintptr_t& primAddress, uint32_t& primSize);

// The CarScene::textures index of the mip GPL binds as `handle`, resolved on first use; -1 when
// it cannot be.
int32_t carBakeTexture(uint32_t handle);

const baked::CarScene& carBakeScene();

// Advances whenever carBakeScene() is dropped and started again (a change of car set), so a GPU
// copy of it knows to start again too. CarScene::objects and textures only grow in between.
uint32_t carBakeGeneration();

struct CarBakeTotals {
    uint32_t objects = 0;
    uint32_t objectsFailed = 0;
    uint32_t objectsRebaked = 0;  // an address that came back holding a different object
    uint32_t textures = 0;
    uint32_t texturesMissing = 0;
    uint32_t sprites = 0;
    uint32_t spritesMissing = 0;
    uint32_t cacheScans = 0;
    // How each file's folder was decided: the only folder holding the name, the folder of the
    // files GPL loaded around it, or still more than one (the first is taken).
    uint32_t folderUnique = 0;
    uint32_t folderByLoadOrder = 0;
    uint32_t folderAmbiguous = 0;
    double   bakeMilliseconds = 0.0;  // object bakes, with the files their MATERIALs name
    double   fileMilliseconds = 0.0;  // every texture and sprite file read and parsed
};
const CarBakeTotals& carBakeTotals();

}  // namespace gplvr
