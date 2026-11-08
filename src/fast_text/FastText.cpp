#include "fast_text/FastText.h"

#include "Discovery.h"
#include "GlTrace.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"
#include "PostProfile.h"
#include "ReplayPanel.h"
#include "Stereo.h"
#include "Window.h"
#include "fast_text/TextDraw.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace gplvr {
namespace {

using fasttext::Quad;
using fasttext::TextDraw;

// -- roglv2's layout, as reversed in report 71 ----------------------------------------------
//
// Bitmap (both the stamp destination and a glyph): +4 pixel accessor, +8 pixel format,
// +0x0C width, +0x10 height, +0x1C blit class, +0x20 pixel buffer, +0x24/+0x28 the instance's
// absolute origin inside its root surface (rogl_bitmapInstanceSetRect 0x200193F0).
constexpr unsigned kBitmapFormat = 0x08;
constexpr unsigned kBitmapWidth = 0x0C;
constexpr unsigned kBitmapHeight = 0x10;
constexpr unsigned kBitmapClass = 0x1C;
constexpr unsigned kBitmapPixels = 0x20;
constexpr unsigned kBitmapOriginX = 0x24;
constexpr unsigned kBitmapOriginY = 0x28;

// The blit table is indexed `1 + srcClass + 17 * destClass` (rogl_blitDispatch 0x20011E80).
// The OpenGL controller registers class 8 for the framebuffer -- every system-memory source
// into it is rogl_blitSysToGl_drawPixels -- and class 9 for a GL texture, which goes to
// glTexSubImage2D and has no place in the framebuffer we are drawing into.
constexpr int kBlitClassGlFramebuffer = 8;

// Pixel buffer: +0x0C row length in pixels, +0x14 row pitch in bytes; vtable slot 6 locks and
// returns the base, slot 7 unlocks.
constexpr unsigned kPixelsRowLength = 0x0C;
constexpr unsigned kPixelsPitch = 0x14;
constexpr unsigned kPixelsVtableLock = 0x18;
constexpr unsigned kPixelsVtableUnlock = 0x1C;

// Stamp set (rogl_fontBlitGlyph 0x20015EB0): +0x0C an object whose +0x3C is the colour dword
// 0xAARRGGBB, +0x34 the glyph record array, +0x40 the set's own 16-entry palette.
constexpr unsigned kSetColourHolder = 0x0C;
constexpr unsigned kSetGlyphs = 0x34;
constexpr unsigned kSetPalette = 0x40;
constexpr unsigned kColourInHolder = 0x3C;
constexpr unsigned kGlyphStride = 16;
constexpr unsigned kGlyphBitmap = 0x00;
constexpr unsigned kGlyphOriginX = 0x08;
constexpr unsigned kGlyphOriginY = 0x0C;

// A set without a palette of its own skips the recolour in rogl_fontBlitGlyph, so its glyphs are
// drawn in the glyph bitmap's own colormap: rogl_blitSysToGl_drawPixels takes it through the
// bitmap's vtable slot 3, which writes {16 dwords 0xAARRGGBB, reference count} and adds a
// reference. The classes keep it at different offsets (BitmapMemory 0x2005FE74 at +0x38,
// BitmapInstance 0x2006029C at +0x3C), so it is asked for through the slot.
constexpr unsigned kBitmapVtableColormap = 0x0C;

// PolyDrawerZBuffer +184 is the DisplayOpenGL; the display's +0x0C/+0x10 are the width and
// height the ortho is built from and +0x24/+0x28 the destination origin it was last given
// (rogl_displaySetDestination 0x20009F10).
constexpr unsigned kDrawerDisplay = 184;
constexpr unsigned kDisplayWidth = 0x0C;
constexpr unsigned kDisplayHeight = 0x10;
constexpr unsigned kDisplayOriginX = 0x24;
constexpr unsigned kDisplayOriginY = 0x28;

// stampDraw 0x20004900: the globals it reads, by the INSTRUCTION that holds each address -- the
// offset of the opcode, with the 32-bit operand following it. Not the operand's own offset:
// 71b, where these four held 0x23, 0x34, 0x5F and 0x17 and every opcode check therefore looked
// at the middle of an address, so all four globals read back null and nothing installed.
constexpr unsigned kStampDrawRva = 0x4900;
constexpr unsigned kBatchOpenAt = 0x22;     // A1 imm32       mov eax, [rogl_batchOpen]
constexpr unsigned kDrawerAt = 0x32;        // 8B 35 imm32    mov esi, [rogl_polyDrawerCurrent]
constexpr unsigned kHandleTableAt = 0x5D;   // 8B 0D imm32    mov ecx, [rogl_handleTable]
constexpr unsigned kStampSetManagerRva = 0x18850;
constexpr unsigned kStampSetInstanceAt = 0x16;  // A1 imm32   mov eax, [the manager instance]

// THE PIXEL ZOOM, which decides which way up a glyph goes (report 71c).
//
// rogl_displaySetupGlState 0x20009C50 is the ONLY place in roglv2 that calls glPixelZoom, once
// per display, and it passes (1.0, flt_20069790). That constant is -1.0, which is what makes
// glDrawPixels lay a top-down image down the screen instead of up it. The first build assumed
// the GL default of (1, 1) and drew every glyph mirrored. The factor is read here out of
// roglv2's own code -- `fld ds:flt_20069790` at RVA 0x149 of that function -- so the mapping
// follows whatever the rasterizer actually asks for rather than a constant in this file.
constexpr unsigned kDisplaySetupGlStateRva = 0x9C50;
constexpr unsigned kPixelZoomYAt = 0x149;  // D9 05 imm32    fld ds:[the y zoom factor]

// roglv2 is about 0x84000 bytes. A global that reads out of a `mov` but lands outside a window
// this size around the module base is a misparse, not an address.
constexpr uintptr_t kModuleWindow = 16u << 20;

// The prologue bytes of stampDraw, checked before its address is used as a landmark:
// push ebp; mov ebp, esp; push -1; push offset <handler>.
constexpr unsigned char kStampDrawPrologue[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68};

// The alpha ramp constant roglv2 multiplies by, as the float it is in the image (0x3B888889,
// the nearest float above 1/240). Taken as bits so the arithmetic below is roglv2's own, not
// whatever a decimal literal happens to parse to.
constexpr uint32_t kAlphaScaleBits = 0x3B888889u;

// -- our own limits --------------------------------------------------------------------------

constexpr size_t kMaxCachedGlyphs = 4096;
constexpr size_t kMaxBatchQuads = 2048;

// -- GPL'S TEXT, PLACED IN THE EYES (VR only) ------------------------------------------------
//
// A RUN IS THE UNIT THAT MOVES, not a glyph and not the surface.
//
// GPL anchors its notices to the corners of its 2560x1440 layout -- "PRESS F1 FOR HELP" at
// (5, 143), "PRESS PAUSE TO RESUME" at (2, 1310) -- so anything that scales the LAYOUT drives
// those corners further into the periphery, which is the opposite of what is wanted. Scaling a
// run about its own centre and then moving that centre keeps the spacing GPL authored inside
// the line and puts the line where it can be read.
//
// A run's extent is only known once it has ended, and its first glyph has to be placed before
// that. So the extent comes from the FRAME BEFORE: a run is recognised across frames by its
// stamp set and the BASELINE GPL ASKED FOR, both of which hold still while the text changes,
// and a run seen for the first time is placed by its own first glyph and counted. GPL's
// destination handles are recycled every frame -- the message band is a fresh 2540x15 instance
// each time -- so the handle cannot be part of the key.
//
// THE BASELINE, NOT THE ROW THE GLYPH LANDS ON. roglv2 takes the glyph's own hot spot off the
// y before it rasterises, and a hot spot belongs to the glyph: keying on the result cut one
// line into a run for every distinct bearing in it, each of those was centred on the two or
// three letters that happened to share a bearing, and the inset then pulled those centres
// together until the letters of one word sat on top of each other. GPL's `y` is the line.
constexpr int kMaxPlacedRuns = 32;

struct PlacedRun {
    int  set = 0;
    int  row = 0;  // the baseline GPL passed, in its own layout space
    bool known = false;
    int  lastMinX = 0;  // the previous frame's extent, which is what places this frame
    int  lastMaxX = 0;
    int  minX = 0;  // this frame's, accumulating
    int  maxX = 0;
    // The rows the run's glyphs cover, relative to its baseline: this frame's, accumulating, and
    // the previous frame's, which is what separateRuns stacks.
    int  top = 0;
    int  bottom = 0;
    int  lastTop = 0;
    int  lastBottom = 0;
    // Eye pixels added to the run's placed row so it clears every run above it.
    float pushY = 0.0f;
    bool seenThisFrame = false;
};

typedef int(__cdecl* RasterFn)(int, int, int, int, int, int, int, int);
typedef int(__thiscall* PixelLockFn)(void* self);
typedef int(__fastcall* DrawerSlotFn)(void* self, void* argument);

// What a bitmap's colormap slot writes: the entries and the shared reference count.
struct ColormapRef {
    uint32_t* entries;
    long*     references;
};
typedef void(__thiscall* ColormapFn)(void* self, ColormapRef* out);

// What one stamp needs, read out of roglv2's objects in one guarded block.
struct StampSource {
    uint8_t* dest;
    int      destWidth;
    int      destHeight;
    int      destClass;
    int      destOriginX;
    int      destOriginY;
    uint8_t* set;  // the stamp set object, which owns this glyph table
    uint8_t* glyph;
    int      glyphWidth;
    int      glyphHeight;
    int      glyphFormat;
    int      glyphOriginX;  // the glyph bitmap's own sub-rect origin in its pixel buffer
    int      glyphOriginY;
    uint8_t* pixels;
    int      pitch;      // bytes a row in the bitmap's own buffer
    int      rowLength;  // pixels a row, which is what roglv2 gives GL_UNPACK_ROW_LENGTH
    int      hotX;  // the glyph record's hot spot, subtracted from the stamp position
    int      hotY;
    uint32_t colour;
    int      hasPalette;
    // The glyph bitmap's own colormap, read only for a set without a palette.
    uint32_t glyphColormap[16];
    // Which of readStamp's checks refused, in words, or null when it read the whole stamp. For a
    // block that failed `readable`, the block it asked about.
    const char* refusal;
    const void* refusedAt;
    size_t      refusedBytes;
};

// One frame a second is sampled for WHAT is being stamped, because the cost only matters if it
// is on screen while the user is driving. The 2026-09-14 string census found the only in-race
// stamp text at spa67 to be GPL's "PRESS PAUSE TO RESUME", so a line naming the runs is what
// separates "the HUD costs 5.78 ms" from "the pause message costs 5.78 ms". A glyph joins the
// run before it when the destination, the set and the row match and it sits a little to the
// right.
//
// A CODE IS NOT A CHARACTER. gpl_fontGlyphIndex (0x4EDC80) looks the character up in the font's
// own STL map, so the index is font data with no formula behind it, and the sets behind the
// in-race notices come through as 0, 14, 17, 48 for ' ', '.', '1', 'P'. A code outside printable
// ASCII is therefore escaped with its raw value rather than collapsed to one character, which is
// what lets this line serve as the inventory of what GPL stamps: a set nobody has decoded yet
// still arrives as readable numbers. In VR it is the ONLY such inventory, because Hud.cpp's own
// census is not installed there.
constexpr int kMaxSampleRuns = 4;
constexpr int kMaxSampleChars = 40;
// Room for every sampled code to arrive as "<255>".
constexpr int kMaxSampleBytes = kMaxSampleChars * 5;

struct SampleRun {
    int  dest = 0;
    int  set = 0;
    int  x = 0;
    int  y = 0;
    int  lastX = 0;
    int  length = 0;  // codes recorded, which is not the number of bytes `text` holds
    int  used = 0;
    char text[kMaxSampleBytes + 1] = {};
};

// What became of one stamp. A kind of stamp is a stamp set plus an outcome, and one of each is
// described in full once a second, with the geometry roglv2's glDrawPixels path would produce
// beside our own. 71c was found by reading roglv2 rather than a log, and this is what would
// have found it in one run: which sets reach us, which fall back and why, and which rows of the
// destination each glyph lands on.
enum StampOutcome {
    kOutcomeDrawn = 0,
    kOutcomeClippedAway,
    kOutcomeOptionOff,
    kOutcomeUnreadable,
    kOutcomeDestination,
    kOutcomeNoPalette,
    kOutcomeFormat,
    kOutcomeNoGl,
    kOutcomeAtlasFull,
    kOutcomes
};
const char* const kOutcomeNames[kOutcomes] = {
    "DRAWN by us",       "clipped away by roglv2's own rule",
    // Unreachable since the fastText option went on 2026-09-27. The slot stays so the rest of
    // the outcomes keep their numbers, which the probe records carry.
    "(no longer used)",  "unreadable",
    "destination is not the GL framebuffer",
    // Unreachable since 2026-10-17: a set without a palette is drawn in the glyph bitmap's own
    // colormap. The slot stays for the same reason as the one above.
    "(no longer used)",
    "the glyph is not an 8-bit colour-index image",
    "no GL",             "the atlas is full",
};

constexpr int kMaxProbes = 8;

struct StampProbe {
    bool used = false;
    int  set = 0;
    int  outcome = 0;
    int  glyph = 0;
    int  dest = 0;
    int  destClass = 0;
    int  destWidth = 0;
    int  destHeight = 0;
    int  destOriginX = 0;
    int  destOriginY = 0;
    int  glyphWidth = 0;
    int  glyphHeight = 0;
    int  glyphFormat = 0;
    int  pitch = 0;
    int  rowLength = 0;
    int  srcOriginX = 0;
    int  srcOriginY = 0;
    int  hotX = 0;
    int  hotY = 0;
    uint32_t colour = 0;
    int  rasterX = 0;  // object space, dest origin included
    int  rasterY = 0;
    int  width = 0;   // after the clip
    int  height = 0;
    int  skipX = 0;
    int  skipY = 0;
    // What our quad actually spans, filled after it is built. Logged beside the band derived
    // from roglv2's pixel zoom, so the log says whether the code and the derivation agree
    // rather than asserting that they do.
    bool  haveQuad = false;
    float quadV0 = 0.0f;
    float quadV1 = 0.0f;
};

struct CachedGlyph {
    uint8_t* bitmap = nullptr;
    void*    set = nullptr;
    int      glyphIndex = 0;
    int      width = 0;
    int      height = 0;
    int      atlasX = 0;
    int      atlasY = 0;
};

struct Counters {
    unsigned calls = 0;
    unsigned drawn = 0;
    unsigned cacheHits = 0;
    unsigned cacheMisses = 0;
    unsigned clippedAway = 0;
    unsigned fellBackDestination = 0;
    unsigned glyphColormaps = 0;  // drawn in the glyph bitmap's own colormap, set without palette
    unsigned fellBackFormat = 0;
    unsigned fellBackUnreadable = 0;
    unsigned fellBackAtlasFull = 0;
    unsigned fellBackNoGl = 0;
    unsigned frames = 0;
    long long cpuTicks = 0;

