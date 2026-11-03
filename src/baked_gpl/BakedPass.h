#pragma once

// One baked main pass (docs/audit-2026-09-18/105-baked-renderer-rewrite.md, R1). GPL's main walk
// still decides what is visible: while it runs, a filter on gpl_sceneNodeRenderTable marks each
// baked item it dispatches and skips GPL's handler for it, so no vertex of a baked item reaches
// roglv2. After the walk the visible items are drawn with real depth into a private target and
// composited into the pass's surface, and the roglv2 flush that paints what GPL still drew --
// the horizon underneath, cars, cockpit, flagmen, particles, refused items -- is tested against
// that depth (ResidualDepth.h).
//
// `rendererType()` is read live: 0 refuses every pass, so GPL draws everything as it ships; 1 and
// 2 bake them (2 also marks the rectangles it drew, bakedMarkFrameEnd). The VR menu changes it at
// any time. Nothing is installed until the first pass that wants the renderer, so a session that
// stays at 0 never patches GPL.

#include <cstdint>

namespace gplvr {

struct Binding;

// The rasterizer entry points the pass needs to place its rectangle. Kept for the first pass that
// wants the renderer, which binds the rest and installs the car, flagman and mirror hooks.
void bakedPassBind(const Binding& binding);

// Around gpl_worldRenderSegmentRange in the main walk thunk. Begin returns whether this pass is
// baked; End draws it. Both are no-ops while rendererType() is 0 or no scene is ready.
bool bakedPassBegin(void* sceneParams);
void bakedPassEnd(void* sceneParams);

// The same around the mirror walk (BakedMirror.h): a pass from the mirror camera p[43], its eye
// p[44..46] and basis p[65..73], into that camera's rectangle. End always runs, and draws only
// what Begin opened.
bool bakedPassBeginMirror(void* sceneParams);
void bakedPassEndMirror(void* sceneParams);

// At the end of a frame, with rendererType() = 2: a red square (a cell is 1/300 of the
// rectangle's height) in the top right corner of every rectangle the baked renderer drew this
// frame, so its passes can be told from GPL's own.
void bakedMarkFrameEnd();

// Report 105 section 4.48: around GPL's draw of a cockpit gplvr draws, the node filter hands GPL
// only the polygon and sprite leaves among `glass` (ascending, `count` nodes: the mirror glass,
// CockpitGlass.h) and drops every other one; lines and the control nodes go to GPL as usual.
// Null ends it.
void bakedCockpitGlassOnly(const uintptr_t* glass, uint32_t count);
// What the filter did under bakedCockpitGlassOnly since the last call of this: glass leaves
// handed to GPL, other leaves dropped, lines handed to GPL (CockpitDraw's check reads them).
void bakedCockpitGlassTake(uint32_t& passed, uint32_t& dropped, uint32_t& lines);
// Whether the node filter sees every polygon type: the lit ones (1D 1E 21 22) are hooked only
// where the table holds the stock handlers, and without them it cannot keep the glass alone.
bool bakedFilterSeesEveryLeaf();

// Report 105 section 4.49 (step 2): whether this baked main pass can draw the cockpit's list over
// its finished eye image, glass included. `mirrorMip` is the car set's mirror.mip handle
// (gpl_mirrorMip); its live GL texture is what the glass samples, copied with roglv2's t flip by
// the car renderer. Asked at the cockpit's first call of the pass; on kOverlayReady the cockpit's
// list is made with the glass in it (CarDrawList::overlay) and GPL's draw of the cockpit hands GPL
// no leaf. Else the cockpit is step 1's, and the first refusal of each kind is logged.
enum BakedOverlayRefusal : int {
    kOverlayReady = -1,
    kOverlayNoRenderer,  // not a baked main pass whose cars the car renderer draws
    kOverlayNoMip,       // mirror.mip's handle reaches no mip object with a GL texture
    kOverlayNotFlipped,  // the mip there lacks the t flip roglv2's loader gives mirror.mip alone
    kOverlayNoSize,      // GL reports no size or format for that texture
    kOverlayNoCopy,      // the car renderer could not make its copy of it
    kOverlayRefusals
};
int bakedCockpitOverlayReady(uint32_t mirrorMip);

// From the main walk's thunk once the walk has returned and bakedPassEnd has run. When the pass
// prepared an overlay: flushes roglv2, so everything GPL submitted in the pass (blob shadows,
// particles, debris, the residual items) is painted under the residual test first, drops an arm
// no flush took, then draws the overlay into the framebuffer the pass was composited into, before
// gpl_sceneRender returns -- so before the eye image is copied out and before any 2D.
void bakedPassMainWalkDone();

// True between bakedPassBegin and bakedPassEnd of a baked main pass, or between their mirror
// counterparts (the node filter is collecting). bakedPassDrawsCars: and the car renderer is up,
// so the car hook may leave a car's body to CarRenderer for this pass.
bool bakedPassCollecting();
bool bakedPassDrawsCars();
// While collecting: whether the pass is a mirror pass.
bool bakedPassMirror();

// While collecting: how many entries the pass's visible list holds now, i.e. where in GPL's walk
// the caller is (Renderer::visible). 0 otherwise.
unsigned bakedPassWalkPosition();

// During a baked main pass that draws cars: whether the car renderer holds the rows of a kept list
// with this key and frame count (CarDrawList::contentKey); if so they are pinned for this pass's
// prepare and `draws` is their count, so the assembly may leave its draws out (report 105
// sections 4.27, 4.29).
bool bakedCarRowsPinned(uint32_t key, uint32_t frames, uint32_t& draws);
// The kept program whose lists carry `key` was dropped: the car renderer gives back its rows, on
// the CPU and in the GPU store (report 105 section 4.66). Render thread.
void bakedCarRowsDropped(uint32_t key);

}  // namespace gplvr
