#include "Hud.h"

#include "DataRebase.h"
#include "FrameBudget.h"
#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
#include "MirrorCache.h"
#include "Pe.h"
#include "RenderTarget.h"
#include "ReplayPanel.h"
#include "Stereo.h"
#include "Window.h"
#include "fast_text/FastText.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// void __cdecl drawDebugText(surface, bgraColour, x, y, text, drawBackground)
//
// The destination is a rasterizer surface handle, in the same space bitmapInstanceAlloc
// returns -- which is why it is declared here as the void* Stereo already holds rather
// than as the int the disassembly shows.
typedef void(__cdecl* DrawDebugTextFn)(void* surface, unsigned colour, int x, int y,
                                       const char* text, int background);
typedef int(__cdecl* BitmapWidthFn)(void* bitmap);

void**          g_slot = nullptr;
DrawDebugTextFn g_original = nullptr;
void**          g_bitmapWidthSlot = nullptr;
void**          g_bitmapHeightSlot = nullptr;

// GPL only ever anchors an item to the left edge, the centre, or the right edge, and the
// widest item is a driver name. Twenty calls is the whole HUD; the ceiling is generous
// and the overflow is logged rather than silently dropped.
constexpr int kMaxCalls = 48;
constexpr int kMaxText = 96;

// gpl_rasterDrawDebugText's slot in the reference link, used only to check that the slot found
// by entry-point name is the one the exe actually calls. Reference-link address, so it goes
// through DataRebase before the comparison.
constexpr uintptr_t kDebugTextSlot = 0x00866E08;

struct Call {
    void*    surface;
    unsigned colour;
    int      x;
    int      y;
    int      background;
    char     text[kMaxText];
};

Call g_calls[kMaxCalls];
int  g_callCount = 0;
int  g_overflowReports = 4;
bool g_replaying = false;

int  g_reports = 4;
int  g_lastDisparity = -9999;

// EVERY DISTINCT DESTINATION, NOT THE LAST ONE.
//
// This was a single `lastPassthrough` pointer, and the comparison `surface != last`
// silenced exactly the evidence it existed to collect: the menu's version string set it
// to surface 2, and the in-race frame-time readout ALSO goes to surface 2 -- it targets
// screenGetBackBuffer() -- so every one of those calls compared equal and printed
// nothing. A silent log was then read as "the thunk is never called in a race", which
// sent three explanations down the wrong road.
//
// A set cannot do that. Eight is more destinations than GPL has.
void* g_seenSurfaces[8] = {};
int   g_seenCount = 0;

bool firstSightOf(void* surface) {
    for (int i = 0; i < g_seenCount; ++i)
        if (g_seenSurfaces[i] == surface) return false;
    // Surface handles are recycled, so a screen-wide text layer rebuilt every frame is a new
    // handle each time. Once the table is full, nothing more is reported.
    if (g_seenCount >= 8) return false;
    g_seenSurfaces[g_seenCount++] = surface;
    if (g_seenCount == 8)
        logf("hud: eight surfaces reported as passed through -- later ones are not logged");
    return true;
}

// What the last frame actually asked for, reported when the number changes. This is the
// one line that says whether the HUD block in gpl_simFrameRender is running at all:
// zero means gpl_optShowTimes is closed or the HUD is drawn by something else entirely,
// and twenty-odd means it is reaching us and the question is only where it lands.
int g_lastReportedCount = 0;
int g_countReports = 8;

// Handles that resolved to a freed table slot. Above zero means gplvr is holding a surface
// handle past its life -- which used to be a crash and is now a refusal, but is still a bug
// wherever the handle came from.
unsigned g_staleHandleReads = 0;
int      g_staleHandleReports = 3;

// Said out loud the first few times, because a counter nobody prints is a crash that became a
// silent wrong answer. Not in the once-a-second burst: this should never fire at all.
void noteStaleHandle(void* bitmap) {
    ++g_staleHandleReads;
    if (g_staleHandleReports <= 0) return;
    --g_staleHandleReports;
    logf("hud: surface %p resolved to a FREED handle-table slot -- its size reads as 0 and the "
         "per-eye split declines. This is gplvr holding a surface handle past its life; the "
         "handle carries no generation, so nothing here can tell a recycled one from a live "
         "one. #%u this session.",
         bitmap, g_staleHandleReads);
}

