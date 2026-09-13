#include "Hud.h"

#include "FrameBudget.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"
#include "RenderTarget.h"
#include "Stereo.h"
#include "Window.h"

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
    if (g_seenCount < 8) g_seenSurfaces[g_seenCount++] = surface;
    return true;
}

// What the last frame actually asked for, reported when the number changes. This is the
// one line that says whether the HUD block in gpl_simFrameRender is running at all:
// zero means gpl_optShowTimes is closed or the HUD is drawn by something else entirely,
// and twenty-odd means it is reaching us and the question is only where it lands.
int g_lastReportedCount = 0;
int g_countReports = 8;

// A NULL SURFACE IS NOT A QUESTION THE RASTERIZER CAN BE ASKED.
//
// roglv2 resolves a handle as `*(handleTable + 2) + 8 * handle` and dereferences what it
// finds, with no check of its own. Handle 0 walks into a null vtable and takes the process
// with it. That is exactly what happened when the census asked for the size of
// stereoSourceSurface() while still in the front end, where there are no eyes and the
// source is null -- an instrument that crashed the thing it was measuring.
int bitmapWidth(void* bitmap) {
    if (!bitmap) return 0;
    if (!g_bitmapWidthSlot || !*g_bitmapWidthSlot) return 0;
    return reinterpret_cast<BitmapWidthFn>(*g_bitmapWidthSlot)(bitmap);
}

