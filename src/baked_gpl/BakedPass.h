#pragma once

// One baked main pass (docs/audit-2026-09-18/105-baked-renderer-rewrite.md, R1). GPL's main walk
// still decides what is visible: while it runs, a filter on gpl_sceneNodeRenderTable marks each
// baked item it dispatches and skips GPL's handler for it, so no vertex of a baked item reaches
// roglv2. After the walk the visible items are drawn with real depth into a private target and
// composited into the pass's surface, and the roglv2 flush that paints what GPL still drew --
// the horizon underneath, cars, cockpit, flagmen, particles, refused items -- is tested against
// that depth (ResidualDepth.h).

#include <cstdint>

namespace baked {
struct GpuPassTimes;
}

namespace gplvr {

struct Binding;

// The rasterizer entry points the pass needs to place its rectangle.
void bakedPassBind(const Binding& binding);

// Around gpl_worldRenderSegmentRange in the main walk thunk. Begin returns whether this pass is
// baked; End draws it. Both are no-ops unless bakedRenderer is on and a scene is ready.
bool bakedPassBegin(void* sceneParams);
void bakedPassEnd(void* sceneParams);

// The same around the mirror walk (BakedMirror.h, bakedMirrors = 1): a pass from the mirror
// camera p[43], its eye p[44..46] and basis p[65..73], into that camera's rectangle. Begin counts
// each refusal by reason; End always runs, and draws only what Begin opened.
bool bakedPassBeginMirror(void* sceneParams);
void bakedPassEndMirror(void* sceneParams);

// bakedRendererKey: baked drawing on and off in the same session. bakedCaptureKey: the next
// frame is drawn by GPL alone and the one after it from the bake, for bakedCaptureFrameEnd.
void bakedPassPollKey();

// One main pass of a captured frame: where it was drawn (GL window pixels of the framebuffer
// it was drawn into) and where it looked from.
struct BakedCaptureView {
    int   rect[4] = {0, 0, 0, 0};
    float eye[3] = {0.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, 0.0f};
    float dlong = 0.0f;  // gpl_selectorCameraDlong as the selectors read it, GPL's raw units
    float dlat = 0.0f;   // and gpl_selectorCameraDlat
    int   debugView = 0;  // bakedDebugKey's view (baked::DebugView) the baked half was drawn in
};

// At the end of a frame: "vanilla" or "baked" when this frame is half of a capture, with the
// views of its main passes (one in flat mode, one per eye in VR), else null. Advances the
// capture to its next half.
const char* bakedCaptureTake(BakedCaptureView views[2], int& count);

// Writes the half of a capture this frame holds (BakedCapture.cpp), beside gplvr.dll.
void bakedCaptureFrameEnd();

// The mirror rectangles of a capture (report 105 section 4.18), each the framebuffer bound for
// reading at `rect` (GL window pixels), RGBA, written beside gplvr.dll as
// gplvr-capture<N>-<half>-mirror<pass>-<stage>.tga (BakedCapture.cpp). `half` is "vanilla" or
// "baked", `pass` the frame's mirror pass from 0.
void bakedCaptureMirrorRect(const int rect[4], const char* half, unsigned pass,
                            const char* stage);

// From the mirror walk's thunk once the walk has returned and the pass has ended (and, for a
// baked pass, been flushed): in a frame that is half of a capture, flushes what GPL drew and
// writes the mirror rectangle as it goes into mirror.mip.
void bakedPassMirrorWalkDone(bool baked);

// The capture probe, written with each half of capture `number` beside gplvr.dll:
// gplvr-captureN-vanilla-items.txt, every item GPL dispatched in the vanilla frame in its order;
// gplvr-captureN-baked-items.txt, the baked frame's visible items by order number; and
// gplvr-captureN-baked-order.bin, its per-pixel order numbers (tools/capture/probe.py reads all
// three).
void bakedProbeWrite(unsigned number);

// At the end of a frame, after bakedCaptureFrameEnd: the debug view's number in red block digits
// (a cell is 1/300 of the rectangle's height) in the top right corner of every rectangle the
// baked renderer drew this frame, so the two renderers and the views can be told apart.
void bakedMarkFrameEnd();

// Debug view 6 (baked::kDebugWithoutGpl): true while a baked main pass is collected in it, when
// the car hook leaves GPL's car draw out. The hook brackets the cockpit's draw with
// bakedCockpitDraw, so the node filter hands the cockpit's nodes to GPL as usual.
bool bakedPassWithoutGpl();
void bakedCockpitDraw(bool drawing);

// Report 105 section 4.48: around GPL's draw of a cockpit gplvr draws, the node filter hands GPL
// only the polygon and sprite leaves among `glass` (ascending, `count` nodes: the mirror glass,
// CockpitGlass.h) and drops every other one; lines and the control nodes go to GPL as usual.
// Null ends it.
void bakedCockpitGlassOnly(const uintptr_t* glass, uint32_t count);
// What the filter did under bakedCockpitGlassOnly since the last call of this: glass leaves
// handed to GPL, other leaves dropped, lines handed to GPL.
void bakedCockpitGlassTake(uint32_t& passed, uint32_t& dropped, uint32_t& lines);
// Whether the node filter sees every polygon type: the lit ones (1D 1E 21 22) are hooked only to
// count and only where the table holds the stock handlers, and without them it cannot keep the
// glass alone.
bool bakedFilterSeesEveryLeaf();

// Report 105 section 4.49 (bakedCockpitOverlay = 1, step 2): whether this baked main pass can draw
// the cockpit's list over its finished eye image, glass included. `mirrorMip` is the car set's
// mirror.mip handle (gpl_mirrorMip); its live GL texture is what the glass samples, copied with
// roglv2's t flip by the car renderer. Asked at the cockpit's first call of the pass; on
// kOverlayReady the cockpit's list is made with the glass in it (CarDrawList::overlay) and GPL's
// draw of the cockpit hands GPL no leaf. Else the cockpit is step 1's, counted by the reason.
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
// counterparts (the node filter is collecting). bakedPassDrawsCars: and bakedCars = 3 with the
// car renderer up, so the car hook may leave a car's body to CarRenderer for this pass.
bool bakedPassCollecting();
bool bakedPassDrawsCars();
// While collecting: whether the pass is a mirror pass.
bool bakedPassMirror();
// Debug view 15 with bakedMirrors = 1: every mirror walk is GPL's, and the car hook walks GPL's
// mirror cars beside its draw and checks the polygon count, as at bakedCars = 1.
bool bakedPassMirrorsByGpl();
// Debug view 16: the second eye walks every car itself (bakedCarShare off for the moment).
bool bakedPassCarsUnshared();
// Debug view 17: the first eye decides every gate itself (bakedCarUnion off for the moment).
bool bakedPassCarsUnconditioned();
// Debug view 18: no car list is kept across frames (bakedCarProgram off for the moment).
bool bakedPassCarsUnkept();

// While collecting: how many entries the pass's visible list holds now, i.e. where in GPL's walk
// the caller is (Renderer::visible). 0 otherwise.
unsigned bakedPassWalkPosition();

// During a baked main pass that draws cars: whether the car renderer holds the rows of a kept list
// with this key and frame count (CarDrawList::contentKey); if so they are pinned for this pass's
// prepare and `draws` is their count, so the assembly may leave its draws out (report 105
// sections 4.27, 4.29).
bool bakedCarRowsPinned(uint32_t key, uint32_t frames, uint32_t& draws);

// Where a timed call ran (report 105 sections 4.44, 4.46): inside a baked main pass's walk window,
// a baked mirror pass's, or outside both.
enum BakedTimeKind : int { kBakedMain, kBakedMirror, kBakedOutside, kBakedTimeKinds };
int bakedTimeKind();

// The node filter's calls since the last call of this, and the TSC ticks they took, estimated
// from the sampled ones, by BakedTimeKind; starts them again.
void bakedFilterTake(uint64_t calls[kBakedTimeKinds], uint64_t ticks[kBakedTimeKinds]);

// The baked passes' walk windows (beginPass to endPass, GPL's world walk with every hook inside
// it) since the last call of this: QueryPerformanceCounter ticks and passes, main [0] and mirror
// [1]; starts them again.
void bakedPassWindowsTake(int64_t ticks[2], uint32_t passes[2]);

// The second's GPU spans of one class of pass (baked::GpuPassTimes), one line prefixed with `who`.
void bakedLogGpuSpans(const char* who, const baked::GpuPassTimes& gpu);

}  // namespace gplvr
