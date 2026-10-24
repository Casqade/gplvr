// bakedCaptureKey's writer: the two halves of an A/B capture, GPL's own frame and the baked one
// after it, each main pass as a TGA beside gplvr.dll with the camera it was drawn from in the log.
// Pause the game before pressing the key, so the two frames show the same instant.

#include "baked_gpl/BakedPass.h"

#include "GlInterop.h"
#include "Log.h"
#include "MenuScale.h"
#include "Paths.h"

#include <cmath>
#include <cstdio>

namespace gplvr {
namespace {

unsigned g_captureNumber = 0;

}  // namespace

void bakedCaptureFrameEnd() {
    BakedCaptureView views[2];
    int              count = 0;
    const char*      half = bakedCaptureTake(views, count);
    if (!half) return;

    // The frame is still bound for drawing here (off-screen in VR, the back buffer in flat mode),
    // and whatever roglv2 still has queued belongs to it.
    rasterFlushPending();

    bool first = half[0] == 'v';
    if (first) ++g_captureNumber;
    GlFunctions gl;
    if (!glResolve(gl)) {
        logf("baked: capture %u %s -- GL did not resolve, nothing written", g_captureNumber, half);
        return;
    }
    for (int i = 0; i < count; ++i) {
        const BakedCaptureView& view = views[i];
        char path[MAX_PATH];
        sprintf_s(path, "%sgplvr-capture%u-%s%s.tga", pathsOwnDirectory(), g_captureNumber, half,
                  count > 1 ? (i ? "-eye1" : "-eye0") : "");
        bool ok = glDumpRectToTga(gl, view.rect[0], view.rect[1], view.rect[2], view.rect[3], path);
        float heading = std::atan2(view.forward[1], view.forward[0]) * 57.29578f;
        float pitch = std::asin(view.forward[2] < -1.0f ? -1.0f
                                : view.forward[2] > 1.0f ? 1.0f
                                                         : view.forward[2]) *
                      57.29578f;
        logf("baked: capture %u %s%s -- %s %s; eye (%.2f, %.2f, %.2f), forward (%.3f, %.3f, "
             "%.3f) = heading %.1f deg pitch %.1f deg, selector dlong %.0f dlat %.0f (GPL's raw "
             "units), rect %d,%d %dx%d, debug view %d",
             g_captureNumber, half, count > 1 ? (i ? " eye 1" : " eye 0") : "",
             ok ? "written to" : "FAILED to write", path, view.eye[0], view.eye[1], view.eye[2],
             view.forward[0], view.forward[1], view.forward[2], heading, pitch, view.dlong,
             view.dlat, view.rect[0], view.rect[1], view.rect[2], view.rect[3], view.debugView);
    }
    bakedProbeWrite(g_captureNumber);
}

}  // namespace gplvr