int bitmapHeight(void* bitmap) {
    if (!bitmap) return 0;
    if (!g_bitmapHeightSlot || !*g_bitmapHeightSlot) return 0;
    return reinterpret_cast<BitmapWidthFn>(*g_bitmapHeightSlot)(bitmap);
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
int remapAnchor(int x, int fullWidth) {
    if (x >= 3 * fullWidth / 4) return x - fullWidth / 2;
    if (x >= fullWidth / 4) return x - fullWidth / 4;
    return x;
}

// HOW FAR IN FROM THE EYE'S EDGES THE HUD IS PULLED, as a fraction of the half-extent.
//
// GPL lays its HUD out for a 4:3 monitor, where a corner is a glance away. An eye at 104
// degrees is not a monitor: x = 5 of 1280 is not "the left edge", it is over your
// shoulder, and the bottom-left corner of the frame is somewhere you cannot comfortably
// look. The pixels were in the published frame all along -- measured, not assumed -- and
// still read as missing.
//
// So the layout is scaled about the eye's centre, keeping every relative position and
// bringing the whole thing into the cone a seated driver actually uses. 1.0 is the raw
// layout and the corners it implies; 0.6 puts a corner item at 20% in from the edge.
constexpr float kHudInset = 0.0f;

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

    int rect[2][4];
    void* left = stereoEyeSurface(0);
    void* right = stereoEyeSurface(1);

    // Everything that is not the in-race HUD on the stereo surface goes through
    // untouched. The frame-time readout is the one to keep in mind: its destination is
    // screenGetBackBuffer(), not the camera's surface, so it fails this test and stays
    // where GPL put it.
    if (!left || !right || call.surface != stereoSourceSurface() ||
        !stereoEyeRect(0, rect[0]) || !stereoEyeRect(1, rect[1])) {
        // Say so, once per destination. The pass-through is correct for the frame-time
        // readout and wrong for the in-race HUD, and from the outside the two look
        // identical: a HUD that never reached an eye and a HUD that was never meant to.
        // Naming the surface is what separates them without another guess.
        if (firstSightOf(call.surface)) {
            logf("hud: \"%s\" went to surface %p UNCHANGED -- the stereo source is %p, "
                 "eyes %p/%p. Expected for the frame-time readout; for an in-race item "
                 "it means the HUD and the eyes are drawn into different surfaces.",
                 call.text, call.surface, stereoSourceSurface(), left, right);
        }
        g_original(call.surface, call.colour, call.x, call.y, call.text, call.background);
        return;
    }

    int fullWidth = bitmapWidth(call.surface);
    if (fullWidth <= 0) {
        g_original(call.surface, call.colour, call.x, call.y, call.text, call.background);
        return;
    }

    int x = remapAnchor(call.x, fullWidth);
    // One eye has nothing to fuse, so it gets no disparity.
    int disparity = g_options.vrHudEye ? 0 : disparityPixels(rect[0][2]);
    if (disparity != g_lastDisparity && g_reports > 0) {
        --g_reports;
        g_lastDisparity = disparity;
        logf("hud: drawn once per eye -- %d px of convergent disparity for vrHudDepth = "
             "%.2f, eye %dx%d out of a %d-wide surface%s",
             disparity, g_options.vrHudDepth, rect[0][2], rect[0][3], fullWidth,
             disparity ? "" : " (at infinity)");
    }

    // y needs no remap: each eye is the full height of the buffer, and a sub-rect
    // instance already offsets into the root by its own origin.
    if (g_options.vrHudEye != 2)
        g_original(left, call.colour, x + disparity / 2, call.y, call.text, call.background);
    if (g_options.vrHudEye != 1)
        g_original(right, call.colour, x - disparity / 2, call.y, call.text, call.background);
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

int censusEnter(int which, int a1, int a2, int a3, int a4) {
    Census& c = g_census[which];
    ++c.count;
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

// stampDraw(0x20004900) is five __cdecl arguments ending in rogl_fontBlitGlyph(a5, a2, ?,
// ?, a3) -- a glyph blitter, which is what a gauge and a row of digits are made of. Only
// a1 is looked up in the handle table, and there is no second handle argument, so unlike
// bitmapDraw this one may have no explicit destination at all and simply draw wherever
// polySetDest last pointed. That would put it on the game's main camera surface -- the
// whole side-by-side buffer -- which is exactly the "layered on top of both eyes" the
// user saw at vrOffscreen = 0, and exactly the "invisible" at 6344 wide.
//
// So the census logs ALL FIVE arguments and names none of them. One race says which is
// which: the constant one is the stamp set, the pair that sweeps is the position.
typedef void(__cdecl* StampDrawFn)(int a1, int a2, int a3, int a4, int a5);

void**      g_stampDrawSlot = nullptr;
StampDrawFn g_stampDrawOriginal = nullptr;
int         g_stampReports = 12;
int         g_stampPerEyeReports = 4;
int         g_lastStampDisparity = -9999;
int         g_stampCount = 0;
int         g_stampHighWater = 0;

// -- STAMPED TEXT: WHAT IT SAYS, AND WHAT CAN BE DROPPED -------------------------------
//
// The arguments, read from both sides on 2026-09-14:
//
//   roglv2 stampDraw (0x20004900): a1 is looked up in the handle table as the destination,
//   a4 is resolved by sub_20018AC0 into a font object, and rogl_fontBlitGlyph draws glyph
//   a5 of that font at a2, a3.
//
//   gplc67 reads the global at 0x00867628 in two places. sub_4EDE40 draws a string (the
//   call at 0x004EDF5E) and the routine just before it draws one glyph (0x004EDE2A); both
//   pass (surface, x, y, font->+33, glyph). The glyph comes from sub_4EDC80: the character
//   looked up in the font's own map, falling back to the font's default glyph. For a font
//   in mode 2 or 4 (font->+22) the string routine adds a quarter-pixel phase of 0..3 to the
//   glyph and advances x in quarter pixels.
//
// The census values -- 48, 82, 69 -- are '0', 'R', 'E', so these fonts map characters to
// their ASCII codes. Text is printed as characters, and any code outside printable ASCII
// is escaped with its raw value.

// Surfaces a bitmapDraw has touched since the last mode change, as either argument. Pixels
// are copied out of those, so a stamp on one can reach the frame another way.
constexpr int kMaxComposited = 32;
int  g_composited[kMaxComposited] = {};
int  g_compositedCount = 0;
bool g_compositedOverflow = false;

void noteComposited(int handle) {
    for (int i = 0; i < g_compositedCount; ++i)
        if (g_composited[i] == handle) return;
    if (g_compositedCount < kMaxComposited)
        g_composited[g_compositedCount++] = handle;
    else
        g_compositedOverflow = true;
}

// A full table can no longer vouch for any surface, so every surface counts as touched.
bool wasComposited(int handle) {
    if (g_compositedOverflow) return true;
    for (int i = 0; i < g_compositedCount; ++i)
        if (g_composited[i] == handle) return true;
    return false;
}

// Whether a stamp that did not qualify for the per-eye split can be dropped: the rule
// documented at vrSkipStampText in Hooks.h. A destination no wider than the display and
// at least 7/8 as wide is a screen-space layer of it; the off-screen frame is wider.
bool stampUnseen(int handle, void* dest, int width, int height, void* left, void* right,
                 void* source) {
    if (!renderTargetActive() || !left || !right || !source) return false;
    if (dest == left || dest == right || dest == source) return false;
    int nativeW = 0;
    int nativeH = 0;
    if (!renderTargetNativeSize(nativeW, nativeH)) return false;
    if (width * 8 < nativeW * 7 || width > nativeW) return false;
    if (height <= 0 || height > nativeH) return false;
    return !wasComposited(handle);
}

// One frame's stamps, grouped into strings in call order: a glyph joins the string before
// it when it has the same destination, set and y, and sits a little to the right.
constexpr int kMaxFrameStrings = 256;
constexpr int kMaxStampChars = 80;
constexpr int kMaxGlyphGap = 64;

struct StampString {
    int            dest;
    int            width;
    int            height;
    int            set;
    int            x;
    int            y;
    int            lastX;
    int            length;
    bool           truncated;
    unsigned short codes[kMaxStampChars];
};

StampString g_frameStrings[kMaxFrameStrings];
int         g_frameStringCount = 0;
int         g_frameStampCalls = 0;
int         g_frameUngrouped = 0;
int         g_frameDropped = 0;

void recordStamp(int dest, int width, int height, int x, int y, int set, int glyph) {
    unsigned short code =
        (glyph >= 0 && glyph < 0xFFFF) ? static_cast<unsigned short>(glyph) : 0xFFFF;
    if (g_frameStringCount > 0) {
        StampString& s = g_frameStrings[g_frameStringCount - 1];
        if (s.dest == dest && s.set == set && s.y == y && x > s.lastX &&
            x - s.lastX <= kMaxGlyphGap) {
            if (s.length < kMaxStampChars)
                s.codes[s.length++] = code;
            else
                s.truncated = true;
            s.lastX = x;
            return;
        }
    }
    if (g_frameStringCount >= kMaxFrameStrings) {
        ++g_frameUngrouped;
        return;
    }
    StampString& s = g_frameStrings[g_frameStringCount++];
    s.dest = dest;
    s.width = width;
    s.height = height;
    s.set = set;
    s.x = x;
    s.y = y;
    s.lastX = x;
    s.length = 1;
    s.truncated = false;
    s.codes[0] = code;
}

// Every string seen since the last mode change, by where it is drawn. A string is logged
// when it first appears and when its text changes, at most once a second each, and no
// more than kMaxTextLinesPerSecond lines a second across all of them.
constexpr int kMaxKnownStrings = 512;
constexpr int kMaxTextLinesPerSecond = 40;

struct KnownString {
    int      dest;
    int      set;
    int      x;
    int      y;
    unsigned loggedHash;
    DWORD    loggedAt;
    bool     logged;
    bool     seen;     // during the current summary second
    bool     waiting;  // its text changed and the rate limit has held the line back
};

KnownString g_known[kMaxKnownStrings];
int         g_knownCount = 0;
int         g_knownHint = 0;

struct StampWindow {
    DWORD     start = 0;
    int       frames = 0;
    long long calls = 0;
    int       maxCalls = 0;
    long long dropped = 0;
    int       lines = 0;
    int       untracked = 0;
    int       ungrouped = 0;
};

StampWindow g_stampWindow;
bool        g_loggedStampDrop = false;

unsigned hashStampString(const StampString& s) {
    unsigned hash = 2166136261u;
    for (int i = 0; i < s.length; ++i) {
        hash = (hash ^ (s.codes[i] & 0xFF)) * 16777619u;
        hash = (hash ^ (s.codes[i] >> 8)) * 16777619u;
    }
    return (hash ^ static_cast<unsigned>(s.length) ^ (s.truncated ? 0x80000000u : 0u)) *
           16777619u;
}

// Printable ASCII as itself; a quote or backslash escaped; anything else as \xNN or
// \uNNNN carrying the raw code. True when any code fell outside printable ASCII.
bool formatStampText(const StampString& s, char* out, size_t size) {
    size_t used = 0;
    bool   unusual = false;
    for (int i = 0; i < s.length && used + 8 < size; ++i) {
        unsigned code = s.codes[i];
        if (code == '"' || code == '\\') {
            out[used++] = '\\';
            out[used++] = static_cast<char>(code);
        } else if (code >= 32 && code < 127) {
            out[used++] = static_cast<char>(code);
        } else {
            unusual = true;
            int n = snprintf(out + used, size - used, code < 256 ? "\\x%02X" : "\\u%04X", code);
            if (n > 0) used += static_cast<size_t>(n);
        }
    }
    if (s.truncated && used + 4 < size) {
        memcpy(out + used, "...", 3);
        used += 3;
    }
    out[used] = 0;
    return unusual;
}

KnownString* findKnown(const StampString& s) {
    if (g_knownHint < g_knownCount) {
        KnownString& k = g_known[g_knownHint];
        if (k.dest == s.dest && k.set == s.set && k.x == s.x && k.y == s.y) {
            ++g_knownHint;
            return &k;
        }
    }
    for (int i = 0; i < g_knownCount; ++i) {
        KnownString& k = g_known[i];
        if (k.dest == s.dest && k.set == s.set && k.x == s.x && k.y == s.y) {
            g_knownHint = i + 1;
            return &k;
        }
    }
    return nullptr;
}

void flushStampStrings(DWORD now) {
    g_knownHint = 0;
    for (int i = 0; i < g_frameStringCount; ++i) {
        const StampString& s = g_frameStrings[i];
        unsigned hash = hashStampString(s);

        KnownString* k = findKnown(s);
        if (!k) {
            if (g_knownCount >= kMaxKnownStrings) {
                ++g_stampWindow.untracked;
                continue;
            }
            k = &g_known[g_knownCount++];
            *k = KnownString{};
            k->dest = s.dest;
            k->set = s.set;
            k->x = s.x;
            k->y = s.y;
            g_knownHint = g_knownCount;
        }
        k->seen = true;
        if (k->logged && k->loggedHash == hash) {
            k->waiting = false;
            continue;
        }
        if ((k->logged && now - k->loggedAt < 1000) ||
            g_stampWindow.lines >= kMaxTextLinesPerSecond) {
            k->waiting = true;
            continue;
        }

        char text[512];
        bool unusual = formatStampText(s, text, sizeof(text));
        char codes[256];
        codes[0] = 0;
        if (unusual) {
            size_t used = 0;
            for (int c = 0; c < s.length && used + 8 < sizeof(codes); ++c) {
                int n = snprintf(codes + used, sizeof(codes) - used, " %u", s.codes[c]);
                if (n > 0) used += static_cast<size_t>(n);
            }
        }
        logf("hud: stamp text %s on surface %d (%dx%d), set %d, at %d,%d: \"%s\"%s%s",
             k->logged ? "CHANGED" : "new", s.dest, s.width, s.height, s.set, s.x, s.y,
             text, unusual ? " -- raw codes:" : "", codes);
        k->logged = true;
        k->loggedHash = hash;
        k->loggedAt = now;
        k->waiting = false;
        ++g_stampWindow.lines;
    }
}

void resetStampTracking() {
    g_compositedCount = 0;
    g_compositedOverflow = false;
    g_knownCount = 0;
    g_knownHint = 0;
    g_loggedStampDrop = false;
}

// THE IN-RACE HUD, ONCE PER EYE.
//
// Established 2026-09-12 after four wrong answers: `stampDraw(109, 2, 1309, 11, 94)` with
// destination 109 measuring 2560x1440 -- the WHOLE render surface -- at 56 calls a frame
// through a race. Under stereo that surface is two 1280x1440 eyes, so a single pass at
// x = 2 puts the entire HUD in the left eye's bottom-left corner and nothing in the right.
// At 104 degrees per eye that corner is far enough into the periphery to read as missing,
// which is exactly what it looked like.
//
// Same treatment as the text path and the same two reasons it works: the destination is
// an argument, so re-issuing costs nothing but the call; and the anchor class is
// recoverable from x against the full width, so an item pinned to an edge stays pinned to
// that edge in an eye half as wide. y needs no remap -- each eye is the full height of the
// buffer, and a sub-rect instance already offsets into the root by its own origin.
void __cdecl stampDrawThunk(int a1, int a2, int a3, int a4, int a5) {
    ++g_stampCount;

    // NOTHING IS MEASURED BEFORE IT IS DECIDED WHETHER THE MEASUREMENT IS MEANINGFUL.
    //
    // This crashed the game three times, and the dumps are unambiguous: a read of
    // 0x0000000C inside roglv2!bitmapWidth, called from here. `stereoSourceSurface()`
    // caches the world surface handle 0x6F, `stereoRelease()` is only reachable from a
    // world render, so LEAVING A RACE NEVER CLEARS IT -- and the front end goes on
    // drawing stamps against a handle whose table slot has been freed. `bitmapWidth`
    // takes a HANDLE and does `table[handle]` then `[+0x0C]`, so a freed slot is a null
    // dereference. The `if (!bitmap)` guard added earlier catches handle 0; it cannot
    // catch a freed non-zero handle.
    //
    // The front-end test was already in the condition below and would have short-circuited
    // every one of those crashes. It was simply read four lines too late, because the
    // measurement was hoisted above the decision.
    if (windowInFrontEnd() || !g_stampDrawOriginal) {
        if (g_stampDrawOriginal) g_stampDrawOriginal(a1, a2, a3, a4, a5);
        return;
    }

    if (g_stampReports > 0 && g_stampCount <= 3) {
        --g_stampReports;
        // a1 holds through a whole race while a4 takes small changing values, so a1 is
        // the destination surface and a4 the stamp set. Its SIZE is the thing that
        // settles what it is: the back buffer, the driving view carved out of it, or
        // something else again -- and that decides whether the HUD can be re-issued per
        // eye the way the text path is.
        void* dest = reinterpret_cast<void*>(static_cast<intptr_t>(a1));
        logf("hud: stampDraw(%d, %d, %d, %d, %d) -- call %d this frame. Destination %d is "
             "%dx%d; the stereo source %p is %dx%d and the eyes are %p/%p.",
             a1, a2, a3, a4, a5, g_stampCount, a1, bitmapWidth(dest), bitmapHeight(dest),
             stereoSourceSurface(), bitmapWidth(stereoSourceSurface()),
             bitmapHeight(stereoSourceSurface()), stereoEyeSurface(0),
             stereoEyeSurface(1));
    }

    int rect[2][4];
    void* dest = reinterpret_cast<void*>(static_cast<intptr_t>(a1));
    void* left = stereoEyeSurface(0);
    void* right = stereoEyeSurface(1);

    // THE HUD'S DESTINATION IS NOT THE SURFACE STEREO SPLIT, AND IT NEVER WAS.
    //
    // It is 109 where the stereo source is 111 -- both 2560x1440, different objects. The
    // log said so the first time the destination was ever printed, and the identity test
    // written afterwards ignored it, so every stamp took the pass-through branch and the
    // per-eye re-issue never ran once.
    //
    // Both are views onto the same framebuffer, so what actually matters is whether this
    // destination spans the WHOLE side-by-side buffer rather than whether it is the same
    // handle the eyes were carved from. Size answers that, and size is also immune to the
    // handle recycling that caught this function out a build ago: 0x6F was the render
    // surface during a race and a 38x22 scrap in the menu afterwards.
    int fullWidth = bitmapWidth(dest);
    int fullHeight = bitmapHeight(dest);
    void* source = stereoSourceSurface();
    bool spansTheBuffer = fullWidth > 0 && source && fullWidth == bitmapWidth(source) &&
                          fullHeight == bitmapHeight(source);

    // One record per call GPL makes, before any re-issue; flushed by hudEndFrame.
    ++g_frameStampCalls;
    if (!g_options.vrSkipStampText) recordStamp(a1, fullWidth, fullHeight, a2, a3, a4, a5);

    if (!left || !right || !spansTheBuffer || !stereoEyeRect(0, rect[0]) ||
        !stereoEyeRect(1, rect[1])) {
        if (g_options.vrSkipStampText &&
            stampUnseen(a1, dest, fullWidth, fullHeight, left, right, source)) {
            ++g_frameDropped;
            if (!g_loggedStampDrop) {
                g_loggedStampDrop = true;
                logf("hud: vrSkipStampText -- stampDraw to surface %d (%dx%d) is being "
                     "dropped, with every stamp like it: not an eye, not the whole "
                     "side-by-side buffer, a screen-wide layer of the display, and never "
                     "touched by a bitmapDraw this phase. Counted once a second.",
                     a1, fullWidth, fullHeight);
            }
            return;
        }
        // Once per distinct destination, so a pass-through that should have been a split
        // says so instead of being silent. Five rounds of this were lost to a branch that
        // declined without comment.
        if (firstSightOf(dest)) {
            logf("hud: stampDraw to surface %d (%dx%d) passed through UNCHANGED -- the "
                 "stereo source is %p (%dx%d), eyes %p/%p. A destination that spans the "
                 "whole buffer is what gets split; this one did not qualify.",
                 a1, fullWidth, fullHeight, source, bitmapWidth(source),
                 bitmapHeight(source), left, right);
        }
        g_stampDrawOriginal(a1, a2, a3, a4, a5);
        return;
    }

    int eyeWidth = rect[0][2];
    int eyeHeight = rect[0][3];

    int x = remapAnchor(a2, fullWidth);
    int y = a3;

    // Toward the middle of the eye, both axes, about its centre.
    if (eyeWidth > 0 && eyeHeight > 0) {
        float halfX = static_cast<float>(eyeWidth) * 0.5f;
        float halfY = static_cast<float>(eyeHeight) * 0.5f;
        x = static_cast<int>(halfX + (static_cast<float>(x) - halfX) * kHudInset);
        y = static_cast<int>(halfY + (static_cast<float>(y) - halfY) * kHudInset);
    }

    int disparity = g_options.vrHudEye ? 0 : disparityPixels(eyeWidth);

    // A LEFT-ANCHORED ITEM MINUS HALF THE DISPARITY IS OFF THE EDGE.
    //
    // x = 5 with 16 px of disparity sent the right eye's copy to -3, which roglv2 clipped
    // away -- so the split ran, reported itself as working, and put exactly one copy on
    // screen. The monitor showed "the HUD as always" and the headset showed it in one eye
    // at the far corner, which is indistinguishable from no HUD at all. The inset above
    // makes this nearly impossible, but nearly is not a guarantee when the anchor classes
    // and the depth are both tunable.
    int half = disparity / 2;
    if (eyeWidth > 0) {
        if (x - half < 0) half = x;
        if (x + half > eyeWidth - 1) half = eyeWidth - 1 - x;
        if (half < 0) half = 0;
    }
    if (disparity != g_lastStampDisparity && g_stampPerEyeReports > 0) {
        --g_stampPerEyeReports;
        g_lastStampDisparity = disparity;
        logf("hud: the in-race HUD is drawn once per eye -- (%d,%d) on a %d-wide surface "
             "becomes (%d,%d) in a %dx%d eye after the anchor remap and a %.2f inset, "
             "separated by %d px (clamped from %d) for vrHudDepth = %.2f. The headset's "
             "copy of this frame was %s.",
             a2, a3, fullWidth, x, y, eyeWidth, eyeHeight, kHudInset, half * 2, disparity,
             g_options.vrHudDepth,
             frameAlreadyPublished()
                 ? "ALREADY TAKEN -- these pixels reach the monitor and never the eyes"
                 : "not yet taken, so these pixels are in it");
    }

    if (g_options.vrHudEye != 2)
        g_stampDrawOriginal(static_cast<int>(reinterpret_cast<intptr_t>(left)), x + half, y,
                            a4, a5);
    if (g_options.vrHudEye != 1)
        g_stampDrawOriginal(static_cast<int>(reinterpret_cast<intptr_t>(right)), x - half, y,
                            a4, a5);
}

int __cdecl bitmapDrawThunk(int source, int x, int y, int dest) {
    ++g_bitmapDrawCount;
    noteComposited(source);
    noteComposited(dest);

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
    // pixels, the reverse of the names above. The +Y mirror pane's image belongs in the right
    // half of the texture, so its copy is moved there; roglv2 copies the read surface's own
    // width, so exactly that half is written.
    x = stereoMirrorCopyX(dest, x);

    return g_bitmapDrawOriginal ? g_bitmapDrawOriginal(source, x, y, dest) : 0;
}

void __cdecl drawDebugTextThunk(void* surface, unsigned colour, int x, int y,
                                const char* text, int background) {
    frameBudgetHudText();
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
    // GetProcAddress at 0x004E2338, which writes 0x00866E08. Anything else here means we
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

    g_stampDrawSlot = rasterGlobal(binding.image, "stampDraw");
    if (g_stampDrawSlot && *g_stampDrawSlot) {
        g_stampDrawOriginal = reinterpret_cast<StampDrawFn>(*g_stampDrawSlot);
        void* census = reinterpret_cast<void*>(&stampDrawThunk);
        if (patchBytes(g_stampDrawSlot, &census, sizeof(census))) {
            logf("hud: stampDraw census installed at global %p -- all five arguments are "
                 "logged for the first calls of a phase, unnamed, so one race says which "
                 "is the stamp set and which is the position.",
                 reinterpret_cast<void*>(g_stampDrawSlot));
        } else {
            g_stampDrawOriginal = nullptr;
        }
    }

    logf("hud: drawDebugText thunked at global %p (held %p) -- gplc67's own slot is "
         "00866E08; if this says otherwise the thunk is on a copy nobody calls. The "
         "in-race HUD is re-issued once per eye surface, with the anchor class remapped "
         "to the eye's own width. Items on any other destination, the frame-time readout "
         "included, are untouched.",
         reinterpret_cast<void*>(g_slot), reinterpret_cast<void*>(g_original));
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
    static int stampReports = 4;
    static int bitmapReports = 4;
    static int censusReports = 6;

    if (g_slot && g_original) {
        reassert("drawDebugText", g_slot, reinterpret_cast<void*>(&drawDebugTextThunk),
                 reinterpret_cast<void**>(&g_original), textReports);
    }
    if (g_stampDrawSlot && g_stampDrawOriginal) {
        reassert("stampDraw", g_stampDrawSlot, reinterpret_cast<void*>(&stampDrawThunk),
                 reinterpret_cast<void**>(&g_stampDrawOriginal), stampReports);
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
        g_stampReports = 12;
        g_stampHighWater = 0;
        for (int i = 0; i < kCensusCount; ++i) {
            g_census[i].reports = 6;
            g_census[i].high = 0;
        }
        g_lastReportedCount = 0;
        g_countReports = 8;
        g_seenCount = 0;
        // Handles are recycled across a mode change, so what was composited or logged
        // before it says nothing about the surfaces after it.
        resetStampTracking();
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

    if (g_stampCount > g_stampHighWater && g_stampReports > 0) {
        g_stampHighWater = g_stampCount;
        logf("hud: up to %d stampDraw call(s) in one frame now.", g_stampHighWater);
    }
    g_stampCount = 0;

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

void hudEndFrame() {
    DWORD now = GetTickCount();
    if (g_stampWindow.start == 0) g_stampWindow.start = now;

    ++g_stampWindow.frames;
    g_stampWindow.calls += g_frameStampCalls;
    if (g_frameStampCalls > g_stampWindow.maxCalls) g_stampWindow.maxCalls = g_frameStampCalls;
    g_stampWindow.dropped += g_frameDropped;
    g_stampWindow.ungrouped += g_frameUngrouped;

    if (!g_options.vrSkipStampText) flushStampStrings(now);

    g_frameStringCount = 0;
    g_frameStampCalls = 0;
    g_frameDropped = 0;
    g_frameUngrouped = 0;

    DWORD elapsed = now - g_stampWindow.start;
    if (elapsed < 1000) return;

    // Silent through a second with no stamps at all, which is every second of the front
    // end: the thunk counts only world-mode calls.
    if (g_stampWindow.calls > 0) {
        double perFrame = static_cast<double>(g_stampWindow.calls) / g_stampWindow.frames;
        if (g_options.vrSkipStampText) {
            logf("hud: stamps, last %lu ms: %lld calls over %d frames (%.0f a frame, max %d), "
                 "%lld dropped (%.0f a frame)",
                 static_cast<unsigned long>(elapsed), g_stampWindow.calls,
                 g_stampWindow.frames, perFrame, g_stampWindow.maxCalls,
                 g_stampWindow.dropped,
                 static_cast<double>(g_stampWindow.dropped) / g_stampWindow.frames);
        } else {
            int distinct = 0;
            int waiting = 0;
            int surfaces[16];
            int surfaceCount = 0;
            for (int i = 0; i < g_knownCount; ++i) {
                KnownString& k = g_known[i];
                if (k.waiting) ++waiting;
                if (!k.seen) continue;
                k.seen = false;
                ++distinct;
                bool listed = false;
                for (int j = 0; j < surfaceCount; ++j)
                    if (surfaces[j] == k.dest) listed = true;
                if (!listed && surfaceCount < 16) surfaces[surfaceCount++] = k.dest;
            }
            logf("hud: stamps, last %lu ms: %lld calls over %d frames (%.0f a frame, max %d), "
                 "%d distinct strings on %d surface(s); %d text lines logged%s, %d changed "
                 "strings held back by the rate limit, %d string sightings past the %d-entry "
                 "table, %d calls past the %d-string frame table",
                 static_cast<unsigned long>(elapsed), g_stampWindow.calls,
                 g_stampWindow.frames, perFrame, g_stampWindow.maxCalls, distinct,
                 surfaceCount, g_stampWindow.lines,
                 g_stampWindow.lines >= kMaxTextLinesPerSecond ? " (CAPPED)" : "", waiting,
                 g_stampWindow.untracked, kMaxKnownStrings, g_stampWindow.ungrouped,
                 kMaxFrameStrings);
        }
    }
    g_stampWindow = StampWindow{};
    g_stampWindow.start = now;
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