    // The VR placement's own tally. EVERY TERM THAT CAN DECLINE HAS ONE: a glyph drawn where
    // GPL put it and a glyph placed in the eyes look identical from a counter that only says
    // "drawn", and this path exists to move them.
    unsigned placedPairs = 0;      // glyphs drawn once per eye at a chosen place
    unsigned placeNoEyeRects = 0;  // no stereo pair this frame
    unsigned placeFrontEnd = 0;    // the engine is in the front end; GPL's layout is left alone
    unsigned placeNoLayout = 0;    // the engine's mode size is not known yet
    unsigned placeOffLayout = 0;   // the destination is not part of GPL's own 2D frame
    unsigned placeFirstSight = 0;  // a run with no extent yet, placed by its first glyph
    unsigned placeOutsideEye = 0;  // scaled past the eye's own rect, so dropped rather than bled
    unsigned placeRunTableFull = 0;
};

// How long the install is allowed to keep trying. The slots are resolved at discovery, but the
// pointer in one of them is only roglv2's own code once the rasterizer has finished resolving
// its exports, so the first attempt can legitimately be too early.
constexpr unsigned kMaxLocateAttempts = 1800;
constexpr unsigned kLoudEvery = 120;

struct State {
    bool     bound = false;
    bool     installed = false;
    bool     pending = false;  // the slots are ours to take, roglv2 is not located yet
    unsigned attempts = 0;
    void** slot[kRasterKinds] = {};
    RasterFn real[kRasterKinds] = {};

    uint8_t*  roglBase = nullptr;
    int32_t*  batchOpen = nullptr;
    uint8_t** drawer = nullptr;
    uint8_t** handleTable = nullptr;
    uint8_t** stampSetManager = nullptr;
    float     pixelZoomY = -1.0f;  // read from roglv2; negative means a top-down image

    bool         glTried = false;
    bool         glReady = false;
    GlApi gl;
    TextDraw     draw;
    HGLRC (WINAPI* wglGetCurrentContext)() = nullptr;
    HGLRC context = nullptr;

    std::vector<CachedGlyph> glyphs;
    bool                     wasFrontEnd = false;

    SampleRun                samples[kMaxSampleRuns];
    int                      sampleCount = 0;
    StampProbe               probes[kMaxProbes];
    int                      probeCount = 0;
    bool                     sampling = true;
    bool                     wantSample = false;

    Counters                 window;
    fasttext::DrawStats      drawStats;
    bool                     loggedScale = false;
    int                      reportsLeft = 6;

    // Which of the seven 2D globals this path holds. In flat mode it is all of them; in VR it
    // is stampDraw alone, because Hud.cpp needs bitmapDraw for the mirror panes' copy offset
    // and the two cannot both re-assert over one slot -- each would adopt the other's thunk as
    // "the original" and the chain would call itself.
    bool  owns[kRasterKinds] = {};
    bool  vrPlacement = false;  // vr, decided once at bind
    int   placeReports = 4;

    PlacedRun runs[kMaxPlacedRuns];
    int       runCount = 0;
    int       runsPushed = 0;  // runs separateRuns moved, as of the last frame

    // The pending quads belong in the replay panel's layer rather than in the frame. One batch
    // never mixes the two: a glyph for the other side flushes first.
    bool batchToPanel = false;

    // Each distinct (set, refusal) readStamp has given, so each is logged once with what had
    // been read before it refused.
    struct UnreadableSeen {
        int         set;
        const char* refusal;
    };
    UnreadableSeen unreadableSeen[16] = {};
    int            unreadableSeenCount = 0;
};

State g_state;

long long now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto start = static_cast<const uint8_t*>(info.BaseAddress);
    return static_cast<const uint8_t*>(address) + bytes <= start + info.RegionSize;
}

int32_t readInt(const uint8_t* object, unsigned offset) {
    int32_t value = 0;
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}

uint8_t* readPointer(const uint8_t* object, unsigned offset) {
    uint8_t* value = nullptr;
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}

// Up to sixteen bytes at `at`, as hex, for a log line that says what was actually there.
void formatBytes(const uint8_t* at, char* text, size_t size) {
    text[0] = 0;
    if (!readable(at, 16)) {
        std::snprintf(text, size, "(not readable)");
        return;
    }
    size_t used = 0;
    for (int i = 0; i < 16 && used + 4 < size; ++i) {
        int written = std::snprintf(text + used, size - used, "%02X ", at[i]);
        if (written <= 0) break;
        used += static_cast<size_t>(written);
    }
}

// The module a code address belongs to, and its base -- roglv2 relocates, and the base is the
// loader's answer rather than the image's preferred one or arithmetic on an entry point.
HMODULE moduleOf(const void* address) {
    HMODULE module = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(address), &module);
    return module;
}

// The operand of a `mov reg, [global]` whose opcode is at `function + at`. Every failure says
// which global it was, the address it looked at, and the bytes it found there.
uint8_t* globalFrom(const char* name, const uint8_t* function, unsigned at,
                    const unsigned char* opcode, unsigned opcodeLength, const uint8_t* base,
                    bool loud) {
    const uint8_t* site = function + at;
    char           bytes[80];
    if (!readable(site, opcodeLength + 4)) {
        if (loud)
            logf("fast text: %s -- the %u bytes at %p are not readable", name, opcodeLength + 4,
                 static_cast<const void*>(site));
        return nullptr;
    }
    if (std::memcmp(site, opcode, opcodeLength) != 0) {
        if (loud) {
            formatBytes(site, bytes, sizeof(bytes));
            logf("fast text: %s -- %p holds %s, and the instruction there should start %02X%s",
                 name, static_cast<const void*>(site), bytes, opcode[0],
                 opcodeLength > 1 ? " .." : "");
        }
        return nullptr;
    }
    uint32_t value = 0;
    std::memcpy(&value, site + opcodeLength, sizeof(value));
    auto global = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(value));
    if (global < base || global >= base + kModuleWindow || !readable(global, 4)) {
        if (loud) {
            formatBytes(site, bytes, sizeof(bytes));
            logf("fast text: %s -- the operand at %p reads %p, which is not a global of a "
                 "module based at %p (the bytes there are %s)",
                 name, static_cast<const void*>(site), static_cast<void*>(global),
                 static_cast<const void*>(base), bytes);
        }
        return nullptr;
    }
    return global;
}

// roglv2's projection x modelview for its 2D drawing:
// glOrtho(-ox, W - ox, H - oy, -oy, 0, 1) with the modelview translate (0.375, 0.375, 0).
void screenMatrix(int width, int height, int originX, int originY, float out[16]) {
    double w = width > 0 ? width : 1;
    double h = height > 0 ? height : 1;
    double left = -originX;
    double right = w - originX;
    double bottom = h - originY;
    double top = -originY;
    double sx = 2.0 / (right - left);
    double sy = 2.0 / (top - bottom);
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = static_cast<float>(sx);
    out[5] = static_cast<float>(sy);
    out[10] = -2.0f;
    out[12] = static_cast<float>(sx * 0.375 - (right + left) / (right - left));
    out[13] = static_cast<float>(sy * 0.375 - (top + bottom) / (top - bottom));
    out[14] = -1.0f;
    out[15] = 1.0f;
}

