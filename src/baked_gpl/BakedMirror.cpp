#include "baked_gpl/BakedMirror.h"

#include "baked_gpl/BakedPass.h"
#include "baked_gpl/ResidualDepth.h"

#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
#include "MirrorOwnCar.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe; report 105 section 4.18, IDA 2026-10-26) ------------------------

// gpl_sceneRender's mirror walk: `call gpl_worldRenderSegmentRange`, eight __cdecl arguments
// cleaned by the caller (`add esp, 20h` at 0x487A58); its result is never read. Code, the same in
// both links.
typedef void(__cdecl* WorldWalkFn)(int, int, int, int, int, int, int, int);
constexpr uintptr_t     kMirrorWalkSite = 0x00487A4C;
constexpr unsigned char kMirrorWalkStock[5] = {0xE8, 0x0F, 0xDF, 0xFF, 0xFF};
constexpr uintptr_t     kWorldWalk = 0x00485960;

struct State {
    bool installed = false;
    // The refusals already logged once.
    bool refusalSaid[kMirrorRefusalCount] = {};
};

State g_mirror;

// THE MIRROR'S DRAW DISTANCE (Options::mirrorDrawDistance). gpl_sceneRender 0x4879C3..0x487A4B hands the
// mirror walk its far bounds from the camera segment's far-distance table entry, chosen by the
// heading against the track (astern, |a4| > 0x60000000: word 3 ahead, word 2 behind), each times
// flt_539FE8 = 0.5 and NOT times gpl_farDistanceScale, the main walk's own factor (0x487F86):
//     a5 = a7 = ahead far, a6 = a8 = behind far.
// gpl_worldRenderSegmentRange (0x485960) takes, with the camera's two dlongs equal as here, a5 as
// the ahead far bound when |a4| > 0x60000000 (a7 otherwise) and a6 as the behind one, and caps
// the mid- and full-detail distances on each side by a7 and a8: so the halved table bound is the
// mirror's hard cut, and gpl_mirrorDistanceScale cannot reach past it. Every bound is then
// clamped to half a lap. The four are scaled together, by the setting over GPL's 0.5.
constexpr float kMirrorFarStock = 0.5f;
constexpr float kDlongPerMetre = 19685.0f;
float           g_farScaleLogged = -1.0f;

int scaledBound(int bound, float factor) {
    double value = static_cast<double>(bound) * factor;
    if (value > 2147483647.0) return 2147483647;
    if (value < 0.0) return 0;
    return static_cast<int>(value);
}

// Replaces the rel32 of the mirror walk's call. The parameter block is the one gpl_sceneRender
// was handed, as the main walk's thunk reads it (Hooks.cpp). End runs whether or not the pass was
// baked.
//
// A baked pass's leftovers are flushed here, under its residual test, and the test is then
// dropped. Nothing is submitted between the walk and bitmapDraw's own flush (0x487A51..0x487C58:
// the matrix pop, gpl_renderClearDrawnMarks, the mip's bitmap lookup), so the flush only comes
// earlier into the same framebuffer; and a walk that left roglv2 nothing cannot leave its depth
// and rectangle armed for the main pass's horizon.
void __cdecl mirrorWorldWalkThunk(int a1, int a2, int a3, int a4, int a5, int a6, int a7,
                                  int a8) {
    void* params = hooksLastSceneParams();
    // The redirect stays installed after a live switch to the vanilla renderer, which keeps GPL's
    // bounds.
    if (g_options.vr && rendererType() != 0 && g_options.mirrorDrawDistance != kMirrorFarStock) {
        float factor = g_options.mirrorDrawDistance / kMirrorFarStock;
        if (g_farScaleLogged != g_options.mirrorDrawDistance) {
            g_farScaleLogged = g_options.mirrorDrawDistance;
            logf("baked mirrors: the mirror walk's far bounds times %.2f (mirrorDrawDistance "
                 "%.2f of the table's, GPL's %.2f) -- this walk: ahead %.0f -> %.0f m, behind "
                 "%.0f -> %.0f m, before the half-lap clamp",
                 factor, g_options.mirrorDrawDistance, kMirrorFarStock, a5 / kDlongPerMetre,
                 scaledBound(a5, factor) / kDlongPerMetre, a6 / kDlongPerMetre,
                 scaledBound(a6, factor) / kDlongPerMetre);
        }
        a5 = scaledBound(a5, factor);
        a6 = scaledBound(a6, factor);
        a7 = scaledBound(a7, factor);
        a8 = scaledBound(a8, factor);
    }
    bool baked = bakedPassBeginMirror(params);
    mirrorOwnCarWalkBegin(baked && bakedPassDrawsCars());
    reinterpret_cast<WorldWalkFn>(kWorldWalk)(a1, a2, a3, a4, a5, a6, a7, a8);
    mirrorOwnCarWalkEnd();
    bakedPassEndMirror(params);
    if (baked) {
        rasterFlushPending();
        residualDisarm();
    }
}

}  // namespace

void bakedMirrorInstall() {
    State& s = g_mirror;
    if (s.installed) return;

    auto site = reinterpret_cast<unsigned char*>(kMirrorWalkSite);
    if (std::memcmp(site, kMirrorWalkStock, sizeof(kMirrorWalkStock)) != 0) {
        logf("baked mirrors: NOT installed -- %08X does not hold the stock `call "
             "gpl_worldRenderSegmentRange` (%02X %02X %02X %02X %02X); GPL draws the mirrors",
             static_cast<unsigned>(kMirrorWalkSite), site[0], site[1], site[2], site[3], site[4]);
        return;
    }
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&mirrorWorldWalkThunk) -
                                    (kMirrorWalkSite + 5));
    if (!patchBytes(site + 1, &rel, sizeof(rel))) {
        logf("baked mirrors: NOT installed -- could not write the mirror walk call at %08X; GPL "
             "draws the mirrors",
             static_cast<unsigned>(kMirrorWalkSite));
        return;
    }
    s.installed = true;
    logf("baked mirrors: mirror walk call at %08X redirected to %08X -- each mirror walk GPL makes "
         "is a baked pass from the mirror camera",
         static_cast<unsigned>(kMirrorWalkSite),
         static_cast<unsigned>(reinterpret_cast<uintptr_t>(&mirrorWorldWalkThunk)));
    mirrorOwnCarInstall();
}

void bakedMirrorRefused(MirrorRefusal why) {
    if (why < 0 || why >= kMirrorRefusalCount) return;
    // rendererType() 0 and the upload still running are the ordinary states of a pass GPL draws.
    if (why == kMirrorDrawingOff || why == kMirrorUploading) return;
    State& s = g_mirror;
    if (s.refusalSaid[why]) return;
    s.refusalSaid[why] = true;
    static const char* const kWhy[kMirrorRefusalCount] = {
        "the baked pass is not bound",
        "no scene parameter block yet",
        "the mirror camera p[43] is null",
        "the node filter could not be installed",
        "",
        "no baked scene for this track yet",
        "the upload stopped for this scene",
        "the renderer is not up in this context",
        "",
        "p[85] bit 0 is clear: GPL draws into the mip, with no rectangle in GL",
        "the mirror camera's surface has no rectangle"};
    logf("baked mirrors: a mirror walk left to GPL (first time) -- %s", kWhy[why]);
}

}  // namespace gplvr
