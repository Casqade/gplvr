#pragma once

// One baked main pass (docs/audit-2026-09-18/105-baked-renderer-rewrite.md, R1). GPL's main walk
// still decides what is visible: while it runs, a filter on gpl_sceneNodeRenderTable marks each
// baked item it dispatches and skips GPL's handler for it, so no vertex of a baked item reaches
// roglv2. After the walk the visible items are drawn with real depth into a private target and
// composited into the pass's surface, and the roglv2 flush that paints what GPL still drew --
// the horizon underneath, cars, cockpit, flagmen, particles, refused items -- is tested against
// that depth (ResidualDepth.h).

namespace gplvr {

struct Binding;

// The rasterizer entry points the pass needs to place its rectangle.
void bakedPassBind(const Binding& binding);

// Around gpl_worldRenderSegmentRange in the main walk thunk. Begin returns whether this pass is
// baked; End draws it. Both are no-ops unless bakedRenderer is on and a scene is ready.
bool bakedPassBegin(void* sceneParams);
void bakedPassEnd(void* sceneParams);

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

// The capture probe, written with each half of capture `number` beside gplvr.dll:
// gplvr-captureN-vanilla-items.txt, every item GPL dispatched in the vanilla frame in its order;
// gplvr-captureN-baked-items.txt, the baked frame's visible items by order number; and
// gplvr-captureN-baked-order.bin, its per-pixel order numbers (tools/capture/probe.py reads all
// three).
void bakedProbeWrite(unsigned number);

// At the end of a frame, after bakedCaptureFrameEnd: a 10x10 red square in the top right corner
// of every rectangle the baked renderer drew this frame, so the two renderers can be told apart.
void bakedMarkFrameEnd();

// Debug view 6 (baked::kDebugWithoutGpl): true while a baked main pass is collected in it, when
// the car hook leaves GPL's car draw out. The hook brackets the cockpit's draw with
// bakedCockpitDraw, so the node filter hands the cockpit's nodes to GPL as usual.
bool bakedPassWithoutGpl();
void bakedCockpitDraw(bool drawing);

// True between bakedPassBegin and bakedPassEnd of a baked main pass (the node filter is
// collecting). bakedPassDrawsCars: and bakedCars = 3 with the car renderer up, so the car hook
// may leave a car's body to CarRenderer for this pass.
bool bakedPassCollecting();
bool bakedPassDrawsCars();

// While collecting: how many entries the pass's visible list holds now, i.e. where in GPL's walk
// the caller is (Renderer::visible). 0 otherwise.
unsigned bakedPassWalkPosition();

}  // namespace gplvr