// The 16-entry palette rogl_fontBlitGlyph builds for a stamp set's colour: the colour's RGB
// with a 0..A alpha ramp, the alpha truncated after a double multiply by the float nearest
// 1/240, exactly as the x87 sequence at 0x20015ED2 does it. The texture holds R, G, B, A bytes
// in that order, which is the order rogl_glSetPixelMapFromPalette feeds the four pixel maps.
void buildPalette(uint32_t colour, uint32_t out[16]) {
    float scaleFloat = 0.0f;
    std::memcpy(&scaleFloat, &kAlphaScaleBits, sizeof(scaleFloat));
    double scale = static_cast<double>(colour >> 24) * static_cast<double>(scaleFloat);
    uint32_t red = (colour >> 16) & 0xFFu;
    uint32_t green = (colour >> 8) & 0xFFu;
    uint32_t blue = colour & 0xFFu;
    for (int i = 0; i < 16; ++i) {
        double value = static_cast<double>(16 * i) * scale;
        uint32_t alpha = static_cast<uint32_t>(static_cast<int>(value)) & 0xFFu;
        out[i] = red | (green << 8) | (blue << 16) | (alpha << 24);
    }
}

// rogl_clipRectToBitmap 0x20012720, reproduced term for term so a glyph that hangs off an edge
// keeps the pixels roglv2 would have kept. It clips the rectangle as if it extended right and
// down from (x, y); the draw then runs upwards in the window, which is roglv2's own quirk and
// only matters when a glyph is half off the top, where it drops the whole glyph either way.
bool clipToBitmap(int destWidth, int destHeight, int& x, int& y, int& width, int& height,
                  int& skipX, int& skipY) {
    int right = x + width;
    int bottom = y + height;
    skipX = 0;
    skipY = 0;
    if (x < 0) {
        skipX = -x;
        x = 0;
    } else if (x > destWidth) {
        skipX = destWidth - x;
        x = destWidth;
    }
    if (right < x)
        right = x;
    else if (right > destWidth)
        right = destWidth;
    width = right - x;
    if (y < 0) {
        skipY = -y;
        y = 0;
    } else if (y > destHeight) {
        y = destHeight;
    }
    if (bottom < y)
        bottom = y;
    else if (bottom > destHeight)
        bottom = destHeight;
    height = bottom - y;
    return width > 0 && height > 0;
}

// Everything one stamp needs, in one block with no C++ object in scope, so the whole read can
// sit under a structured exception handler. roglv2 checks none of these handles itself, and a
// stamp against a surface whose table slot has been freed has taken this game down before
// (Hud.cpp, 2026-09-12).
bool canRead(StampSource& out, const void* address, size_t bytes, const char* refusal) {
    if (address && readable(address, bytes)) return true;
    out.refusal = refusal;
    out.refusedAt = address;
    out.refusedBytes = bytes;
    return false;
}

// The glyph bitmap's colormap, taken the way rogl_blitSysToGl_drawPixels takes it. Only an 8-bit
// glyph has one to take; any other format is refused later by its own check. Called inside
// readStamp's guarded block.
bool readGlyphColormap(StampSource& out) {
    if (out.glyphFormat < 0 || out.glyphFormat > 2) return true;
    uint8_t* vtable = readPointer(out.glyph, 0);
    if (!canRead(out, vtable, kBitmapVtableColormap + 4,
                 "the glyph bitmap's vtable cannot be read"))
        return false;
    auto colormap = reinterpret_cast<ColormapFn>(readPointer(vtable, kBitmapVtableColormap));
    uint8_t* code = reinterpret_cast<uint8_t*>(colormap);
    if (!g_state.roglBase || code < g_state.roglBase || code >= g_state.roglBase + kModuleWindow) {
        out.refusal = "the glyph bitmap's colormap slot is not roglv2 code";
        out.refusedAt = code;
        return false;
    }
    ColormapRef ref = {};
    colormap(out.glyph, &ref);
    // The slot added a reference. The bitmap keeps its own, so dropping ours never frees.
    if (ref.references && *ref.references > 1) --*ref.references;
    if (!canRead(out, ref.entries, sizeof(out.glyphColormap),
                 ref.entries ? "the glyph bitmap's colormap cannot be read"
                             : "neither the set nor the glyph bitmap has a palette"))
        return false;
    std::memcpy(out.glyphColormap, ref.entries, sizeof(out.glyphColormap));
    return true;
}

bool readStamp(int handle, int set, int glyph, StampSource& out) {
    State& s = g_state;
    std::memset(&out, 0, sizeof(out));
    if (handle <= 0 || set < 0 || glyph < 0) {
        out.refusal = "a negative or zero handle, set or glyph";
        return false;
    }
    if (!s.handleTable || !s.stampSetManager) {
        out.refusal = "roglv2's handle table or stamp set manager is not located";
        return false;
    }
    __try {
        uint8_t* table = *s.handleTable;
        if (!canRead(out, table, 12, "the handle table cannot be read")) return false;
        uint8_t* entries = readPointer(table, 8);
        if (!entries) {
            out.refusal = "the handle table has no entries";
            return false;
        }
        if (!canRead(out, entries + 8 * static_cast<unsigned>(handle), 8,
                     "the destination's handle-table entry cannot be read"))
            return false;
        out.dest = readPointer(entries, 8 * static_cast<unsigned>(handle));
        if (!canRead(out, out.dest, 0x2C, "the destination bitmap cannot be read")) return false;
        out.destWidth = readInt(out.dest, kBitmapWidth);
        out.destHeight = readInt(out.dest, kBitmapHeight);
        out.destClass = readInt(out.dest, kBitmapClass);
        out.destOriginX = readInt(out.dest, kBitmapOriginX);
        out.destOriginY = readInt(out.dest, kBitmapOriginY);

        uint8_t* manager = *s.stampSetManager;
        if (!canRead(out, manager, 12, "the stamp set manager cannot be read")) return false;
        uint8_t* sets = readPointer(manager, 8);
        if (!sets) {
            out.refusal = "the stamp set manager has no sets";
            return false;
        }
        if (!canRead(out, sets + 8 * static_cast<unsigned>(set), 8,
                     "the set's entry in the manager cannot be read"))
            return false;
        uint8_t* setObject = readPointer(sets, 8 * static_cast<unsigned>(set));
        if (!canRead(out, setObject, 0x44, "the stamp set object cannot be read")) return false;
        out.set = setObject;
        out.hasPalette = readPointer(setObject, kSetPalette) != nullptr ? 1 : 0;
        // rogl_fontBlitGlyph reads the colour only for a set with a palette; the sets without one
        // (6, 2 and 9 in the 2026-10-16 logs) have no colour holder at all.
        if (out.hasPalette) {
            uint8_t* holder = readPointer(setObject, kSetColourHolder);
            if (!canRead(out, holder, kColourInHolder + 4,
                         holder ? "the set's colour holder cannot be read"
                                : "the set has a palette and no colour holder"))
                return false;
            out.colour = static_cast<uint32_t>(readInt(holder, kColourInHolder));
        }

        uint8_t* records = readPointer(setObject, kSetGlyphs);
        if (!records) {
            out.refusal = "the set has no glyph records";
            return false;
        }
        uint8_t* record = records + kGlyphStride * static_cast<unsigned>(glyph);
        if (!canRead(out, record, kGlyphStride, "the glyph's record cannot be read"))
            return false;
        out.hotX = readInt(record, kGlyphOriginX);
        out.hotY = readInt(record, kGlyphOriginY);
        out.glyph = readPointer(record, kGlyphBitmap);
        if (!canRead(out, out.glyph, 0x2C,
                     out.glyph ? "the glyph's bitmap cannot be read"
                               : "the glyph record has no bitmap"))
            return false;
        out.glyphWidth = readInt(out.glyph, kBitmapWidth);
        out.glyphHeight = readInt(out.glyph, kBitmapHeight);
        out.glyphFormat = readInt(out.glyph, kBitmapFormat);
        out.glyphOriginX = readInt(out.glyph, kBitmapOriginX);
        out.glyphOriginY = readInt(out.glyph, kBitmapOriginY);
        out.pixels = readPointer(out.glyph, kBitmapPixels);
        if (!canRead(out, out.pixels, 0x20,
                     out.pixels ? "the glyph's pixel buffer cannot be read"
                                : "the glyph bitmap has no pixel buffer"))
            return false;
        out.pitch = readInt(out.pixels, kPixelsPitch);
        out.rowLength = readInt(out.pixels, kPixelsRowLength);
        if (!out.hasPalette && !readGlyphColormap(out)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.refusal = "a read faulted";
        return false;
    }
    if (out.glyphWidth <= 0 || out.glyphHeight <= 0) {
        out.refusal = "the glyph bitmap has no area";
        return false;
    }
    if (out.glyphWidth > fasttext::kMaxGlyphSide || out.glyphHeight > fasttext::kMaxGlyphSide) {
        out.refusal = "the glyph is larger than the atlas takes";
        return false;
    }
    if (out.pitch <= 0) {
        out.refusal = "the pixel buffer's pitch is not positive";
        return false;
    }
    return true;
}

// Lock the glyph's pixel buffer and hand back the base. The two calls are the ones
// rogl_blitSysToGl_drawPixels makes, slots 6 and 7 of the pixel buffer's vtable.
uint8_t* lockPixels(uint8_t* pixels) {
    if (!pixels || !readable(pixels, 8)) return nullptr;
    uint8_t* vtable = readPointer(pixels, 0);
    if (!vtable || !readable(vtable, kPixelsVtableUnlock + 4)) return nullptr;
    auto lock = reinterpret_cast<PixelLockFn>(readPointer(vtable, kPixelsVtableLock));
    if (!lock) return nullptr;
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(lock(pixels)));
}

void unlockPixels(uint8_t* pixels) {
    if (!pixels || !readable(pixels, 8)) return;
    uint8_t* vtable = readPointer(pixels, 0);
    if (!vtable || !readable(vtable, kPixelsVtableUnlock + 4)) return;
    auto unlock = reinterpret_cast<PixelLockFn>(readPointer(vtable, kPixelsVtableUnlock));
    if (unlock) unlock(pixels);
}

// -- GL --------------------------------------------------------------------------------------

bool ensureGl() {
    State& s = g_state;
    if (!s.wglGetCurrentContext) {
        HMODULE module = GetModuleHandleA("opengl32.dll");
        if (!module) return false;
        s.wglGetCurrentContext =
            reinterpret_cast<HGLRC(WINAPI*)()>(GetProcAddress(module, "wglGetCurrentContext"));
        if (!s.wglGetCurrentContext) return false;
    }
    HGLRC context = s.wglGetCurrentContext();
    if (!context) return false;
    if (s.glReady && context == s.context) return true;
    if (s.glReady && context != s.context) {
        s.draw.release(false);
        s.glyphs.clear();
        s.glReady = false;
        logf("fast text: the GL context changed -- the atlas is rebuilt");
    }
    if (s.glTried && !s.glReady && context == s.context) return false;
    s.glTried = true;
    s.context = context;
    std::string error;
    if (!fillGlApi(s.gl) || !s.draw.init(s.gl, error)) {
        logf("fast text: the GL path could not start (%s) -- every stamp stays roglv2's",
             error.empty() ? "opengl32 entry points" : error.c_str());
        return false;
    }
    s.glyphs.clear();
    s.glReady = true;
    logf("fast text: GL ready -- one %dx%d R8 glyph atlas, a %d-row palette texture and one "
         "batched quad list a run",
         fasttext::kAtlasSize, fasttext::kAtlasSize, fasttext::kMaxPaletteRows);
    return true;
}