// A NULL SURFACE IS NOT A QUESTION THE RASTERIZER CAN BE ASKED.
//
// roglv2 resolves a handle as `*(handleTable + 2) + 8 * handle` and dereferences what it
// finds, with no check of its own. Handle 0 walks into a null vtable and takes the process
// with it. That is exactly what happened when the census asked for the size of
// stereoSourceSurface() while still in the front end, where there are no eyes and the
// source is null -- an instrument that crashed the thing it was measuring.
// A NON-NULL HANDLE IS NOT A LIVE ONE EITHER, and that is the half this guard used to miss.
// The 2026-10-09 crash was `roglv2!bitmapWidth+0x27` reading `[eax+0Ch]` with eax = 0: handle
// 0x6F was not null, it was RECYCLED -- the lobby-to-main-menu teardown freed the block and
// left table slots 0x6D..0x73 zeroed, so the lookup produced a null POINTER from a perfectly
// ordinary-looking handle. Nothing gplvr holds can tell the two apart, because a handle carries
// no generation and roglv2 validates nothing.
//
// So these two cannot be allowed to fault. A size that cannot be read is 0, which every caller
// already treats as "do not split", and a diagnostic accessor must not be able to kill the
// process it is measuring. SEH costs nothing until something faults, and only PODs live here.
int bitmapWidth(void* bitmap) {
    if (!bitmap) return 0;
    if (!g_bitmapWidthSlot || !*g_bitmapWidthSlot) return 0;
    __try {
        return reinterpret_cast<BitmapWidthFn>(*g_bitmapWidthSlot)(bitmap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteStaleHandle(bitmap);
        return 0;
    }
}

int bitmapHeight(void* bitmap) {
    if (!bitmap) return 0;
    if (!g_bitmapHeightSlot || !*g_bitmapHeightSlot) return 0;
    __try {
        return reinterpret_cast<BitmapWidthFn>(*g_bitmapHeightSlot)(bitmap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteStaleHandle(bitmap);
        return 0;
    }
}

// screenGetBackBuffer, for telling the window's own surface from the frame GPL lays its 2D out
// against. They are the same size, so size alone cannot separate them.
typedef void*(__cdecl* BackBufferFn)();
void** g_backBufferSlot = nullptr;

// CAN THIS DESTINATION BE SPLIT PER EYE AT ALL?
//
// Only when the eyes are sub-rects of the very surface GPL is drawing into. Re-issuing a 2D
// call with an eye surface as its destination moves WHERE IN THAT SURFACE the pixels go; it
// does not move them to a different framebuffer. So the split works when the eye targets were
// carved out of GPL's own destination, and does nothing whatsoever when they live on a separate
// off-screen target.
//
// THIS WAS A SIZE TEST AGAINST stereoSourceSurface() AND THAT WAS CORRECT. I replaced it with a
// test against the engine's display mode, on the reading that the old one had rotted when the
// world moved off-screen -- 2560x1440 destination, 6344x3172 source, no longer equal, every
// stamp passing through. The sizes stopped matching because the PREMISE stopped holding, and
// the old test was right to decline. Measured 2026-10-07, which is what settles it: with the
// mode test in place GPL's "car must be stopped" text vanished from the MONITOR as well, while
// GPL's own fps counter -- which targets the back buffer and is excluded here -- stayed. The
// split had taken the text away from the one surface it was visible on and put it in a buffer
// nobody reads.
//
// So the eyes must come from this destination. What GPL's 2D needs in order to reach a headset
// that renders off-screen is a quad layer, not a per-eye re-issue: the 2D surface is readable as
// a GL texture, and a layer is composited rather than drawn into an eye.
bool destinationCanSplit(void* dest, int& fullWidth, int& fullHeight) {
    fullWidth = bitmapWidth(dest);
    fullHeight = bitmapHeight(dest);
    if (fullWidth <= 0 || fullHeight <= 0) return false;

    void* source = stereoSourceSurface();
    if (!source) return false;
    if (fullWidth != bitmapWidth(source) || fullHeight != bitmapHeight(source)) return false;

    // AND THE SOURCE MUST STILL BE THE SURFACE THE EYES WERE CARVED FROM. A recycled source
    // handle reads as whatever now owns the slot: after a replay it was the mirror camera's
    // 512x512 surface, mirror.mip's copy-out matched it, and was split into eye instances whose
    // parent had been freed -- roglv2 then read a GL framebuffer as memory and faulted
    // (gplc67.exe.18520.dmp). The size it was carved at cannot be recycled with it.
    int carvedWidth = 0;
    int carvedHeight = 0;
    if (!stereoSourceCarvedSize(carvedWidth, carvedHeight) || fullWidth != carvedWidth ||
        fullHeight != carvedHeight)
        return false;

    // Only a destination roglv2 draws through the framebuffer is part of a frame. mirror.mip is
    // a texture, and splitting a copy into it changes roglv2's blit from a GL copy to a CPU one.
    // Asked through fast text's handle-table read, which answers false when it is not located;
    // the size checks above stand alone then.
    int  originX = 0;
    int  originY = 0;
    int  width = 0;
    int  height = 0;
    bool framebuffer = false;
    if (fastTextSurfaceInfo(static_cast<int>(reinterpret_cast<intptr_t>(dest)), originX, originY,
                            width, height, framebuffer) &&
        !framebuffer)
        return false;

    // THE BACK BUFFER CAN BE THE SAME SIZE AND IS NOT THIS. The frame-time readout targets it --
    // surface 2, the one whose collisions with the menu's version string cost three
    // explanations. Splitting it would put a diagnostic readout inside the picture twice.
    if (g_backBufferSlot && *g_backBufferSlot) {
        auto getBackBuffer = reinterpret_cast<BackBufferFn>(*g_backBufferSlot);
        if (dest == getBackBuffer()) return false;
    }
    return true;
}

// THE ANCHOR REMAP, AND WHY THREE CLASSES ARE ENOUGH.
//
// Every item in the HUD is placed at x = 0-ish, x = W/2-ish or x = W-ish, and nothing
// else. The three classes below turn each of those into the same relative place in an
// eye that is half as wide:
//
//   x = 1, 49                    -> unchanged; still 1 and 49 in a W/2-wide eye
//   x = W/2 - 64, W/2 + 8        -> W/4 - 64, W/4 + 8; dead centre of the eye
//   x = W - 81, W - 57, W-1-8n   -> the same distance from the eye's right edge
//
// The quarter-width thresholds are what sort an item into its class. They have room to
// spare: the nearest item to a boundary is the centre block's W/2 - 64, which at the
// smallest eye GPL can render is still far above W/4.
// GPL'S 2D IS LAID OUT AGAINST THE ENGINE FRAME, AND THE EYES NEED NOT BE THAT SIZE.
//
// remapAnchor below puts an item into a space half the destination's width, and for a long time
// that was the whole mapping, because the eyes WERE the two halves of the destination. Once the
// world renders off-screen they are not: a 2560x1440 engine frame against two 3172x3172 eyes.
// Translating alone then leaves the whole layout bunched into the top-left of each eye at the
// wrong relative position, which is what the replay control band showed.
//
// Only positions scale. The glyphs and bitmaps drawn at them keep their authored pixel size, so
// the layout comes out proportionally right and the art stays the angular size it already was.
// That is the most a per-eye re-issue can do; making the art itself bigger needs GPL's 2D on a
// quad layer, where the whole surface is scaled as one.
void scaleToEye(int& x, int& y, int fullWidth, int fullHeight, int eyeWidth, int eyeHeight) {
    int half = fullWidth / 2;
    if (half > 0 && eyeWidth > 0)
        x = static_cast<int>(static_cast<long long>(x) * eyeWidth / half);
    if (fullHeight > 0 && eyeHeight > 0)
        y = static_cast<int>(static_cast<long long>(y) * eyeHeight / fullHeight);
}

int remapAnchor(int x, int fullWidth) {
    if (x >= 3 * fullWidth / 4) return x - fullWidth / 2;
    if (x >= fullWidth / 4) return x - fullWidth / 4;
    return x;
}

// Horizontal disparity in pixels for `vrHudDepth` metres, or 0 for infinity.
//
// The left eye's copy goes RIGHT of centre and the right eye's LEFT of centre, which is
// convergent and pulls the HUD towards the viewer. With f the eye's focal length in
// pixels, the apparent distance is Z = f * IPD / d, so d = f * IPD / Z.
//
// The IPD is the runtime's, not a constant: the eye separation comes back from
// vrAcquireEyes with the poses, and it is the user's real one. Without a tracked frame
// there is no separation to use and no headset to be uncomfortable in, so the disparity
// is zero and the two copies are identical.
int disparityPixels(int eyeWidth) {
    if (g_options.vrHudDepth <= 0.0f) return 0;

    float fov = 0.0f;
    float separation = 0.0f;
    if (!stereoEyeGeometry(fov, separation)) return 0;

    float halfAngle = fov * 0.5f;
    if (halfAngle <= 0.0f || halfAngle >= 1.5f) return 0;

    float focal = (static_cast<float>(eyeWidth) * 0.5f) / std::tan(halfAngle);
    float pixels = focal * separation / g_options.vrHudDepth;

    // A disparity wider than the item it separates is not a depth, it is two HUDs. The
    // ceiling is a twentieth of the eye, which at any sane depth is never reached.
    float ceiling = static_cast<float>(eyeWidth) * 0.05f;
    if (pixels > ceiling) pixels = ceiling;
    return static_cast<int>(pixels + 0.5f);
}

// One HUD item, sent wherever it should go. Shared by the live call and the replay, so
// the two can never drift apart.
void emit(const Call& call) {
    if (!g_original) return;

    // THE FRONT END HAS NO EYES, AND THE SOURCE HANDLE IT WOULD ASK ABOUT IS A CORPSE.
    //
    // This is the guard `bitmapDrawThunk` already carries and this caller did not -- the one of
    // the callers of destinationCanSplit that was unprotected, and
    // the one that crashed on 2026-10-09. MenuScale's rasterFlushPending synthesises a
    // drawDebugText of "" against the back buffer once per front-end frame, so this path runs
    // continuously in the menus, with g_state.sourceSurface still holding the race's 0x6F.
    // Leaving a race never clears it: stereoRelease() is the only writer of null and both of
    // its call sites are inside the world-render build.
    if (windowInFrontEnd()) {
        g_original(call.surface, call.colour, call.x, call.y, call.text, call.background);
        return;
    }

    int rect[2][4];
    void* left = stereoEyeSurface(0);
    void* right = stereoEyeSurface(1);

    // Everything that is not GPL's 2D on the engine's own frame goes through untouched. The
    // frame-time readout is the one to keep in mind: its destination is
    // screenGetBackBuffer(), not the engine frame, so it fails this test and stays where GPL
    // put it.
    //
    // Declining is also correct whenever the eyes are not carved out of this destination --
    // see destinationCanSplit, which is the whole of why.
    int fullWidth = 0;
    int fullHeight = 0;
    if (!left || !right || !destinationCanSplit(call.surface, fullWidth, fullHeight) ||
        !stereoEyeRect(0, rect[0]) || !stereoEyeRect(1, rect[1])) {
        // Say so, once per destination. The pass-through is correct for the frame-time
        // readout and wrong for the in-race HUD, and from the outside the two look
        // identical: a HUD that never reached an eye and a HUD that was never meant to.
        // Naming the surface is what separates them without another guess.
        if (firstSightOf(call.surface)) {
            void* source = stereoSourceSurface();
            logf("hud: \"%s\" went to surface %p (%dx%d) UNCHANGED -- the stereo source is %p "
                 "(%dx%d), eyes %p/%p. Expected for the frame-time readout, whose destination "
                 "is the back buffer, and expected for everything while the world renders "
                 "off-screen: the eyes are not part of this surface, so a per-eye re-issue "
                 "would put the pixels in a buffer nobody reads.",
                 call.text, call.surface, fullWidth, fullHeight, source, bitmapWidth(source),
                 bitmapHeight(source), left, right);
        }
        g_original(call.surface, call.colour, call.x, call.y, call.text, call.background);
        return;
    }

    int x = remapAnchor(call.x, fullWidth);
    int y = call.y;

    scaleToEye(x, y, fullWidth, fullHeight, rect[0][2], rect[0][3]);

    int disparity = disparityPixels(rect[0][2]);
    if (disparity != g_lastDisparity && g_reports > 0) {
        --g_reports;
        g_lastDisparity = disparity;
        logf("hud: drawn once per eye -- %d px of convergent disparity for vrHudDepth = "
             "%.2f, eye %dx%d out of a %d-wide surface%s",
             disparity, g_options.vrHudDepth, rect[0][2], rect[0][3], fullWidth,
             disparity ? "" : " (at infinity)");
    }

    // y needs no anchor remap: each eye is the full height of the buffer, and a sub-rect
    // instance already offsets into the root by its own origin.
    g_original(left, call.colour, x + disparity / 2, y, call.text, call.background);
    g_original(right, call.colour, x - disparity / 2, y, call.text, call.background);
}

// -- THE 2D CENSUS ---------------------------------------------------------------------
//
// If the in-race HUD is not text then it is bitmaps, and `drawDebugText` is the ONLY text
// entry in the rasterizer's whole export list -- the 2D entries are bitmapDraw,
// bitmapScale, stampDraw and fillRect. The user's `vrOffscreen = 1` symptom points
// straight at this: only the oil and pressure gauges survive, which is what happens to
// LEFT-ANCHORED items when the surface grows to 6344 wide and everything anchored centre
// or right lands outside the eye on show.
//
// The signature is read from roglv2, not guessed: bitmapDraw(int, int, int, int) at
// 0x20001950 looks its FIRST argument up in the handle table as the source and its FOURTH
// as the destination, and hands the middle pair to rogl_blitDispatch as the position.
//
// This only counts and names destinations. It changes nothing about the drawing.
typedef int(__cdecl* BitmapDrawFn)(int source, int x, int y, int dest);

void**       g_bitmapDrawSlot = nullptr;
BitmapDrawFn g_bitmapDrawOriginal = nullptr;

// SCOPED TO THE PHASE UNDER TEST, because a budget that is not gets spent before it
// arrives. The first run of this census named ten destinations between 2.2 and 2.6
// seconds -- all of them front-end menu art -- and had nothing left for the eighty
// seconds of racing it was built for. That is the third instrument in this file to fail
// the same way: the mirror reports spent themselves at the seat, the surface dedupe
// silenced itself on a repeat, and now this. Everything here resets when the engine
// changes mode, so the race gets its own budget and its own table.
int   g_seenDest[24] = {};
int   g_seenDestCount = 0;
int   g_bitmapDrawReports = 12;
int   g_bitmapDrawCount = 0;
int   g_bitmapDrawHighWater = 0;
bool  g_wasFrontEnd = true;

// How many bitmapDraw calls the per-eye branch took, against how many it saw. A split that
// never runs reads exactly like one that is not needed, which is what cost this path its
// first two builds.
int   g_bitmapSplitCount = 0;
int   g_bitmapSplitReports = 4;

// Destinations whose refusal has been named already, so a per-frame call does not print a line
// a frame. Separate from g_seenDest, which the census owns and spends on its own budget.
int   g_refusedDest[16] = {};
int   g_refusedDestCount = 0;
int   g_bitmapRefuseReports = 8;

bool knownRefusal(int dest) {
    for (int i = 0; i < g_refusedDestCount; ++i)
        if (g_refusedDest[i] == dest) return true;
    if (g_refusedDestCount < 16) g_refusedDest[g_refusedDestCount++] = dest;
    return false;
}

// -- THE REST OF THE 2D SURFACE, SWEPT IN ONE GO --------------------------------------
//
// The HUD is visible in flat mode while drawDebugText, bitmapDraw and stampDraw are all
// silent through a whole race, so it is drawn by something else. Rather than decompile
// each remaining entry point and census them one build at a time, all of them go through
// one over-wide thunk.
//
// THIS IS SAFE BECAUSE THEY ARE ALL __cdecl. Declaring eight arguments where the real
// function takes four means a5..a8 read past the caller's arguments into its own frame --
// readable memory, garbage values, never a fault -- and when the thunk forwards, IT is
// the caller and cleans up its own eight. The callee reads the four it knows about and
// ignores the rest. Only the first arguments are ever printed, and only those are
// trustworthy.
//
// fillRect is the one whose shape is confirmed from roglv2 (0x20002E10):
// fillRect(destination, colour, x0, y0, x1, y1), with a1 the handle-table lookup exactly
// as in bitmapDraw and stampDraw. The rest are assumed to follow the same convention of
// destination-first, which the sweep will either bear out or not.
typedef int(__cdecl* WideFn)(int, int, int, int, int, int, int, int);

struct Census {
    const char* name;
    void**      slot;
    void*       original;
    int         count;
    int         high;
    int         reports;
};

Census g_census[] = {
    {"fillRect", nullptr, nullptr, 0, 0, 6},    {"bitmapScale", nullptr, nullptr, 0, 0, 6},
    {"drawLine", nullptr, nullptr, 0, 0, 6},    {"drawPixel", nullptr, nullptr, 0, 0, 6},
    {"aaLine", nullptr, nullptr, 0, 0, 6},      {"aaTriangle", nullptr, nullptr, 0, 0, 6},
};
constexpr int kCensusCount = sizeof(g_census) / sizeof(g_census[0]);
// fillRect's index in the table above. It gets its own column in the per-frame signature: a
// black letterbox bar is a filled rectangle, and the artifact being hunted is described as
// something drawn OVER a frame that was already complete.
constexpr int kCensusFillRect = 0;

int censusEnter(int which, int a1, int a2, int a3, int a4) {
    Census& c = g_census[which];
    ++c.count;
    // THE ORDERING CONTRACT THE FAST TEXT PATH GIVES UP IN VR. It normally holds all seven 2D
    // globals so that any other 2D call flushes its pending quads first, and a run of text
    // issued before a polygon is on the framebuffer before it. With stampDraw its only global,
    // these thunks are where that flush has to happen. Inert when nothing is batched.
    fastTextFlushNow();
    // Listed once per kind. The anti-aliased primitives take floats, so a3 and a4 are not a
    // position for all of them, and a replay draws aaLines at new values every frame.
    if (replayPanelCapturing())
        replayPanelNoteDraw(c.name, 0, 0, 0, 0, false, "a primitive, listed once per kind");
    if (c.reports > 0 && c.count <= 2) {
        --c.reports;
        void* dest = reinterpret_cast<void*>(static_cast<intptr_t>(a1));
        logf("hud: %s(%d, %d, %d, %d, ...) -- call %d this frame. If a1 is the "
             "destination it is %dx%d.",
             c.name, a1, a2, a3, a4, c.count, bitmapWidth(dest), bitmapHeight(dest));
    }
    return 0;
}

template <int N>
int __cdecl censusThunk(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8) {
    censusEnter(N, a1, a2, a3, a4);
    void* original = g_census[N].original;
    // A FILL IN A RACE IS ONE OF GPL'S DIALOGS -- F1's help is a 640x480 root over a translucent
    // box -- and the dialog goes to a panel of its own over the stereo picture, at GPL's own
    // pixels: its box, its frame art, its scroll arrows and its text together. Placed per eye,
    // the text was spread to stay legible and the box could not follow it.
    // fillRect(destination, colour, x, y, width, height) -- width and height, not a corner.
    if (N == kCensusFillRect && original) {
        int  originX = 0;
        int  originY = 0;
        int  width = 0;
        int  height = 0;
        bool framebuffer = false;
        if (fastTextSurfaceInfo(a1, originX, originY, width, height, framebuffer) &&
            framebuffer) {
            replayPanelNoteFill(originX + a3, originY + a4, a5, a6);
            if (replayPanelCapturing() && replayPanelTakes(originX + a3, originY + a4, a5, a6)) {
                fastTextOpenRasterBatch();
                ReplayPanelTarget target;
                return reinterpret_cast<WideFn>(original)(a1, a2, a3, a4, a5, a6, a7, a8);
            }
        }
    }
    return original ? reinterpret_cast<WideFn>(original)(a1, a2, a3, a4, a5, a6, a7, a8)
                    : 0;
}

void* censusThunkFor(int which) {
    switch (which) {
        case 0: return reinterpret_cast<void*>(&censusThunk<0>);
        case 1: return reinterpret_cast<void*>(&censusThunk<1>);
        case 2: return reinterpret_cast<void*>(&censusThunk<2>);
        case 3: return reinterpret_cast<void*>(&censusThunk<3>);
        case 4: return reinterpret_cast<void*>(&censusThunk<4>);
        default: return reinterpret_cast<void*>(&censusThunk<5>);
    }
}

int __cdecl bitmapDrawThunk(int source, int x, int y, int dest) {
    ++g_bitmapDrawCount;
    // Anything the fast text path still has batched goes down first -- see censusEnter.
    fastTextFlushNow();

    if (g_bitmapDrawReports > 0) {
        bool known = false;
        for (int i = 0; i < g_seenDestCount; ++i)
            if (g_seenDest[i] == dest) known = true;
        if (!known) {
            if (g_seenDestCount < 24) g_seenDest[g_seenDestCount++] = dest;
            --g_bitmapDrawReports;
            logf("hud: bitmapDraw -> destination %d, first seen at %d,%d (source %d). The "
                 "stereo source surface is %p and the eyes are %p/%p; a destination equal "
                 "to the stereo source is 2D landing on the whole side-by-side buffer, "
                 "which is one HUD across both eyes.",
                 dest, x, y, source, stereoSourceSurface(), stereoEyeSurface(0),
                 stereoEyeSurface(1));
        }
    }

    // gpl_sceneRender's mirror copy-out is bitmapDraw(mirror.mip's bitmap, 0, 0, mirror
    // camera surface): roglv2 binds the FIRST argument's texture and reads the FOURTH's
    // pixels, the reverse of the names above. Each mirror pane's image belongs in the region of
    // mirror.mip its glass's UVs read, so both coordinates are rewritten here. roglv2 copies the
    // read surface's own width and height, so exactly that rect is written.
    bool mirrorCopy = stereoMirrorCopy(source, dest, x, y);

    if (!g_bitmapDrawOriginal) return 0;

    // The copy binds mirror.mip behind roglv2's texture cache; the binding goes back after it.
    // The flush comes first so the binding noted is the one the cache last saw.
    if (mirrorCopy && g_options.mirrorCopyRebind) {
        rasterFlushPending();
        mirrorCacheCopyBegin();
        int result = g_bitmapDrawOriginal(source, x, y, dest);
        mirrorCacheCopyEnd();
        return result;
    }

    // GPL'S REPLAY BANDS GO TO THE REPLAY PANEL, at the coordinates GPL gave them. The button art
    // is drawn into the panel's layer and shown on a quad of its own; see ReplayPanel.h. Only a
    // destination roglv2 draws through the framebuffer -- the mirror copy-out writes a texture.
    // The batch is opened first, so the polygons GPL queued before this call are flushed into the
    // frame they belong to rather than into the layer.
    if (replayPanelCapturing()) {
        int   originX = 0;
        int   originY = 0;
        int   surfaceWidth = 0;
        int   surfaceHeight = 0;
        bool  framebuffer = false;
        void* art = reinterpret_cast<void*>(static_cast<intptr_t>(dest));
        bool  known = fastTextSurfaceInfo(source, originX, originY, surfaceWidth,
                                          surfaceHeight, framebuffer);
        int   artWidth = bitmapWidth(art);
        int   artHeight = bitmapHeight(art);
        bool  taken = known && framebuffer &&
                     replayPanelTakes(originX + x, originY + y, artWidth, artHeight);
        char  detail[96];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "destination %d, %s, %dx%d at %d,%d in its root; art %d", source,
                    !known ? "UNREADABLE" : (framebuffer ? "framebuffer" : "NOT the framebuffer"),
                    surfaceWidth, surfaceHeight, originX, originY, dest);
        replayPanelNoteDraw("bitmapDraw", originX + x, originY + y, artWidth, artHeight, taken,
                            detail);
        if (taken) {
            fastTextOpenRasterBatch();
            ReplayPanelTarget target;
            return g_bitmapDrawOriginal(source, x, y, dest);
        }
    }

    // GPL'S 2D ART, ONCE PER EYE.
    //
    // This path had no per-eye branch at all, which is why GPL's bitmap 2D stayed in window
    // coordinates even when the text was relocated: the replay control band is drawn here,
    // and its labels are drawn by stampDraw, so the two halves of one widget band went to
    // different places. The destination is an ARGUMENT, so the re-issue costs nothing but the
    // call.
    //
    // THE FRONT-END TEST COMES FIRST. The removed stampDraw thunk crashed the game three times
    // by measuring before deciding: stereoSourceSurface() caches the world surface handle,
    // leaving a race never clears it, and bitmapWidth on a freed handle is a null dereference
    // rather than a zero.
    // THE FIRST ARGUMENT IS THE DESTINATION, not the fourth.
    //
    // roglv2 binds a1's texture and reads a4's pixels, so a1 is where the pixels land -- which
    // is why stereoMirrorCopy above takes a1 as `writeBitmap` and rewrites it. The parameter
    // names here came from the C signature rather than from roglv2 and are backwards. Testing
    // a4 measured the surface being READ: for the mirror copy-out that is a 256x512 pane, which
    // is why this branch reported "not the engine's own frame" for every mirror copy and
    // declined the one case it exists for.
    //
    // Read after stereoMirrorCopy, which may have redirected it.
    void* destSurface = reinterpret_cast<void*>(static_cast<intptr_t>(source));
    int   fullWidth = 0;
    int   fullHeight = 0;
    int   rect[2][4];
    void* left = stereoEyeSurface(0);
    void* right = stereoEyeSurface(1);

    // EVERY TERM NAMES ITSELF. A split that declines without saying why reads exactly like one
    // that was never needed, and the replay control band spent a build looking like a placement
    // problem because this branch was silent.
    const char* refusal = nullptr;
    if (windowInFrontEnd())
        refusal = "the engine is in the front end";
    else if (!left || !right)
        refusal = "there are no eye surfaces";
    else if (!destinationCanSplit(destSurface, fullWidth, fullHeight))
        refusal = "the eyes are not carved out of this destination, so a per-eye re-issue has "
                  "nowhere to put anything -- either a different surface size from the stereo "
                  "source, or the window's back buffer";
    else if (!stereoEyeRect(0, rect[0]) || !stereoEyeRect(1, rect[1]))
        refusal = "there are no eye rects this frame";

    if (refusal) {
        if (g_bitmapRefuseReports > 0 && !knownRefusal(dest)) {
            --g_bitmapRefuseReports;
            int modeWidth = 0;
            int modeHeight = 0;
            windowEngineModeSize(modeWidth, modeHeight);
            logf("hud: bitmapDraw to destination %d (%dx%d) was NOT split -- %s. The engine's "
                 "mode is %dx%d and the eyes are %p/%p. A destination that is the engine's own "
                 "frame is what gets split.",
                 dest, fullWidth, fullHeight, refusal, modeWidth, modeHeight, left, right);
        }
        return g_bitmapDrawOriginal(source, x, y, dest);
    }

    int eyeWidth = rect[0][2];
    int eyeHeight = rect[0][3];

    int outX = remapAnchor(x, fullWidth);
    int outY = y;
    scaleToEye(outX, outY, fullWidth, fullHeight, eyeWidth, eyeHeight);

    int disparity = disparityPixels(eyeWidth);
    int half = disparity / 2;
    if (eyeWidth > 0) {
        if (outX - half < 0) half = outX;
        if (outX + half > eyeWidth - 1) half = eyeWidth - 1 - outX;
        if (half < 0) half = 0;
    }

    ++g_bitmapSplitCount;
    if (g_bitmapSplitReports > 0) {
        --g_bitmapSplitReports;
        logf("hud: bitmapDraw split once per eye -- source %d at (%d,%d) on a %dx%d engine "
             "frame becomes (%d,%d) in a %dx%d eye, separated by %d px. Split %d of the %d "
             "bitmapDraw calls so far.",
             source, x, y, fullWidth, fullHeight, outX, outY, eyeWidth, eyeHeight, half * 2,
             g_bitmapSplitCount, g_bitmapDrawCount);
    }

    // The EYE goes in the first argument, which is the one the pixels land in, and `dest` keeps
    // the surface being read. Swapping these drew each eye's image into the art instead of the
    // art into each eye.
    g_bitmapDrawOriginal(static_cast<int>(reinterpret_cast<intptr_t>(left)), outX + half, outY,
                         dest);
    return g_bitmapDrawOriginal(static_cast<int>(reinterpret_cast<intptr_t>(right)),
                                outX - half, outY, dest);
}

void __cdecl drawDebugTextThunk(void* surface, unsigned colour, int x, int y,
                                const char* text, int background) {
    frameBudgetHudText();
    // Anything the fast text path still has batched goes down first -- see censusEnter.
    fastTextFlushNow();
    Call call;
    call.surface = surface;
    call.colour = colour;
    call.x = x;
    call.y = y;
    call.background = background;

    // COPIED, not pointed at. GPL formats most of these into a stack buffer that is gone
    // by the time an extra render asks for the frame again.
    call.text[0] = 0;
    if (text) {
        size_t length = strnlen(text, kMaxText - 1);
        memcpy(call.text, text, length);
        call.text[length] = 0;
    }

    // An empty string is rasterFlushPending's own call, not GPL's.
    if (call.text[0] && replayPanelCapturing()) {
        char detail[96];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, "\"%s\" on a %dx%d surface", call.text,
                    bitmapWidth(surface), bitmapHeight(surface));
        replayPanelNoteDraw("drawDebugText", x, y, 0, 0, false, detail);
    }

    if (!g_replaying) {
        if (g_callCount < kMaxCalls) {
            g_calls[g_callCount++] = call;
        } else if (g_overflowReports > 0) {
            --g_overflowReports;
            logf("hud: more than %d drawDebugText calls in one frame -- the extras are "
                 "drawn but not recorded, so an extra render will not put them back",
                 kMaxCalls);
        }
    }

    emit(call);
}

}  // namespace

