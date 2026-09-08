#pragma once

// Locates GPL's rasterizer function-pointer table and its render chain at runtime,
// without a single hardcoded address.
//
// Everything keys off the rasterizer entry-point name literals, which are the one
// thing every build of the game must contain verbatim. The chain is:
//
//   "polySetDest"  -- a string in the image
//     -> push <thatString>            the GetProcAddress call in the loader
//       -> mov [imm32], eax           the writable global holding the resolved pointer
//         -> call [thatGlobal]        exactly two call sites in the whole image
//           -> the one whose function also contains D9 F2 (fptan) is cameraBind,
//              because cameraBind computes the projection scale as tan(fov * 0.5)
//             -> the function calling cameraBind exactly twice is sceneRender,
//                because it renders the mirror pass and then the main pass
//               -> sceneRender's sole caller is the sim frame render
//
// Every step asserts how many hits it expects. If any of them is not satisfied we
// refuse to install anything: misfiring on an unknown build is far worse than
// declining to start.

#include "Pe.h"

#include <cstdint>
#include <vector>

namespace gplvr {

// Resolve one rasterizer entry point's global by its export name, the way the table
// below is filled. For the entry points the table does not carry a field for.
void** rasterGlobal(const ModuleImage& image, const char* name);

struct RasterTable {
    void** screenDisplay = nullptr;
    void** screenGetBackBuffer = nullptr;
    void** polySetDest = nullptr;
    void** polyDraw = nullptr;
    void** bitmapInstance = nullptr;

    // Stereo's working set. bitmapInstanceAlloc is the one that carves a sub-rect view
    // out of an existing surface -- "bitmapInstance" is a different entry point, and
    // the two names are easy to confuse.
    void** bitmapWidth = nullptr;
    void** bitmapHeight = nullptr;
    void** bitmapInstanceAlloc = nullptr;
    void** bitmapInstanceFree = nullptr;
};

struct Binding {
    ModuleImage image;
    RasterTable raster;

    uintptr_t cameraBind = 0;
    uintptr_t sceneRender = 0;
    uintptr_t simFrameRender = 0;

    // Reached from cameraBind, which calls rebuildProjection first thing when the camera
    // is dirty; rebuildProjection in turn has exactly two callers in the image --
    // cameraBind and cameraCreate. Optional: if either step is ambiguous, stereo stays
    // unavailable and everything else still works.
    uintptr_t cameraRebuildProjection = 0;
    uintptr_t cameraCreate = 0;

    // Addresses of the E8 opcodes calling sceneRender. We redirect these rather
    // than detouring sceneRender's prologue -- no trampoline, trivially reversible.
    std::vector<uintptr_t> sceneRenderCallSites;

    // The cameraBind calls that live INSIDE sceneRender, which is where the eye
    // frustum has to be re-applied.
    //
    // sceneRender calls cameraSetFov on the camera it is about to draw with, 27 bytes
    // before each of these -- so anything we write into the camera object before
    // sceneRender runs is overwritten before the projection is ever rebuilt. That is
    // why a frustum written before sceneRender has no effect at any value: two frusta an
    // octave apart produced pixel-identical halves. Redirecting these sites puts us
    // after the game's write and before cameraBind rebuilds the matrix.
    //
    // It writes the PRINCIPAL POINT on the very next line too -- see stereoOnCameraBind.
    //
    // Empty means the fix is not installed and the eyes render the game's own fov; that
    // is a degraded picture, not a crash, so it is a warning rather than a failure.
    std::vector<uintptr_t> cameraBindCallSitesInScene;

    bool complete() const {
        return raster.screenDisplay && cameraBind && sceneRender &&
               !sceneRenderCallSites.empty();
    }
};

// Resolves the rasterizer globals only. Cheap, and they stay null until the game
// has actually loaded a rasterizer DLL -- so this doubles as our "is it ready yet"
// probe.
bool discoverRasterTable(const ModuleImage& image, RasterTable& out);

// Full discovery. Logs each step and what it found.
bool discover(Binding& out);

}  // namespace gplvr