// The pending quads, drawn under roglv2's own 2D transform. Everything GPL draws after this
// call lands over them, which is why every other 2D entry point flushes first.
void flushPending() {
    State& s = g_state;
    if (!s.glReady || s.draw.pending() == 0) return;
    uint8_t* drawer = s.drawer ? *s.drawer : nullptr;
    uint8_t* display = (drawer && readable(drawer, kDrawerDisplay + 4))
                           ? readPointer(drawer, kDrawerDisplay)
                           : nullptr;
    if (display && readable(display, kDisplayOriginY + 4)) {
        int width = readInt(display, kDisplayWidth);
        int height = readInt(display, kDisplayHeight);
        // ONE READ-BACK, ONCE. The quad and glRasterPos2i land on the same pixels only while
        // the viewport is the whole display: the raster position is transformed and then
        // glDrawPixels writes w by h pixels from it whatever the viewport scale is, while the
        // quad is scaled. The viewport is read here the first time anything is drawn and the
        // answer is logged, rather than assumed for the life of the mod.
        if (!s.loggedScale && s.gl.getIntegerv) {
            s.loggedScale = true;
            GLint viewport[4] = {};
            s.gl.getIntegerv(gl::kViewport, viewport);
            logf("fast text: roglv2's display is %dx%d at origin %d,%d and the viewport is "
                 "%d,%d %dx%d%s",
                 width, height, readInt(display, kDisplayOriginX),
                 readInt(display, kDisplayOriginY), viewport[0], viewport[1], viewport[2],
                 viewport[3],
                 (viewport[2] == width && viewport[3] == height && viewport[0] == 0 &&
                  viewport[1] == 0)
                     ? ""
                     : " -- THEY DO NOT AGREE: the text will be scaled where roglv2's would "
                       "not be.");
        }
        float screen[16];
        screenMatrix(width, height, readInt(display, kDisplayOriginX),
                     readInt(display, kDisplayOriginY), screen);
        if (s.batchToPanel) {
            // The same quads under the same transform, into the panel's layer instead; if it
            // cannot be bound they land where GPL put them, which is where they always did.
            ReplayPanelTarget target;
            s.draw.draw(screen, s.drawStats);
        } else {
            s.draw.draw(screen, s.drawStats);
        }
        glTraceStepIfStarted("fast text: a run of GPL's glyphs drawn");
    }
}

// roglv2's 2D batch, opened exactly as stampDraw's own prologue opens it: an open polygon batch
// is flushed through the drawer's vtable slot 3, then slot 0 is called with no destination and
// the batch flag becomes 1. Our quads are drawn before that, because the flush it triggers
// belongs to polygons GPL submitted before this text.
void openStampBatch() {
    State& s = g_state;
    if (!s.batchOpen || !s.drawer) return;
    if (*s.batchOpen == 1) return;
    flushPending();
    uint8_t* drawer = *s.drawer;
    if (!drawer || !readable(drawer, 4)) return;
    uint8_t* vtable = readPointer(drawer, 0);
    if (!vtable || !readable(vtable, 16)) return;
    if (*s.batchOpen != 0) {
        auto flush = reinterpret_cast<DrawerSlotFn>(readPointer(vtable, 12));
        *s.batchOpen = 0;
        if (flush) flush(drawer, nullptr);
    }
    auto setDestination = reinterpret_cast<DrawerSlotFn>(readPointer(vtable, 0));
    if (setDestination) setDestination(drawer, nullptr);
    *s.batchOpen = 1;
}

const CachedGlyph* findGlyph(const StampSource& source, void* set, int glyphIndex) {
    State& s = g_state;
    for (const CachedGlyph& cached : s.glyphs) {
        if (cached.bitmap == source.glyph && cached.set == set &&
            cached.glyphIndex == glyphIndex && cached.width == source.glyphWidth &&
            cached.height == source.glyphHeight)
            return &cached;
    }
    return nullptr;
}

// Take the glyph's rows out of roglv2's pixel buffer and put them in the atlas, byte for byte:
// a texel still holds roglv2's colour index, and the shader takes its top nibble the way
// GL_INDEX_SHIFT = -4 does.
const CachedGlyph* cacheGlyph(const StampSource& source, void* set, int glyphIndex) {
    State& s = g_state;
    if (s.glyphs.size() >= kMaxCachedGlyphs) return nullptr;
    uint8_t* base = lockPixels(source.pixels);
    if (!base) return nullptr;
    const uint8_t* first =
        base + static_cast<ptrdiff_t>(source.glyphOriginX) +
        static_cast<ptrdiff_t>(source.glyphOriginY) * source.pitch;
    bool ok = readable(first, static_cast<size_t>(source.pitch) *
                                  static_cast<size_t>(source.glyphHeight - 1) +
                                  static_cast<size_t>(source.glyphWidth));
    int atlasX = 0;
    int atlasY = 0;
    if (ok)
        ok = s.draw.addGlyph(first, source.glyphWidth, source.glyphHeight, source.pitch, atlasX,
                             atlasY, s.drawStats);
    unlockPixels(source.pixels);
    if (!ok) return nullptr;
    CachedGlyph cached;
    cached.bitmap = source.glyph;
    cached.set = set;
    cached.glyphIndex = glyphIndex;
    cached.width = source.glyphWidth;
    cached.height = source.glyphHeight;
    cached.atlasX = atlasX;
    cached.atlasY = atlasY;
    s.glyphs.push_back(cached);
    return &s.glyphs.back();
}

void sampleStamp(int handle, int x, int y, int set, int glyph) {
    State& s = g_state;
    SampleRun* run = nullptr;
    for (int i = 0; i < s.sampleCount; ++i) {
        SampleRun& candidate = s.samples[i];
        if (candidate.dest == handle && candidate.set == set && candidate.y == y &&
            x >= candidate.lastX && x - candidate.lastX <= 64)
            run = &candidate;
    }
    if (!run) {
        if (s.sampleCount >= kMaxSampleRuns) return;
        run = &s.samples[s.sampleCount++];
        *run = SampleRun{};
        run->dest = handle;
        run->set = set;
        run->x = x;
        run->y = y;
    }
    run->lastX = x;
    if (run->length >= kMaxSampleChars) return;
    ++run->length;
    size_t room = static_cast<size_t>(kMaxSampleBytes - run->used);
    int written = 0;
    if (glyph >= 32 && glyph < 127)
        written = std::snprintf(run->text + run->used, room, "%c", static_cast<char>(glyph));
    else
        written = std::snprintf(run->text + run->used, room, "<%d>", glyph);
    if (written < 0 || static_cast<size_t>(written) >= room) return;
    run->used += written;
    run->text[run->used] = 0;
}

// One of each kind of stamp a second, described in full. `source` may be all zeroes when the
// read itself failed; the geometry fields are filled only once the clip has run.
void recordProbe(int handle, int set, int glyph, const StampSource& source, int outcome,
                 int rasterX, int rasterY, int width, int height, int skipX, int skipY) {
    State& s = g_state;
    if (!s.sampling) return;
    for (int i = 0; i < s.probeCount; ++i)
        if (s.probes[i].set == set && s.probes[i].outcome == outcome) return;
    if (s.probeCount >= kMaxProbes) return;
    StampProbe& probe = s.probes[s.probeCount++];
    probe = StampProbe{};
    probe.used = true;
    probe.set = set;
    probe.outcome = outcome;
    probe.glyph = glyph;
    probe.dest = handle;
    probe.destClass = source.destClass;
    probe.destWidth = source.destWidth;
    probe.destHeight = source.destHeight;
    probe.destOriginX = source.destOriginX;
    probe.destOriginY = source.destOriginY;
    probe.glyphWidth = source.glyphWidth;
    probe.glyphHeight = source.glyphHeight;
    probe.glyphFormat = source.glyphFormat;
    probe.pitch = source.pitch;
    probe.rowLength = source.rowLength;
    probe.srcOriginX = source.glyphOriginX;
    probe.srcOriginY = source.glyphOriginY;
    probe.hotX = source.hotX;
    probe.hotY = source.hotY;
    probe.colour = source.colour;
    probe.rasterX = rasterX;
    probe.rasterY = rasterY;
    probe.width = width;
    probe.height = height;
    probe.skipX = skipX;
    probe.skipY = skipY;
}

void noteProbeQuad(int set, int outcome, float v0, float v1) {
    State& s = g_state;
    if (!s.sampling) return;
    for (int i = 0; i < s.probeCount; ++i) {
        StampProbe& probe = s.probes[i];
        if (probe.set != set || probe.outcome != outcome || probe.haveQuad) continue;
        probe.haveQuad = true;
        probe.quadV0 = v0;
        probe.quadV1 = v1;
        return;
    }
}

// One stampDraw call. True when we drew it (or when roglv2 would have drawn nothing).
// -- THE VR PLACEMENT ------------------------------------------------------------------------
//
// Everything below is reached only with `vr`. Flat mode runs the original
// path untouched, and so does the front end, every menu and every frame with no stereo pair --
// which is what keeps the shared code above exercised in the mode this project has four times
// found a VR-only gate hiding a bug in.

PlacedRun* findRun(int set, int row) {
    State& s = g_state;
    for (int i = 0; i < s.runCount; ++i) {
        PlacedRun& run = s.runs[i];
        if (run.set == set && run.row == row) return &run;
    }
    if (s.runCount >= kMaxPlacedRuns) {
        ++s.window.placeRunTableFull;
        return nullptr;
    }
    PlacedRun& run = s.runs[s.runCount++];
    run = PlacedRun{};
    run.set = set;
    run.row = row;
    return &run;
}

// The previous frame's extent becomes this frame's placement, and this frame starts a new one.
// From fastTextEndFrame, once a frame, after everything has been drawn.
void rollRuns() {
    State& s = g_state;
    int kept = 0;
    for (int i = 0; i < s.runCount; ++i) {
        PlacedRun& run = s.runs[i];
        // A run nobody stamped this frame is dropped rather than kept with a stale extent: the
        // notices come and go, and a row that comes back is a new string.
        if (!run.seenThisFrame) continue;
        run.lastMinX = run.minX;
        run.lastMaxX = run.maxX;
        run.lastTop = run.top;
        run.lastBottom = run.bottom;
        run.known = true;
        run.seenThisFrame = false;
        run.minX = 0;
        run.maxX = 0;
        run.top = 0;
        run.bottom = 0;
        if (kept != i) s.runs[kept] = run;
        ++kept;
    }
    s.runCount = kept;
}