void hudBind(const Binding& binding) {
    g_bitmapWidthSlot = binding.raster.bitmapWidth;
    g_bitmapHeightSlot = binding.raster.bitmapHeight;
    g_backBufferSlot = binding.raster.screenGetBackBuffer;

    g_slot = rasterGlobal(binding.image, "drawDebugText");
    if (!g_slot || !*g_slot) {
        logf("hud: drawDebugText was not resolved -- the in-race HUD stays as GPL draws "
             "it, which in stereo is half a HUD in each eye");
        return;
    }
    if (!g_bitmapWidthSlot || !*g_bitmapWidthSlot) {
        logf("hud: bitmapWidth was not resolved -- without it there is no way to tell a "
             "left-anchored item from a right-anchored one, so the HUD stays as GPL "
             "draws it");
        return;
    }

    g_original = reinterpret_cast<DrawDebugTextFn>(*g_slot);
    void* hook = reinterpret_cast<void*>(&drawDebugTextThunk);
    if (!patchBytes(g_slot, &hook, sizeof(hook))) {
        g_original = nullptr;
        logf("hud: the drawDebugText global could not be written -- the HUD stays as GPL "
             "draws it");
        return;
    }

    // The GLOBAL'S ADDRESS, because a patch that lands on the wrong slot and a patch
    // that lands on the right one look identical from everywhere else. gplc67 calls
    // `call ds:gpl_rasterDrawDebugText` and gpl_rasterResolveEntryPoints fills that from
    // GetProcAddress at 0x004E2338, which writes kDebugTextSlot. Anything else here means we
    // are holding a slot nothing calls.
    // The 2D census rides alongside. It is a counter, not a behaviour change, and it
    // fails quietly: if the global cannot be found or written, the HUD work above is
    // unaffected.
    g_bitmapDrawSlot = rasterGlobal(binding.image, "bitmapDraw");
    if (g_bitmapDrawSlot && *g_bitmapDrawSlot) {
        g_bitmapDrawOriginal = reinterpret_cast<BitmapDrawFn>(*g_bitmapDrawSlot);
        void* census = reinterpret_cast<void*>(&bitmapDrawThunk);
        if (patchBytes(g_bitmapDrawSlot, &census, sizeof(census))) {
            logf("hud: bitmapDraw census installed at global %p -- destinations will be "
                 "named once each, to settle whether the in-race HUD is bitmaps rather "
                 "than text.",
                 reinterpret_cast<void*>(g_bitmapDrawSlot));
        } else {
            g_bitmapDrawOriginal = nullptr;
        }
    }

    for (int i = 0; i < kCensusCount; ++i) {
        g_census[i].slot = rasterGlobal(binding.image, g_census[i].name);
        if (!g_census[i].slot || !*g_census[i].slot) continue;
        g_census[i].original = *g_census[i].slot;
        void* thunk = censusThunkFor(i);
        if (!patchBytes(g_census[i].slot, &thunk, sizeof(thunk)))
            g_census[i].original = nullptr;
    }
    logf("hud: the rest of the 2D surface is censused too -- fillRect, bitmapScale, "
         "drawLine, drawPixel, aaLine, aaTriangle. The HUD is visible in flat mode while "
         "drawDebugText, bitmapDraw and stampDraw are silent for a whole race, so it is "
         "one of these or it is 3D geometry.");

    logf("hud: stampDraw is NOT thunked here -- the fast text path owns that global and draws "
         "GPL's glyphs from its own atlas, once per eye, where vrGplTextScale and "
         "vrGplTextInset put them; the `fast text:` lines carry its census. The other thunks "
         "here flush that path's pending batch before they draw, which is the ordering the six "
         "globals it gives up were holding.");

    // The expected slot goes through rebase, because the slot itself was resolved by entry-point
    // name and will legitimately differ from the reference link's address on an image whose data
    // moved. Comparing against the raw constant made this line cry wolf on every such image.
    uintptr_t expected = rebase(kDebugTextSlot);
    logf("hud: drawDebugText thunked at global %p (held %p) -- this image's own slot is %08X; if "
         "this says otherwise the thunk is on a copy nobody calls. The in-race HUD is re-issued "
         "once per eye surface, with the anchor class remapped to the eye's own width. Items on "
         "any other destination, the frame-time readout included, are untouched.",
         reinterpret_cast<void*>(g_slot), reinterpret_cast<void*>(g_original),
         static_cast<unsigned>(expected));
}

