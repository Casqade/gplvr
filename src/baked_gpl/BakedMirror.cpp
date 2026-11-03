#include "baked_gpl/BakedMirror.h"

#include "baked_gpl/BakedPass.h"
#include "baked_gpl/ResidualDepth.h"

#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
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
    bool  baked = bakedPassBeginMirror(params);
    reinterpret_cast<WorldWalkFn>(kWorldWalk)(a1, a2, a3, a4, a5, a6, a7, a8);
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