// THE INSET MOVES LINES TOGETHER WHILE THE SCALE MAKES THEM TALLER. Two of GPL's notices a line
// apart end up a quarter of a line apart at twice the height, one over the other. So the runs are
// laid out the way placeInEyes will put them -- the previous frame's extents, scaled about the
// run's centre, at the inset row -- and walked in GPL's own top-to-bottom order, each one moved
// down until it clears every run above it that it shares columns with. GPL's order survives;
// only the spacing the inset took away is given back, and only where it was needed.
//
// Right after rollRuns, so the push is ready for the next frame's first glyph of each run.
void separateRuns() {
    State& s = g_state;
    s.runsPushed = 0;
    int layoutWidth = 0;
    int layoutHeight = 0;
    windowEngineModeSize(layoutWidth, layoutHeight);
    if (layoutWidth <= 0 || layoutHeight <= 0) return;

    float scale = static_cast<float>(g_options.vrGplTextScale);
    float inset = g_options.vrGplTextSpread;
    // A clear row between stacked lines, at the glyphs' own scale.
    float gap = scale;

    int order[kMaxPlacedRuns];
    int count = 0;
    for (int i = 0; i < s.runCount; ++i) {
        // Insertion by row, then set, so the walk is GPL's top-to-bottom order and repeatable.
        int at = count++;
        while (at > 0) {
            const PlacedRun& before = s.runs[order[at - 1]];
            const PlacedRun& run = s.runs[i];
            if (before.row < run.row || (before.row == run.row && before.set <= run.set)) break;
            order[at] = order[at - 1];
            --at;
        }
        order[at] = i;
    }

    float left[kMaxPlacedRuns];
    float right[kMaxPlacedRuns];
    float top[kMaxPlacedRuns];
    float bottom[kMaxPlacedRuns];
    for (int k = 0; k < count; ++k) {
        PlacedRun& run = s.runs[order[k]];
        float centreX = 0.5f * static_cast<float>(run.lastMinX + run.lastMaxX);
        float halfWidth = 0.5f * static_cast<float>(run.lastMaxX - run.lastMinX) * scale;
        float placedX = (centreX - 0.5f * static_cast<float>(layoutWidth)) * inset;
        float placedY = (static_cast<float>(run.row) - 0.5f * static_cast<float>(layoutHeight)) *
                        inset;
        left[k] = placedX - halfWidth;
        right[k] = placedX + halfWidth;
        top[k] = placedY + static_cast<float>(run.lastTop) * scale;
        bottom[k] = placedY + static_cast<float>(run.lastBottom) * scale;

        // Moving down past one run can put this one over another, so the walk repeats until a
        // pass moves nothing. Every move is downward and bounded by the runs above, so it ends.
        float push = 0.0f;
        for (int pass = 0; pass <= k; ++pass) {
            bool moved = false;
            for (int j = 0; j < k; ++j) {
                if (left[k] >= right[j] || left[j] >= right[k]) continue;
                if (top[k] + push >= bottom[j] + gap || top[j] >= bottom[k] + push) continue;
                push = bottom[j] + gap - top[k];
                moved = true;
            }
            if (!moved) break;
        }
        top[k] += push;
        bottom[k] += push;
        if (push != run.pushY && s.placeReports > 0 && push > 0.0f) {
            --s.placeReports;
            logf("fast text: the run on GPL row %d (set %d) would overlap the one above it at "
                 "inset %.2f and scale %d -- moved down %.0f eye pixels to clear it",
                 run.row, run.set, static_cast<double>(inset), g_options.vrGplTextScale,
                 static_cast<double>(push));
        }
        run.pushY = push;
        if (push > 0.0f) ++s.runsPushed;
    }
}

// Horizontal disparity in pixels for `vrGplTextDepth` metres, or 0 for infinity. The same
// derivation Hud.cpp uses -- Z = f * IPD / d with f the eye's focal length in pixels -- so the
// two paths cannot disagree about what a depth means. The separation is the runtime's, not a
// constant: without a tracked frame there is none to use and the two copies are identical.
int disparityPixels(int eyeWidth) {
    if (g_options.vrGplTextDepth <= 0.0f || eyeWidth <= 0) return 0;

    float fov = 0.0f;
    float separation = 0.0f;
    if (!stereoEyeGeometry(fov, separation)) return 0;

    float halfAngle = fov * 0.5f;
    if (halfAngle <= 0.0f || halfAngle >= 1.5f) return 0;

    float focal = (static_cast<float>(eyeWidth) * 0.5f) / std::tan(halfAngle);
    float pixels = focal * separation / g_options.vrGplTextDepth;
    float ceiling = static_cast<float>(eyeWidth) * 0.05f;
    if (pixels > ceiling) pixels = ceiling;
    return static_cast<int>(pixels + 0.5f);
}

// One glyph, placed once per eye. True when the quads were appended and the caller must not
// also append the unplaced one; false means the caller carries on exactly as in flat mode, and
// a counter above says which term declined.
//
// `ax`/`ay` are the glyph's clipped rectangle in GPL's own layout space -- absolute, origin
// included -- which is the space the notices' coordinates were authored in. The eye rects are
// in the off-screen target's pixels, and roglv2's own 2D transform spans that target (the
// render target overrides the display's size), so a quad written in target pixels lands where
// it is put. That is the whole reason this works where re-issuing the call cannot: the quad
// goes through the transform, and the destination handle no longer decides anything.
bool placeInEyes(const StampSource& source, int set, int ax, int ay, int baseY, int width,
                 int height, const CachedGlyph& cached, int skipX, int skipY,
                 uint32_t paletteRow) {
    State& s = g_state;
    if (!s.vrPlacement) return false;

    if (windowInFrontEnd()) {
        ++s.window.placeFrontEnd;
        return false;
    }

    int layoutWidth = 0;
    int layoutHeight = 0;
    windowEngineModeSize(layoutWidth, layoutHeight);
    if (layoutWidth <= 0 || layoutHeight <= 0) {
        ++s.window.placeNoLayout;
        return false;
    }

    // Only a destination that is part of GPL's own 2D frame. The full-frame surface and the
    // message band's 2540x15 instance both are; anything laid out against something else has
    // no business being measured from the centre of a layout it was not laid out against.
    if (source.destOriginX < 0 || source.destOriginY < 0 ||
        source.destOriginX + source.destWidth > layoutWidth ||
        source.destOriginY + source.destHeight > layoutHeight) {
        ++s.window.placeOffLayout;
        return false;
    }

    int rect[2][4];
    if (!stereoEyeRect(0, rect[0]) || !stereoEyeRect(1, rect[1])) {
        ++s.window.placeNoEyeRects;
        return false;
    }

    // THE KEY IS THE BASELINE GPL ASKED FOR, NOT THE ROW THE GLYPH LANDS ON. `ay` has already
    // had the glyph's own hot spot taken off it, and a hot spot is per glyph -- so keying on it
    // split one line into a run for every distinct bearing in it, each run was then centred on
    // its own two or three letters, and with the inset applied to each of those centres the
    // letters of one word piled up on each other. GPL's `y` is the line, and it holds still for
    // every glyph on it.
    PlacedRun* run = findRun(set, baseY);
    if (!run) return false;

    // This frame's extent, accumulated whether or not the run is placeable yet.
    if (!run->seenThisFrame) {
        run->seenThisFrame = true;
        run->minX = ax;
        run->maxX = ax + width;
        run->top = ay - baseY;
        run->bottom = ay + height - baseY;
    } else {
        if (ax < run->minX) run->minX = ax;
        if (ax + width > run->maxX) run->maxX = ax + width;
        if (ay - baseY < run->top) run->top = ay - baseY;
        if (ay + height - baseY > run->bottom) run->bottom = ay + height - baseY;
    }

    // A run with no extent yet is placed by this glyph alone, which puts a one-frame-old string
    // a few characters off centre and corrects itself on the next frame. Counted, because
    // "every glyph is a first sighting" would mean the key is not holding still.
    float anchorX = 0.0f;
    if (run->known) {
        anchorX = 0.5f * static_cast<float>(run->lastMinX + run->lastMaxX);
    } else {
        ++s.window.placeFirstSight;
        anchorX = static_cast<float>(ax) + 0.5f * static_cast<float>(width);
    }
    // The line GPL asked for, so every glyph on it shares one anchor and the difference between
    // them is the glyph's own bearing, which the scale carries.
    float anchorY = static_cast<float>(baseY);

    float scale = static_cast<float>(g_options.vrGplTextScale);
    float inset = g_options.vrGplTextSpread;

    // GPL'S OWN PIXELS, PULLED TOWARD THE MIDDLE. This was a fraction of the layout re-read as
    // the same fraction of the eye, and the two rects are not the same shape: the eye measured
    // 3172x3172 against a 2560x1440 layout, so that route stretched x by 1.24 and y by 2.20,
    // which no glyph size followed. A delta measured in GPL's pixels keeps the layout's shape
    // and matches the glyphs, which are drawn in those same pixels; `inset` is then just how
    // much of the delta survives, and 0 would be dead centre.
    float placedX = (anchorX - 0.5f * static_cast<float>(layoutWidth)) * inset;
    float placedY = (anchorY - 0.5f * static_cast<float>(layoutHeight)) * inset;
    if (run->known) placedY += run->pushY;

    float offsetX = (static_cast<float>(ax) - anchorX) * scale;
    float offsetY = (static_cast<float>(ay) - anchorY) * scale;
    float span = static_cast<float>(height) * scale;
    float glyphWidth = static_cast<float>(width) * scale;

    int disparity = disparityPixels(rect[0][2]);

    bool appended = false;
    for (int eye = 0; eye < 2; ++eye) {
        float ex = static_cast<float>(rect[eye][0]);
        float ey = static_cast<float>(rect[eye][1]);
        float ew = static_cast<float>(rect[eye][2]);
        float eh = static_cast<float>(rect[eye][3]);
        // The left eye's copy goes right of centre and the right eye's left of centre, which is
        // convergent and pulls the text toward the viewer.
        float shift = static_cast<float>(eye == 0 ? disparity : -disparity) * 0.5f;

        float x0 = ex + 0.5f * ew + placedX + shift + offsetX;
        float rasterY = ey + 0.5f * eh + placedY + offsetY;

        // A GLYPH THAT LEAVES ITS OWN EYE IS DROPPED, NOT CLAMPED. The off-screen target's two
        // eye rects are adjacent, so a quad hanging off one edge is drawn into the other eye's
        // picture -- one eye's text bleeding into the other is the first of the three faults
        // this project recorded against hand-placed 2D, and a run scaled 4x can reach it.
        if (x0 < ex || x0 + glyphWidth > ex + ew || rasterY < ey ||
            rasterY + span > ey + eh) {
            ++s.window.placeOutsideEye;
            continue;
        }

        Quad quad;
        quad.x0 = x0;
        quad.x1 = x0 + glyphWidth;
        quad.yV0 = s.pixelZoomY < 0.0f ? rasterY : rasterY + 1.0f;
        quad.yV1 = s.pixelZoomY < 0.0f ? rasterY + span : quad.yV0 - span;
        quad.u0 = static_cast<float>(cached.atlasX + skipX);
        quad.u1 = quad.u0 + static_cast<float>(width);
        quad.v0 = static_cast<float>(cached.atlasY + skipY);
        quad.v1 = quad.v0 + static_cast<float>(height);
        quad.palette = paletteRow;
        // The probe line reads "ours spans ..." off this, and a placed glyph that did not set it
        // would be reported as not drawn at all. The left eye's copy is the one recorded.
        if (!appended) noteProbeQuad(set, kOutcomeDrawn, quad.yV0, quad.yV1);
        s.draw.append(quad);
        appended = true;
    }

    if (!appended) {
        // Both eyes refused. Returning false here would draw the glyph where GPL put it, which
        // is the one thing this path exists to stop; it is reported instead and the glyph is
        // dropped, so the only way to see nothing is to ask for a placement nothing fits.
        if (s.placeReports > 0) {
            --s.placeReports;
            logf("fast text: a glyph of set %d at GPL (%d,%d) %dx%d landed outside both eye "
                 "rects at scale %d and inset %.2f -- %u glyphs have landed outside this "
                 "second. Lower vrGplTextScale or raise vrGplTextSpread.",
                 set, ax, ay, width, height, g_options.vrGplTextScale,
                 static_cast<double>(inset), s.window.placeOutsideEye);
        }
        return true;
    }

    ++s.window.placedPairs;
    if (s.placeReports > 0 && s.window.placedPairs == 1) {
        --s.placeReports;
        logf("fast text: GPL's stamped text is placed in the eyes -- set %d at GPL (%d,%d) on a "
             "%dx%d layout became (%.0f,%.0f) in a %dx%d eye at scale %d, inset %.2f, %d px of "
             "convergent disparity for vrGplTextDepth = %.2f. The run's anchor came from %s.",
             set, ax, baseY, layoutWidth, layoutHeight,
             static_cast<double>(static_cast<float>(rect[0][0]) +
                                 0.5f * static_cast<float>(rect[0][2]) + placedX),
             static_cast<double>(static_cast<float>(rect[0][1]) +
                                 0.5f * static_cast<float>(rect[0][3]) + placedY),
             rect[0][2], rect[0][3], g_options.vrGplTextScale, static_cast<double>(inset),
             disparity, static_cast<double>(g_options.vrGplTextDepth),
             run->known ? "the previous frame's extent" : "this glyph alone, for one frame");
    }
    return true;
}