// THE THUNK DOES NOT STAY INSTALLED, AND THAT IS WHY THE HUD WAS NEVER PER-EYE.
//
// gpl_rasterResolveEntryPoints (0x004E21B0) fills every rasterizer global from
// GetProcAddress, `drawDebugText` among them at 0x004E2338, and it runs again on a mode
// change -- which is exactly what entering a race is. So the thunk survives the front end,
// catches the menu's version string, and is then overwritten by the genuine entry point
// the moment the world mode comes up. Everything after that draws unremapped, which is
// the HUD spanning both eye halves as though there were one fullscreen view.
//
// Stereo already holds its entries AS GLOBALS and reads the slot on every call for this
// reason. The same discipline, expressed as a re-assert: whoever last wrote the slot, we
// write it again, and we adopt whatever they put there as the new original.
// Put one thunk back if the globals were re-resolved over it, adopting whatever is there
// now as the new original. Reports the first few times, per entry point.
void reassert(const char* name, void** slot, void* thunk, void** originalStorage,
              int& reports) {
    if (!slot || !*slot) return;  // between the clear and the GetProcAddress; try again
    if (*slot == thunk) return;

    *originalStorage = *slot;
    if (!patchBytes(slot, &thunk, sizeof(thunk))) return;

    if (reports > 0) {
        --reports;
        logf("hud: the %s global had been re-resolved to %p and the thunk was gone -- "
             "re-asserted, and that pointer is the new original.",
             name, *originalStorage);
    }
}

