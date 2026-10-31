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
#include <cstdlib>

namespace gplvr {
namespace {

unsigned g_captureNumber = 0;

// A 32-bit TGA, bottom row first, alpha kept: GPL's copy-out takes the alpha into mirror.mip too.
bool dumpRectRgba(const GlFunctions& gl, const int rect[4], const char* path) {
    if (!gl.readPixels || rect[2] <= 0 || rect[3] <= 0) return false;
    size_t bytes = static_cast<size_t>(rect[2]) * static_cast<size_t>(rect[3]) * 4;
    auto*  pixels = static_cast<unsigned char*>(std::malloc(bytes));
    if (!pixels) return false;
    // 0x80E1 is GL_BGRA, TGA's byte order.
    if (!glReadRectPacked(gl, rect[0], rect[1], rect[2], rect[3], 0x80E1, pixels)) {
        std::free(pixels);
        return false;
    }
    unsigned char header[18] = {};
    header[2] = 2;
    header[12] = static_cast<unsigned char>(rect[2] & 0xFF);
    header[13] = static_cast<unsigned char>((rect[2] >> 8) & 0xFF);
    header[14] = static_cast<unsigned char>(rect[3] & 0xFF);
    header[15] = static_cast<unsigned char>((rect[3] >> 8) & 0xFF);
    header[16] = 32;
    header[17] = 8;  // eight alpha bits, origin bottom left
    bool  ok = false;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") == 0 && file) {
        ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
             std::fwrite(pixels, 1, bytes, file) == bytes;
        std::fclose(file);
    }
    std::free(pixels);
    return ok;
}

}  // namespace

void bakedCaptureMirrorRect(const int rect[4], const char* half, unsigned pass,
                            const char* stage) {
    // The vanilla half's mirror passes run before its frame end numbers the capture.
    unsigned number = half[0] == 'v' ? g_captureNumber + 1 : g_captureNumber;
    GlFunctions gl;
    char        path[MAX_PATH];
    sprintf_s(path, "%sgplvr-capture%u-%s-mirror%u-%s.tga", pathsOwnDirectory(), number, half,
              pass, stage);
    bool ok = glResolve(gl) && dumpRectRgba(gl, rect, path);
    logf("baked mirrors: capture %u %s mirror pass %u %s -- %s %s, rect %d,%d %dx%d", number,
         half, pass, stage, ok ? "written to" : "FAILED to write", path, rect[0], rect[1],
         rect[2], rect[3]);
}

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