// One line per distinct (set, refusal): which of readStamp's checks gave up on a stamp, and
// everything it had read by then. A block that failed `readable` is described as VirtualQuery
// sees it at both ends, because a block crossing into a second region is refused even when both
// regions are committed and readable.
void reportUnreadable(int handle, int x, int y, int set, int glyph, const StampSource& source) {
    State& s = g_state;
    for (int i = 0; i < s.unreadableSeenCount; ++i) {
        if (s.unreadableSeen[i].set == set && s.unreadableSeen[i].refusal == source.refusal)
            return;
    }
    if (s.unreadableSeenCount >= static_cast<int>(sizeof(s.unreadableSeen) /
                                                  sizeof(s.unreadableSeen[0])))
        return;
    s.unreadableSeen[s.unreadableSeenCount++] = {set, source.refusal};

    logf("fast text: set %d glyph %d at GPL (%d,%d) on handle %d is unreadable -- %s. Read so "
         "far: destination %p class %d %dx%d, set %p, colour %08X, palette %d, glyph bitmap %p "
         "%dx%d format %d, pixels %p pitch %d row length %d.",
         set, glyph, x, y, handle, source.refusal ? source.refusal : "no reason recorded",
         static_cast<const void*>(source.dest), source.destClass, source.destWidth,
         source.destHeight, static_cast<const void*>(source.set), source.colour,
         source.hasPalette, static_cast<const void*>(source.glyph), source.glyphWidth,
         source.glyphHeight, source.glyphFormat, static_cast<const void*>(source.pixels),
         source.pitch, source.rowLength);
    if (!source.refusedAt) return;

    auto describe = [](const void* address, char* text, size_t size) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(address, &info, sizeof(info))) {
            _snprintf_s(text, size, _TRUNCATE, "VirtualQuery failed (%lu)", GetLastError());
            return;
        }
        _snprintf_s(text, size, _TRUNCATE, "region %p + %zu, state %lX, protect %lX",
                    info.BaseAddress, info.RegionSize, info.State, info.Protect);
    };
    auto first = static_cast<const uint8_t*>(source.refusedAt);
    const uint8_t* last = first + (source.refusedBytes ? source.refusedBytes - 1 : 0);
    char firstText[96];
    char lastText[96];
    describe(first, firstText, sizeof(firstText));
    describe(last, lastText, sizeof(lastText));
    logf("fast text:   the refused block is %p + %zu: first byte in %s; last byte in %s",
         source.refusedAt, source.refusedBytes, firstText, lastText);
}

bool handleStamp(int handle, int x, int y, int set, int glyph) {
    State&      s = g_state;
    StampSource source;
    std::memset(&source, 0, sizeof(source));
    ++s.window.calls;
    if (s.sampling) sampleStamp(handle, x, y, set, glyph);
    long long start = now();

    if (!readStamp(handle, set, glyph, source)) {
        ++s.window.fellBackUnreadable;
        reportUnreadable(handle, x, y, set, glyph, source);
        recordProbe(handle, set, glyph, source, kOutcomeUnreadable, 0, 0, 0, 0, 0, 0);
        flushPending();
        s.window.cpuTicks += now() - start;
        return false;
    }
    if (source.destClass != kBlitClassGlFramebuffer) {
        ++s.window.fellBackDestination;
        recordProbe(handle, set, glyph, source, kOutcomeDestination, 0, 0, 0, 0, 0, 0);
        flushPending();
        s.window.cpuTicks += now() - start;
        return false;
    }
    // Formats 0, 1 and 2 are the 8-bit colour-index images rogl_blitSysToGl_drawPixels sends as
    // GL_COLOR_INDEX / GL_UNSIGNED_BYTE with the pixel maps set; everything else is a packed
    // colour and has no index for the atlas to hold.
    if (source.glyphFormat < 0 || source.glyphFormat > 2) {
        ++s.window.fellBackFormat;
        recordProbe(handle, set, glyph, source, kOutcomeFormat, 0, 0, 0, 0, 0, 0);
        flushPending();
        s.window.cpuTicks += now() - start;
        return false;
    }
    if (!ensureGl()) {
        ++s.window.fellBackNoGl;
        recordProbe(handle, set, glyph, source, kOutcomeNoGl, 0, 0, 0, 0, 0, 0);
        s.window.cpuTicks += now() - start;
        return false;
    }

    int drawX = x - source.hotX;
    int drawY = y - source.hotY;
    int width = source.glyphWidth;
    int height = source.glyphHeight;
    int skipX = 0;
    int skipY = 0;
    // GPL's batch must be opened whether or not there is anything left to draw: it is what
    // flushes the polygons GPL submitted before this text.
    openStampBatch();
    if (!clipToBitmap(source.destWidth, source.destHeight, drawX, drawY, width, height, skipX,
                      skipY)) {
        ++s.window.clippedAway;
        recordProbe(handle, set, glyph, source, kOutcomeClippedAway,
                    drawX + source.destOriginX, drawY + source.destOriginY, width, height,
                    skipX, skipY);
        s.window.cpuTicks += now() - start;
        return true;
    }
    const CachedGlyph* cached = findGlyph(source, source.set, glyph);
    if (cached) {
        ++s.window.cacheHits;
    } else {
        cached = cacheGlyph(source, source.set, glyph);
        if (!cached) {
            ++s.window.fellBackAtlasFull;
            recordProbe(handle, set, glyph, source, kOutcomeAtlasFull,
                        drawX + source.destOriginX, drawY + source.destOriginY, width, height,
                        skipX, skipY);
            flushPending();
            s.window.cpuTicks += now() - start;
            return false;
        }
        ++s.window.cacheMisses;
    }

    uint32_t entries[16];
    if (source.hasPalette) {
        buildPalette(source.colour, entries);
    } else {
        // The glyph's own colormap, 0xAARRGGBB, into the texture's R, G, B, A byte order.
        ++s.window.glyphColormaps;
        for (int i = 0; i < 16; ++i) {
            uint32_t c = source.glyphColormap[i];
            entries[i] = ((c >> 16) & 0xFFu) | (c & 0xFF00u) | ((c & 0xFFu) << 16) |
                         (c & 0xFF000000u);
        }
    }
    int row = s.draw.paletteRow(entries);
    if (row < 0) {
        // Every row is spoken for by quads that have not been drawn yet. Draw them, then the
        // rows are free again.
        flushPending();
        s.draw.resetPalettes();
        row = s.draw.paletteRow(entries);
        if (row < 0) {
            s.window.cpuTicks += now() - start;
            return false;
        }
    }

    // Where glDrawPixels would have put these pixels (report 71, section 3, as corrected by
    // 71c).
    //
    // rogl_blitSysToGl_drawPixels sets the raster position to X = drawX + dest.originX,
    // Y = drawY + dest.originY in roglv2's 2D object space. Window y runs up the screen and
    // roglv2's ortho makes object y run down it, so the raster position is Y pixels down from
    // the top. glDrawPixels then lays the image out with the pixel zoom roglv2 set once per
    // display:
    //   - y zoom -1 (what roglv2 asks for): image row j goes to a SMALLER window y, i.e.
    //     further down the screen, i.e. object y = Y + j. The glyph hangs below the raster
    //     position and a top-down source comes out the right way up.
    //   - y zoom +1 (the GL default, which the first build assumed): image row j goes UP the
    //     screen instead, object y = Y - j, and the same source comes out mirrored.
    // So the quad spans object y from Y to Y + zoomed height, v rising with object y when the
    // zoom is negative and falling when it is positive. x is unaffected: the x zoom is 1.
    //
    // Because the quad goes through the same projection, modelview and viewport the raster
    // position does, the two land on the same pixels whatever those are set to.

    // IN VR THE SAME QUAD GOES SOMEWHERE ELSE, TWICE. The transform is the same, so the
    // arithmetic above still says where roglv2 would have put it; placeInEyes takes that
    // rectangle and lays it off from the centre of each eye instead. It declines, by name,
    // whenever this is not a world-mode stereo frame with GPL's own 2D as the destination, and
    // then the untouched quad below is appended exactly as in flat mode.
    //
    // A glyph in GPL's replay bands is not placed: the untouched quad goes to the replay panel's
    // layer, where GPL's own coordinates are the right ones. A batch holds one side or the other.
    bool toPanel = s.vrPlacement && replayPanelCapturing() &&
                   replayPanelTakes(drawX + source.destOriginX, drawY + source.destOriginY,
                                    width, height);
    if (toPanel != s.batchToPanel) {
        flushPending();
        s.batchToPanel = toPanel;
    }
    if (!toPanel && s.vrPlacement && replayPanelCapturing())
        replayPanelNoteDraw("stampDraw", drawX + source.destOriginX, drawY + source.destOriginY,
                            width, height, false, "placed in the eyes instead");
    if (!toPanel &&
        placeInEyes(source, set, drawX + source.destOriginX, drawY + source.destOriginY,
                    y + source.destOriginY, width, height, *cached, skipX, skipY,
                    static_cast<uint32_t>(row))) {
        recordProbe(handle, set, glyph, source, kOutcomeDrawn, drawX + source.destOriginX,
                    drawY + source.destOriginY, width, height, skipX, skipY);
        ++s.window.drawn;
        if (s.draw.pending() >= kMaxBatchQuads) flushPending();
        s.window.cpuTicks += now() - start;
        return true;
    }

    float rasterY = static_cast<float>(drawY + source.destOriginY);
    float span = static_cast<float>(height);
    Quad  quad;
    quad.x0 = static_cast<float>(drawX + source.destOriginX);
    quad.x1 = quad.x0 + static_cast<float>(width);
    quad.yV0 = s.pixelZoomY < 0.0f ? rasterY : rasterY + 1.0f;
    quad.yV1 = s.pixelZoomY < 0.0f ? rasterY + span : quad.yV0 - span;
    quad.u0 = static_cast<float>(cached->atlasX + skipX);
    quad.u1 = quad.u0 + static_cast<float>(width);
    quad.v0 = static_cast<float>(cached->atlasY + skipY);
    quad.v1 = quad.v0 + static_cast<float>(height);
    quad.palette = static_cast<uint32_t>(row);
    recordProbe(handle, set, glyph, source, kOutcomeDrawn, drawX + source.destOriginX,
                drawY + source.destOriginY, width, height, skipX, skipY);
    noteProbeQuad(set, kOutcomeDrawn, quad.yV0, quad.yV1);
    s.draw.append(quad);
    ++s.window.drawn;
    if (s.draw.pending() >= kMaxBatchQuads) flushPending();
    s.window.cpuTicks += now() - start;
    return true;
}