// EVERY THUNK, NOT JUST THE FIRST ONE I LEARNED THIS ABOUT.
//
// gpl_rasterResolveEntryPoints re-fills the rasterizer globals on every mode change, so
// anything patched into one is silently gone the moment a race starts. That was found and
// fixed for drawDebugText -- and then stampDraw, bitmapDraw and six census entries were
// added afterwards with a one-shot install apiece, which is the same bug reintroduced
// eight times over.
//
// It is also the best explanation for stampDraw appearing at 56 calls a frame in one race
// and not at all in the next three: the thunk was alive or dead depending on whether a
// re-resolve had happened since it was installed, and the count was measuring the hook
// rather than the game.
void hudEnsureThunk() {
    static int textReports = 4;
    static int bitmapReports = 4;
    static int censusReports = 6;

    if (g_slot && g_original) {
        reassert("drawDebugText", g_slot, reinterpret_cast<void*>(&drawDebugTextThunk),
                 reinterpret_cast<void**>(&g_original), textReports);
    }
    if (g_bitmapDrawSlot && g_bitmapDrawOriginal) {
        reassert("bitmapDraw", g_bitmapDrawSlot, reinterpret_cast<void*>(&bitmapDrawThunk),
                 reinterpret_cast<void**>(&g_bitmapDrawOriginal), bitmapReports);
    }
    for (int i = 0; i < kCensusCount; ++i) {
        if (!g_census[i].slot || !g_census[i].original) continue;
        reassert(g_census[i].name, g_census[i].slot, censusThunkFor(i),
                 &g_census[i].original, censusReports);
    }
}

