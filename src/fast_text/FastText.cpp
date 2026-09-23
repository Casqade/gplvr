#include "fast_text/FastText.h"

#include "Discovery.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"
#include "PostProfile.h"
#include "Window.h"
#include "fast_text/TextDraw.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

typedef int(__cdecl* RasterFn)(int, int, int, int, int, int, int, int);
typedef int(__thiscall* PixelLockFn)(void* self);
typedef int(__fastcall* DrawerSlotFn)(void* self, void* argument);

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
};

// One frame a second is sampled for WHAT is being stamped, because the cost only matters if it
// is on screen while the user is driving. The 2026-09-14 string census found the only in-race
// stamp text at spa67 to be GPL's "PRESS PAUSE TO RESUME", so a line naming the runs is what
// separates "the HUD costs 5.78 ms" from "the pause message costs 5.78 ms". A glyph joins the
// run before it when the destination, the set and the row match and it sits a little to the
// right; the code is printed as a character when it is printable, which these fonts' codes are
// (a set with a sub-pixel phase adds 0..3 to it).
constexpr int kMaxSampleRuns = 4;
constexpr int kMaxSampleChars = 40;

struct SampleRun {
    int  dest = 0;
    int  set = 0;
    int  x = 0;
    int  y = 0;
    int  lastX = 0;
    int  length = 0;
    char text[kMaxSampleChars + 1] = {};
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
    "fastText is off",   "unreadable",
    "destination is not the GL framebuffer",
    "the set carries no palette",
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
    unsigned fellBackNoPalette = 0;
    unsigned fellBackFormat = 0;
    unsigned fellBackUnreadable = 0;
    unsigned fellBackAtlasFull = 0;
    unsigned fellBackNoGl = 0;
    unsigned frames = 0;
    long long cpuTicks = 0;
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