int rasterCall(int kind, int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) {
    State&    s = g_state;
    long long start = postProfileRasterEnter(kind);
    bool      taken = false;
    if (kind == kRasterStampDraw) {
        taken = handleStamp(a0, a1, a2, a3, a4);
        // A GLYPH THIS PATH GAVE BACK IS STILL GPL'S, AND IN A REPLAY BAND IT BELONGS ON THE PANEL.
        // roglv2 draws it itself, so it is drawn with the panel's layer bound: GPL's own pixels
        // at GPL's own place in the layer. Measured 2026-10-14: btnLive's label, "Live Feed", is
        // nine glyphs fast text reads as unreadable, and drawn by roglv2 into the frame they sat
        // in the eye's top left at native size.
        if (!taken && s.vrPlacement && replayPanelCapturing()) {
            int  originX = 0;
            int  originY = 0;
            int  width = 0;
            int  height = 0;
            bool framebuffer = false;
            bool onPanel = fastTextSurfaceInfo(a0, originX, originY, width, height,
                                               framebuffer) &&
                           framebuffer &&
                           replayPanelTakes(originX + a1, originY + a2, 1, 1);
            replayPanelNoteDraw("stampDraw by roglv2", originX + a1, originY + a2, 0, 0, onPanel,
                                "fast text gave it back -- see its `back to roglv2` counts");
            if (onPanel && s.real[kind]) {
                // What fast text has batched goes first, and roglv2's batch is opened before the
                // layer is bound, so the glyph's own prologue flushes nothing into it.
                flushPending();
                openStampBatch();
                int result = 0;
                {
                    ReplayPanelTarget target;
                    result = s.real[kind](a0, a1, a2, a3, a4, a5, a6, a7);
                }
                postProfileRasterLeave(kind, start);
                return result;
            }
        }
    } else {
        flushPending();
    }
    int result = 0;
    if (!taken && s.real[kind]) result = s.real[kind](a0, a1, a2, a3, a4, a5, a6, a7);
    postProfileRasterLeave(kind, start);
    return result;
}

// One thunk per entry: cdecl, so passing more arguments than the callee takes is harmless and
// the caller (us) cleans up. The same shape PostProfile.cpp uses.
template <int Kind>
int __cdecl rasterThunk(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) {
    return rasterCall(Kind, a0, a1, a2, a3, a4, a5, a6, a7);
}

void* thunkFor(int kind) {
    switch (kind) {
    case 0: return reinterpret_cast<void*>(&rasterThunk<0>);
    case 1: return reinterpret_cast<void*>(&rasterThunk<1>);
    case 2: return reinterpret_cast<void*>(&rasterThunk<2>);
    case 3: return reinterpret_cast<void*>(&rasterThunk<3>);
    case 4: return reinterpret_cast<void*>(&rasterThunk<4>);
    case 5: return reinterpret_cast<void*>(&rasterThunk<5>);
    default: return reinterpret_cast<void*>(&rasterThunk<6>);
    }
}

// roglv2's own globals, from the code of the stampDraw the slot points at. The base is the
// loaded module's, asked of the loader, and every operand is checked against the opcode that
// should be holding it and against the module it should point into. `loud` is the first
// attempt and every hundredth after it: the retry runs per frame while the slot is not yet
// something we can read, and a log line a frame would bury the rest of the file.
bool locateRoglv2(void* stampDraw, bool loud) {
    State& s = g_state;
    auto   entry = static_cast<uint8_t*>(stampDraw);
    char   bytes[80];
    if (!entry) return false;

    HMODULE module = moduleOf(entry);
    HMODULE self = moduleOf(reinterpret_cast<const void*>(&kStampDrawPrologue[0]));
    if (module && module == self) {
        if (loud)
            logf("fast text: the stampDraw global holds %p, which is inside gplvr itself -- "
                 "another gplvr hook took the slot first and there is no roglv2 code at it to "
                 "read the globals out of",
                 stampDraw);
        return false;
    }
    if (!module) {
        if (loud)
            logf("fast text: the stampDraw global holds %p, which belongs to no loaded module",
                 stampDraw);
        return false;
    }
    auto base = reinterpret_cast<uint8_t*>(module);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, sizeof(path) - 1);

    if (!readable(entry, 0x70)) {
        if (loud)
            logf("fast text: the stampDraw global holds %p, in %s based at %p, and its first "
                 "0x70 bytes are not readable",
                 stampDraw, path, static_cast<void*>(base));
        return false;
    }
    if (std::memcmp(entry, kStampDrawPrologue, sizeof(kStampDrawPrologue)) != 0) {
        if (loud) {
            formatBytes(entry, bytes, sizeof(bytes));
            logf("fast text: the stampDraw global holds %p (%s, base %p, RVA 0x%X), which "
                 "starts %s -- roglv2's stampDraw starts 55 8B EC 6A FF 68. A thunk, or a "
                 "rasterizer this build does not know.",
                 stampDraw, path, static_cast<void*>(base),
                 static_cast<unsigned>(entry - base), bytes);
        }
        return false;
    }
    if (entry != base + kStampDrawRva) {
        if (loud)
            logf("fast text: stampDraw is at RVA 0x%X in %s (base %p), and every offset this "
                 "build reads is written for 0x%X -- nothing is installed",
                 static_cast<unsigned>(entry - base), path, static_cast<void*>(base),
                 kStampDrawRva);
        return false;
    }

    static const unsigned char kMovEax[] = {0xA1};
    static const unsigned char kMovEsi[] = {0x8B, 0x35};
    static const unsigned char kMovEcx[] = {0x8B, 0x0D};
    static const unsigned char kFldDword[] = {0xD9, 0x05};
    s.roglBase = base;
    s.batchOpen = reinterpret_cast<int32_t*>(
        globalFrom("rogl_batchOpen", entry, kBatchOpenAt, kMovEax, 1, base, loud));
    s.drawer = reinterpret_cast<uint8_t**>(
        globalFrom("rogl_polyDrawerCurrent", entry, kDrawerAt, kMovEsi, 2, base, loud));
    s.handleTable = reinterpret_cast<uint8_t**>(
        globalFrom("rogl_handleTable", entry, kHandleTableAt, kMovEcx, 2, base, loud));
    uint8_t* manager = base + kStampSetManagerRva;
    if (readable(manager, 0x20)) {
        s.stampSetManager = reinterpret_cast<uint8_t**>(
            globalFrom("rogl_stampSetManagerInstance", manager, kStampSetInstanceAt, kMovEax, 1,
                       base, loud));
    } else {
        s.stampSetManager = nullptr;
        if (loud)
            logf("fast text: rogl_stampSetManagerInstance -- rogl_stampSetManager at %p (base "
                 "%p + 0x%X) is not readable",
                 static_cast<void*>(manager), static_cast<void*>(base), kStampSetManagerRva);
    }
    // The pixel zoom, out of rogl_displaySetupGlState. Without it the mapping in
    // quadForStamp is a guess, so a failure here refuses the install rather than drawing
    // something upside down.
    uint8_t* setupState = base + kDisplaySetupGlStateRva;
    uint8_t* zoomY = readable(setupState, kPixelZoomYAt + 8)
                         ? globalFrom("the glPixelZoom y factor", setupState, kPixelZoomYAt,
                                      kFldDword, 2, base, loud)
                         : nullptr;
    if (zoomY) {
        std::memcpy(&s.pixelZoomY, zoomY, sizeof(s.pixelZoomY));
    } else if (loud) {
        logf("fast text: the glPixelZoom y factor did not read out of "
             "rogl_displaySetupGlState at %p (base %p + 0x%X)",
             static_cast<void*>(setupState), static_cast<void*>(base),
             kDisplaySetupGlStateRva);
    }
    if (!s.batchOpen || !s.drawer || !s.handleTable || !s.stampSetManager || !zoomY) {
        if (loud)
            logf("fast text: roglv2's globals did not read out of stampDraw at %p (%s, base %p) "
                 "-- batch %p, drawer %p, handles %p, stamp sets %p. The lines above say which "
                 "read failed and what was at it.",
                 stampDraw, path, static_cast<void*>(base),
                 reinterpret_cast<void*>(s.batchOpen), reinterpret_cast<void*>(s.drawer),
                 reinterpret_cast<void*>(s.handleTable),
                 reinterpret_cast<void*>(s.stampSetManager));
        return false;
    }
    logf("fast text: %s is at %p -- stampDraw %p, batchOpen %p, polyDrawerCurrent %p, "
         "handleTable %p, stampSetManagerInstance %p, drawPixels blit %p, glPixelZoom y %.1f "
         "at %p (%s, so a glyph's first source row is its %s one on screen)",
         path, static_cast<void*>(base), stampDraw, reinterpret_cast<void*>(s.batchOpen),
         reinterpret_cast<void*>(s.drawer), reinterpret_cast<void*>(s.handleTable),
         reinterpret_cast<void*>(s.stampSetManager), static_cast<void*>(base + 0x8C30),
         s.pixelZoomY, static_cast<void*>(zoomY),
         s.pixelZoomY < 0.0f ? "negative" : "positive",
         s.pixelZoomY < 0.0f ? "top" : "bottom");
    return true;
}

void reassert(int kind) {
    State& s = g_state;
    void** slot = s.slot[kind];
    if (!slot || !*slot) return;  // between the clear and the GetProcAddress; try again
    void* thunk = thunkFor(kind);
    if (*slot == thunk) return;
    auto found = reinterpret_cast<RasterFn>(*slot);
    if (!patchBytes(slot, &thunk, sizeof(thunk))) return;
    s.real[kind] = found;
    if (s.reportsLeft > 0) {
        --s.reportsLeft;
        logf("fast text: the %s global had been re-resolved to %p and the thunk was gone -- "
             "re-asserted, and that pointer is the new original.",
             kRasterNames[kind], reinterpret_cast<void*>(found));
    }
}

// Locate roglv2 through the slot as it stands now and, if that works, take the globals this
// path owns -- all seven in flat mode, stampDraw alone in VR.
bool tryInstall(bool loud) {
    State& s = g_state;
    if (!s.slot[kRasterStampDraw] || !*s.slot[kRasterStampDraw]) return false;
    if (!locateRoglv2(*s.slot[kRasterStampDraw], loud)) return false;
    for (int kind = 0; kind < kRasterKinds; ++kind) {
        if (!s.owns[kind]) continue;
        if (!s.slot[kind] || !*s.slot[kind]) return false;
        s.real[kind] = reinterpret_cast<RasterFn>(*s.slot[kind]);
        void* thunk = thunkFor(kind);
        if (patchBytes(s.slot[kind], &thunk, sizeof(thunk))) continue;
        logf("fast text: the %s global could not be written -- nothing is installed",
             kRasterNames[kind]);
        // Put back whatever was already patched: a half-installed chain would drop calls.
        for (int undo = 0; undo < kind; ++undo) {
            if (!s.owns[undo]) continue;
            void* original = reinterpret_cast<void*>(s.real[undo]);
            patchBytes(s.slot[undo], &original, sizeof(original));
        }
        return false;
    }
    s.installed = true;
    s.pending = false;
    if (s.vrPlacement) {
        logf("fast text: installed on the stampDraw global alone (%p), because in VR Hud.cpp "
             "holds bitmapDraw for the mirror panes' copy offset. The batch is flushed from "
             "Hud.cpp's own thunks instead, so the ordering contract is unchanged: anything "
             "GPL draws after a run of text still lands over it. GPL's stamped glyphs are "
             "drawn from a GPU atlas, once per eye, at vrGplTextScale and vrGplTextSpread.",
             reinterpret_cast<void*>(s.slot[kRasterStampDraw]));
    } else {
        logf("fast text: installed on all seven 2D globals (stampDraw at %p). GPL's stamped "
             "glyphs are drawn from a GPU atlas as one batched quad list; the other six flush "
             "that list first, so nothing changes order. No setting in flat mode: it is what "
             "this path does.",
             reinterpret_cast<void*>(s.slot[kRasterStampDraw]));
    }
    return true;
}

}  // namespace