void hudBeginFrame() {
    bool inFrontEnd = windowInFrontEnd();
    if (inFrontEnd != g_wasFrontEnd) {
        g_wasFrontEnd = inFrontEnd;
        g_seenDestCount = 0;
        g_bitmapDrawReports = 12;
        g_bitmapDrawHighWater = 0;
        g_bitmapSplitCount = 0;
        g_bitmapSplitReports = 4;
        g_refusedDestCount = 0;
        g_bitmapRefuseReports = 8;
        for (int i = 0; i < kCensusCount; ++i) {
            g_census[i].reports = 6;
            g_census[i].high = 0;
        }
        g_lastReportedCount = 0;
        g_countReports = 8;
        g_seenCount = 0;
        logf("hud: census reset -- the engine has entered %s, and the counts below are "
             "that phase's own.",
             inFrontEnd ? "the front end" : "a world mode");
    }

    for (int i = 0; i < kCensusCount; ++i) {
        Census& c = g_census[i];
        if (c.count > c.high && c.reports > 0) {
            c.high = c.count;
            logf("hud: up to %d %s call(s) in one frame now.", c.high, c.name);
        }
        c.count = 0;
    }

    if (g_bitmapDrawCount > g_bitmapDrawHighWater && g_bitmapDrawReports > 0) {
        g_bitmapDrawHighWater = g_bitmapDrawCount;
        logf("hud: up to %d bitmapDraw call(s) in one frame now, across %d distinct "
             "destination(s).",
             g_bitmapDrawHighWater, g_seenDestCount);
    }
    g_bitmapDrawCount = 0;

    // A NEW HIGH-WATER MARK, not every change. The front end alternates between zero and
    // one call as the version string comes and goes, and reporting each flip would spend
    // the whole budget in the menu and have nothing left for the race -- which is the
    // failure the surface reporting already made once.
    if (g_callCount > g_lastReportedCount && g_countReports > 0) {
        --g_countReports;
        g_lastReportedCount = g_callCount;
        logf("hud: up to %d drawDebugText call(s) in one frame now. The in-race block in "
             "gpl_simFrameRender is about twenty; if that number never appears while "
             "driving then gpl_optShowTimes is closed or the HUD is not drawn with this "
             "entry point at all.",
             g_callCount);
    }
    g_callCount = 0;
}

void hudReplay() {
    if (!g_original || g_callCount == 0) return;

    g_replaying = true;
    for (int i = 0; i < g_callCount; ++i) emit(g_calls[i]);
    g_replaying = false;
}

bool hudBitmapDrawHooked() {
    return g_bitmapDrawSlot && g_bitmapDrawOriginal &&
           *g_bitmapDrawSlot == reinterpret_cast<void*>(&bitmapDrawThunk);
}

}  // namespace gplvr