    Counters                 window;
    fasttext::DrawStats      drawStats;
    bool                     keyLast = false;
    bool                     loggedScale = false;
    int                      reportsLeft = 6;
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
bool readStamp(int handle, int set, int glyph, StampSource& out) {
    State& s = g_state;
    std::memset(&out, 0, sizeof(out));
    if (handle <= 0 || set < 0 || glyph < 0) return false;
    if (!s.handleTable || !s.stampSetManager) return false;
    __try {
        uint8_t* table = *s.handleTable;
        if (!table || !readable(table, 12)) return false;
        uint8_t* entries = readPointer(table, 8);
        if (!entries || !readable(entries + 8 * static_cast<unsigned>(handle), 8)) return false;
        out.dest = readPointer(entries, 8 * static_cast<unsigned>(handle));
        if (!out.dest || !readable(out.dest, 0x2C)) return false;
        out.destWidth = readInt(out.dest, kBitmapWidth);
        out.destHeight = readInt(out.dest, kBitmapHeight);
        out.destClass = readInt(out.dest, kBitmapClass);
        out.destOriginX = readInt(out.dest, kBitmapOriginX);
        out.destOriginY = readInt(out.dest, kBitmapOriginY);

        uint8_t* manager = *s.stampSetManager;
        if (!manager || !readable(manager, 12)) return false;
        uint8_t* sets = readPointer(manager, 8);
        if (!sets || !readable(sets + 8 * static_cast<unsigned>(set), 8)) return false;
        uint8_t* setObject = readPointer(sets, 8 * static_cast<unsigned>(set));
        if (!setObject || !readable(setObject, 0x44)) return false;
        out.set = setObject;
        out.hasPalette = readPointer(setObject, kSetPalette) != nullptr ? 1 : 0;
        uint8_t* holder = readPointer(setObject, kSetColourHolder);
        if (!holder || !readable(holder, kColourInHolder + 4)) return false;
        out.colour = static_cast<uint32_t>(readInt(holder, kColourInHolder));

        uint8_t* records = readPointer(setObject, kSetGlyphs);
        if (!records) return false;
        uint8_t* record = records + kGlyphStride * static_cast<unsigned>(glyph);
        if (!readable(record, kGlyphStride)) return false;
        out.hotX = readInt(record, kGlyphOriginX);
        out.hotY = readInt(record, kGlyphOriginY);
        out.glyph = readPointer(record, kGlyphBitmap);
        if (!out.glyph || !readable(out.glyph, 0x2C)) return false;
        out.glyphWidth = readInt(out.glyph, kBitmapWidth);
        out.glyphHeight = readInt(out.glyph, kBitmapHeight);
        out.glyphFormat = readInt(out.glyph, kBitmapFormat);
        out.glyphOriginX = readInt(out.glyph, kBitmapOriginX);
        out.glyphOriginY = readInt(out.glyph, kBitmapOriginY);
        out.pixels = readPointer(out.glyph, kBitmapPixels);
        if (!out.pixels || !readable(out.pixels, 0x20)) return false;
        out.pitch = readInt(out.pixels, kPixelsPitch);
        out.rowLength = readInt(out.pixels, kPixelsRowLength);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (out.glyphWidth <= 0 || out.glyphHeight <= 0) return false;
    if (out.glyphWidth > fasttext::kMaxGlyphSide || out.glyphHeight > fasttext::kMaxGlyphSide)
        return false;
    if (out.pitch <= 0) return false;
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
                       "not be. Turn fastText off and say so.");
        }
        float screen[16];
        screenMatrix(width, height, readInt(display, kDisplayOriginX),
                     readInt(display, kDisplayOriginY), screen);
        s.draw.draw(screen, s.drawStats);
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
    if (run->length < kMaxSampleChars) {
        run->text[run->length++] =
            (glyph >= 32 && glyph < 127) ? static_cast<char>(glyph) : '?';
        run->text[run->length] = 0;
    }
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
bool handleStamp(int handle, int x, int y, int set, int glyph) {
    State&      s = g_state;
    StampSource source;
    std::memset(&source, 0, sizeof(source));
    ++s.window.calls;
    if (s.sampling) sampleStamp(handle, x, y, set, glyph);
    if (!g_options.fastText) {
        recordProbe(handle, set, glyph, source, kOutcomeOptionOff, 0, 0, 0, 0, 0, 0);
        flushPending();
        return false;
    }
    long long start = now();

    if (!readStamp(handle, set, glyph, source)) {
        ++s.window.fellBackUnreadable;
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
    if (!source.hasPalette) {
        ++s.window.fellBackNoPalette;
        recordProbe(handle, set, glyph, source, kOutcomeNoPalette, 0, 0, 0, 0, 0, 0);
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
    buildPalette(source.colour, entries);
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
    if (kind == kRasterStampDraw)
        taken = handleStamp(a0, a1, a2, a3, a4);
    else
        flushPending();
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

// Locate roglv2 through the slot as it stands now and, if that works, take all seven globals.
bool tryInstall(bool loud) {
    State& s = g_state;
    if (!s.slot[kRasterStampDraw] || !*s.slot[kRasterStampDraw]) return false;
    if (!locateRoglv2(*s.slot[kRasterStampDraw], loud)) return false;
    for (int kind = 0; kind < kRasterKinds; ++kind) {
        if (!s.slot[kind] || !*s.slot[kind]) return false;
        s.real[kind] = reinterpret_cast<RasterFn>(*s.slot[kind]);
        void* thunk = thunkFor(kind);
        if (patchBytes(s.slot[kind], &thunk, sizeof(thunk))) continue;
        logf("fast text: the %s global could not be written -- nothing is installed",
             kRasterNames[kind]);
        // Put back whatever was already patched: a half-installed chain would drop calls.
        for (int undo = 0; undo < kind; ++undo) {
            void* original = reinterpret_cast<void*>(s.real[undo]);
            patchBytes(s.slot[undo], &original, sizeof(original));
        }
        return false;
    }
    s.installed = true;
    s.pending = false;
    logf("fast text: installed on all seven 2D globals (stampDraw at %p). GPL's stamped glyphs "
         "are drawn from a GPU atlas as one batched quad list; the other six flush that list "
         "first, so nothing changes order. fastText = %d, key 0x%02X.",
         reinterpret_cast<void*>(s.slot[kRasterStampDraw]), g_options.fastText ? 1 : 0,
         g_options.fastTextKey);
    return true;
}

}  // namespace

void fastTextBind(const Binding& binding) {
    State& s = g_state;
    if (s.bound) return;
    s.bound = true;
    if (g_options.vr) {
        logf("fast text: not installed -- in VR the seven 2D globals belong to Hud.cpp, which "
             "re-issues every stamp once per eye");
        return;
    }
    if (!g_options.fastText && !g_options.fastTextKey) {
        logf("fast text: fastText = 0 and no key -- GPL's stamped text stays roglv2's");
        return;
    }
    for (int kind = 0; kind < kRasterKinds; ++kind) {
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
    for (int kind = 0; kind < kRasterKinds; ++kind) reassert(kind);

    // The fonts are reloaded around a mode change, so a cached glyph bitmap pointer could be a
    // different glyph afterwards. The atlas is dropped whenever the engine crosses between the
    // front end and a world mode, which is where that happens.
    bool inFrontEnd = windowInFrontEnd();
    if (inFrontEnd != s.wasFrontEnd) {
        s.wasFrontEnd = inFrontEnd;
        if (s.glReady) {
            flushPending();
            s.draw.resetAtlas();
            s.glyphs.clear();
            logf("fast text: the engine changed mode -- the glyph atlas is dropped and rebuilt");
        }
    }

    if (g_options.fastTextKey) {
        bool down = (GetAsyncKeyState(g_options.fastTextKey) & 0x8000) != 0;
        if (down && !s.keyLast) {
            g_options.fastText = !g_options.fastText;
            flushPending();
            logf("fast text: %s", g_options.fastText
                                      ? "ON -- glyphs come from our atlas, one draw a run"
                                      : "OFF -- roglv2 draws every glyph with glDrawPixels");
        }
        s.keyLast = down;
    }
}

void fastTextEndFrame() {
    State& s = g_state;
    if (!s.installed) return;
    flushPending();
    ++s.window.frames;
    s.sampling = false;  // one frame a second is enough to say what is being stamped
}

void fastTextFlushNow() {
    if (!g_state.installed) return;
    flushPending();
}

void fastTextReport() {
    State& s = g_state;
    if (!s.installed) return;
    Counters& c = s.window;
    if (!c.frames) return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    double perFrame = 1000.0 / static_cast<double>(frequency.QuadPart) / c.frames;
    unsigned fellBack = c.fellBackDestination + c.fellBackNoPalette + c.fellBackFormat +
                        c.fellBackUnreadable + c.fellBackAtlasFull + c.fellBackNoGl;
    logf("fast text: %s -- %.1f stampDraw calls a frame, %.1f drawn by us in %.1f draws "
         "(cache %u hit, %u uploaded), %.1f clipped away, %.1f back to roglv2 (destination not "
         "the framebuffer %u, no palette %u, glyph format %u, unreadable %u, atlas full %u, no "
         "GL %u); atlas %dx%d holding %zu glyphs, %u uploads of %u KiB, %u palette uploads; "
         "cpu %.3f ms a frame (%.3f of it in the draw, %.3f in uploads)",
         g_options.fastText ? "ON" : "OFF", static_cast<double>(c.calls) / c.frames,
         static_cast<double>(c.drawn) / c.frames,
         static_cast<double>(s.drawStats.draws) / c.frames, c.cacheHits, c.cacheMisses,
         static_cast<double>(c.clippedAway) / c.frames, static_cast<double>(fellBack) / c.frames,
         c.fellBackDestination, c.fellBackNoPalette, c.fellBackFormat, c.fellBackUnreadable,
         c.fellBackAtlasFull, c.fellBackNoGl, fasttext::kAtlasSize, fasttext::kAtlasSize,
         s.glyphs.size(), s.drawStats.glyphUploads, s.drawStats.glyphBytes / 1024u,
         s.drawStats.paletteUploads, c.cpuTicks * perFrame, s.drawStats.drawTicks * perFrame,
         s.drawStats.uploadTicks * perFrame);
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
    s.sampling = true;
    c = Counters{};
    s.drawStats = fasttext::DrawStats{};
}

}  // namespace gplvr