void fastTextBind(const Binding& binding) {
    State& s = g_state;
    if (s.bound) return;
    s.bound = true;
    // WHICH GLOBALS THIS PATH TAKES, and why VR is not all of them.
    //
    // The other six exist here to FLUSH the pending quad batch before anything else draws, so
    // text issued before a polygon is on the framebuffer before it. In VR Hud.cpp already holds
    // every one of them -- bitmapDraw carries the mirror panes' copy offset, which is the only
    // place it can be applied -- and both modules re-assert their thunk over whatever is in a
    // slot and adopt what they find as "the original". Installing both on one slot therefore
    // makes each one the other's original, and the chain calls itself for ever. Hud.cpp calls
    // fastTextFlushNow() from its own thunks instead, which honours the same contract with one
    // owner per slot.
    s.vrPlacement = g_options.vr;
    for (int kind = 0; kind < kRasterKinds; ++kind)
        s.owns[kind] = !s.vrPlacement || kind == kRasterStampDraw;

    for (int kind = 0; kind < kRasterKinds; ++kind) {
        if (!s.owns[kind]) continue;
        s.slot[kind] = rasterGlobal(binding.image, kRasterNames[kind]);
        if (s.slot[kind] && *s.slot[kind]) continue;
        logf("fast text: the %s global did not resolve -- nothing is installed",
             kRasterNames[kind]);
        return;
    }
    // The slots hold what they hold now, which may still be a rasterizer that has not finished
    // resolving its own exports. The frame poll keeps trying.
    s.pending = true;
    if (!tryInstall(true))
        logf("fast text: not installed yet -- the frame poll retries, and says so if it takes");
}

bool fastTextOwnsRasterEntries() { return g_state.installed || g_state.pending; }

void fastTextPollFrame() {
    State& s = g_state;
    // The frame the report asked for, armed at the top of a frame so the stamps that follow are
    // inside it and fastTextEndFrame closes it at the end of the same one.
    if (s.wantSample) {
        s.wantSample = false;
        s.sampling = true;
    }
    if (!s.installed) {
        if (!s.pending) return;
        ++s.attempts;
        if (tryInstall(s.attempts % kLoudEvery == 0)) {
            logf("fast text: that took %u more frames after the first try", s.attempts);
        } else if (s.attempts >= kMaxLocateAttempts) {
            s.pending = false;
            logf("fast text: gave up after %u frames -- GPL's stamped text stays roglv2's, and "
                 "the seven 2D globals are left to whoever else wants them",
                 s.attempts);
        }
        return;
    }
    for (int kind = 0; kind < kRasterKinds; ++kind)
        if (s.owns[kind]) reassert(kind);

    // The fonts are reloaded around a mode change, so a cached glyph bitmap pointer could be a
    // different glyph afterwards. The atlas is dropped whenever the engine crosses between the
    // front end and a world mode, which is where that happens.
    bool inFrontEnd = windowInFrontEnd();
    if (inFrontEnd != s.wasFrontEnd) {
        s.wasFrontEnd = inFrontEnd;
        // The run extents go with the atlas. They are keyed on a stamp set and a row in GPL's
        // layout, and a mode change rebuilds both the fonts and the layout, so an extent
        // carried across it would place the first frame of the new mode from the old one's.
        s.runCount = 0;
        // A fresh detail budget for the phase under test, so a race gets its own lines rather
        // than finding them spent on the front end -- the failure three instruments in Hud.cpp
        // made before this one.
        s.placeReports = 4;
        if (s.glReady) {
            flushPending();
            s.draw.resetAtlas();
            s.glyphs.clear();
            logf("fast text: the engine changed mode -- the glyph atlas is dropped and rebuilt");
        }
    }

}

void fastTextEndFrame() {
    State& s = g_state;
    if (!s.installed) return;
    flushPending();
    ++s.window.frames;
    // After the draw, so a run's extent is complete: it becomes the placement the NEXT frame's
    // first glyph is put by, which is how a line can be centred before its length is known.
    if (s.vrPlacement) {
        rollRuns();
        separateRuns();
    }
    s.sampling = false;  // one frame a second is enough to say what is being stamped
}

void fastTextFlushNow() {
    if (!g_state.installed) return;
    flushPending();
}

bool fastTextSurfaceInfo(int handle, int& originX, int& originY, int& width, int& height,
                         bool& framebuffer) {
    State& s = g_state;
    if (handle <= 0 || !s.handleTable) return false;
    __try {
        uint8_t* table = *s.handleTable;
        if (!table || !readable(table, 12)) return false;
        uint8_t* entries = readPointer(table, 8);
        if (!entries || !readable(entries + 8 * static_cast<unsigned>(handle), 8)) return false;
        uint8_t* bitmap = readPointer(entries, 8 * static_cast<unsigned>(handle));
        if (!bitmap || !readable(bitmap, 0x2C)) return false;
        originX = readInt(bitmap, kBitmapOriginX);
        originY = readInt(bitmap, kBitmapOriginY);
        width = readInt(bitmap, kBitmapWidth);
        height = readInt(bitmap, kBitmapHeight);
        framebuffer = readInt(bitmap, kBitmapClass) == kBlitClassGlFramebuffer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void fastTextOpenRasterBatch() {
    if (!g_state.installed) return;
    openStampBatch();
}

void fastTextReport() {
    State& s = g_state;
    if (!s.installed) return;
    Counters& c = s.window;
    if (!c.frames) return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    double perFrame = 1000.0 / static_cast<double>(frequency.QuadPart) / c.frames;
    unsigned fellBack = c.fellBackDestination + c.fellBackFormat + c.fellBackUnreadable +
                        c.fellBackAtlasFull + c.fellBackNoGl;
    logf("fast text: %.1f stampDraw calls a frame, %.1f drawn by us in %.1f draws "
         "(cache %u hit, %u uploaded, %u in the glyph's own colormap), %.1f clipped away, %.1f "
         "back to roglv2 (destination not the framebuffer %u, glyph format %u, unreadable %u, "
         "atlas full %u, no GL %u); atlas %dx%d holding %zu glyphs, %u uploads of %u KiB, %u "
         "palette uploads; cpu %.3f ms a frame (%.3f of it in the draw, %.3f in uploads)",
         static_cast<double>(c.calls) / c.frames,
         static_cast<double>(c.drawn) / c.frames,
         static_cast<double>(s.drawStats.draws) / c.frames, c.cacheHits, c.cacheMisses,
         c.glyphColormaps, static_cast<double>(c.clippedAway) / c.frames,
         static_cast<double>(fellBack) / c.frames, c.fellBackDestination, c.fellBackFormat,
         c.fellBackUnreadable,
         c.fellBackAtlasFull, c.fellBackNoGl, fasttext::kAtlasSize, fasttext::kAtlasSize,
         s.glyphs.size(), s.drawStats.glyphUploads, s.drawStats.glyphBytes / 1024u,
         s.drawStats.paletteUploads, c.cpuTicks * perFrame, s.drawStats.drawTicks * perFrame,
         s.drawStats.uploadTicks * perFrame);
    // THE PLACEMENT'S OWN LINE, and only in VR, because in flat mode every term below is
    // structurally zero and a line of zeroes is a line nobody reads. `placed` is the number
    // that says whether vrGplTextScale and vrGplTextSpread reached anything at all: zero with a
    // non-zero `drawn` means every glyph went where GPL put it, and one of the refusals says
    // which term decided that.
    if (s.vrPlacement) {
        logf("fast text: placement -- %.1f glyphs a frame placed once per eye at scale %d, "
             "inset %.2f, over %d live run(s), %d of them moved down to clear the run above; "
             "refused: front end %u, no eye rects %u, no "
             "layout size %u, destination off GPL's 2D frame %u, outside the eye rect %u; %u "
             "glyphs placed from their own position because the run was new, %u run sightings "
             "past the %d-entry table",
             static_cast<double>(c.placedPairs) / c.frames, g_options.vrGplTextScale,
             static_cast<double>(g_options.vrGplTextSpread), s.runCount, s.runsPushed,
             c.placeFrontEnd,
             c.placeNoEyeRects, c.placeNoLayout, c.placeOffLayout, c.placeOutsideEye,
             c.placeFirstSight, c.placeRunTableFull, kMaxPlacedRuns);
    }
    for (int i = 0; i < s.sampleCount; ++i) {
        const SampleRun& run = s.samples[i];
        logf("fast text: stamped on surface %d, set %d, at %d,%d: \"%s\"%s", run.dest, run.set,
             run.x, run.y, run.text, run.length >= kMaxSampleChars ? " ..." : "");
    }
    // One of each kind of stamp, with everything a flip or a misplacement can be read off.
    // `rows` is the band of destination rows the glyph covers and which source row is at the
    // near end of it; with the pixel zoom negative that near end is the glyph's FIRST row and
    // the band runs downwards, which is what 71c corrected.
    for (int i = 0; i < s.probeCount; ++i) {
        const StampProbe& p = s.probes[i];
        if (!p.used) continue;
        int first = p.rasterY;
        int last = p.rasterY + p.height - 1;
        if (s.pixelZoomY >= 0.0f) {
            first = p.rasterY + 1;
            last = p.rasterY + 2 - p.height;
        }
        char ours[64] = "not drawn";
        if (p.haveQuad)
            std::snprintf(ours, sizeof(ours), "object y %.1f (v0) to %.1f (v1)", p.quadV0,
                          p.quadV1);
        logf("fast text: set %d glyph %d -- %s. Glyph %dx%d fmt %d, pitch %d (%s), row length "
             "%d, sub-rect origin %d,%d, hot spot %d,%d, colour %08X. Destination %d class %d "
             "%dx%d origin %d,%d. Raster %d,%d; after the clip %dx%d skipping %d,%d. Zoom y "
             "%.1f, so roglv2 writes rows %d..%d with source row %d at %d; ours spans %s.",
             p.set, p.glyph, kOutcomeNames[p.outcome], p.glyphWidth, p.glyphHeight,
             p.glyphFormat, p.pitch, p.pitch < 0 ? "NEGATIVE, bottom-up" : "positive, top-down",
             p.rowLength, p.srcOriginX, p.srcOriginY, p.hotX, p.hotY, p.colour, p.dest,
             p.destClass, p.destWidth, p.destHeight, p.destOriginX, p.destOriginY, p.rasterX,
             p.rasterY, p.width, p.height, p.skipX, p.skipY, s.pixelZoomY, first, last,
             p.skipY, first, ours);
    }
    s.sampleCount = 0;
    s.probeCount = 0;
    // ARMED BY THE NEXT FRAME'S POLL, NOT HERE. This report runs from the present hook and
    // fastTextEndFrame runs later in that same hook, so arming here was undone before a single
    // stamp of the next frame could be seen: the census and the probe lines never once printed,
    // and a log with neither in it reads exactly like a frame with no text in it.
    s.wantSample = true;
    c = Counters{};
    s.drawStats = fasttext::DrawStats{};
}

}  // namespace gplvr
