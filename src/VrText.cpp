#include "VrText.h"

#include "Log.h"

#include <cstring>

namespace gplvr {
namespace {

// The printable ASCII range, which is the whole of what a menu built from option names
// needs. A character outside it draws the empty list at that index, which is nothing --
// deliberately, rather than a box: a missing glyph in a label is a cosmetic fault and
// must not become a stripe of noise across the panel.
constexpr int kFirstChar = 32;
constexpr int kCharCount = 96;

typedef BOOL(WINAPI* WglUseFontBitmapsAFn)(HDC, DWORD, DWORD, DWORD);
typedef GLuint(WINAPI* GlGenListsFn)(GLsizei);
typedef void(WINAPI* GlDeleteListsFn)(GLuint, GLsizei);
typedef void(WINAPI* GlListBaseFn)(GLuint);
typedef void(WINAPI* GlCallListsFn)(GLsizei, GLenum, const GLvoid*);
typedef void(WINAPI* GlRasterPos2fFn)(GLfloat, GLfloat);

struct Entries {
    bool                 resolved = false;
    bool                 reported = false;
    WglUseFontBitmapsAFn useFontBitmaps = nullptr;
    GlGenListsFn         genLists = nullptr;
    GlDeleteListsFn      deleteLists = nullptr;
    GlListBaseFn         listBase = nullptr;
    GlCallListsFn        callLists = nullptr;
    GlRasterPos2fFn      rasterPos2f = nullptr;
};

struct Font {
    GLuint base = 0;
    int    height = 0;
    int    ascent = 0;
    int    builtFor = 0;             // the pixel height these lists were built at
    HGLRC  context = nullptr;        // the context they belong to
    int    widths[kCharCount] = {};  // advance per character, in pixels
};

// The fonts are kept by pixel size, and a slot names the one it was last asked for. Every size
// the menus draw at (vrTextPrebuild) then stays built side by side, so moving between the VR
// menu, its tweak strips and the radials only re-points a slot and builds nothing.
constexpr int kFontPool = 4;

Entries g_entries;
Font    g_pool[kFontPool];
int     g_slotFont[kVrTextSlots] = {-1, -1};
bool    g_reportedFailure = false;

const Font* slotFont(int slot) {
    int index = g_slotFont[slot];
    return index >= 0 && g_pool[index].base ? &g_pool[index] : nullptr;
}

bool resolveEntries(const GlFunctions& gl) {
    if (g_entries.resolved) return true;
    if (!gl.module) return false;

    // Straight out of opengl32 by name. Every one of these is a core GL 1.1 or WGL export
    // of that DLL, so wglGetProcAddress -- which serves extensions and returns null for
    // core entry points on some drivers -- is the wrong door.
    g_entries.useFontBitmaps = reinterpret_cast<WglUseFontBitmapsAFn>(
        GetProcAddress(gl.module, "wglUseFontBitmapsA"));
    g_entries.genLists =
        reinterpret_cast<GlGenListsFn>(GetProcAddress(gl.module, "glGenLists"));
    g_entries.deleteLists =
        reinterpret_cast<GlDeleteListsFn>(GetProcAddress(gl.module, "glDeleteLists"));
    g_entries.listBase =
        reinterpret_cast<GlListBaseFn>(GetProcAddress(gl.module, "glListBase"));
    g_entries.callLists =
        reinterpret_cast<GlCallListsFn>(GetProcAddress(gl.module, "glCallLists"));
    g_entries.rasterPos2f =
        reinterpret_cast<GlRasterPos2fFn>(GetProcAddress(gl.module, "glRasterPos2f"));

    g_entries.resolved = g_entries.useFontBitmaps && g_entries.genLists &&
                         g_entries.deleteLists && g_entries.listBase &&
                         g_entries.callLists && g_entries.rasterPos2f;
    if (!g_entries.resolved && !g_entries.reported) {
        g_entries.reported = true;
        logf("vr text: opengl32 did not give up %s%s%s%s%s%s-- there is no menu text",
             g_entries.useFontBitmaps ? "" : "wglUseFontBitmapsA ",
             g_entries.genLists ? "" : "glGenLists ",
             g_entries.deleteLists ? "" : "glDeleteLists ",
             g_entries.listBase ? "" : "glListBase ",
             g_entries.callLists ? "" : "glCallLists ",
             g_entries.rasterPos2f ? "" : "glRasterPos2f ");
    }
    return g_entries.resolved;
}

bool validSlot(int slot) { return slot >= 0 && slot < kVrTextSlots; }

bool slotUses(int index) {
    for (int slot = 0; slot < kVrTextSlots; ++slot)
        if (g_slotFont[slot] == index) return true;
    return false;
}

// The pool entry holding `pixelHeight` in the current context, built when there is none. -1
// with `why` set when it cannot be had.
int fontFor(const GlFunctions& gl, int pixelHeight, const char*& why) {
    why = nullptr;
    if (!resolveEntries(gl)) {
        why = "opengl32 did not give up the font calls (see the vr text: line)";
        return -1;
    }
    if (!gl.wglGetCurrentContext || !gl.wglGetCurrentDC) {
        why = "wglGetCurrentContext or wglGetCurrentDC did not resolve";
        return -1;
    }

    if (pixelHeight < 8) pixelHeight = 8;
    if (pixelHeight > 96) pixelHeight = 96;

    HGLRC context = gl.wglGetCurrentContext();
    if (!context) {
        why = "no GL context is current";
        return -1;
    }

    // A context change means the old lists are somebody else's now. Forget them rather
    // than deleting names that no longer belong to us.
    for (Font& font : g_pool)
        if (font.base && font.context != context) font = Font{};
    for (int index = 0; index < kFontPool; ++index)
        if (g_pool[index].base && g_pool[index].builtFor == pixelHeight) return index;

    // An empty entry, else one no slot is pointing at.
    int target = -1;
    for (int index = 0; index < kFontPool && target < 0; ++index)
        if (!g_pool[index].base) target = index;
    for (int index = 0; index < kFontPool && target < 0; ++index)
        if (!slotUses(index)) target = index;
    if (target < 0) target = 0;
    Font& font = g_pool[target];
    if (font.base) {
        logf("vr text: all %d fonts are in use -- the %d px one is dropped for %d px",
             kFontPool, font.builtFor, pixelHeight);
        g_entries.deleteLists(font.base, kCharCount);
        for (int slot = 0; slot < kVrTextSlots; ++slot)
            if (g_slotFont[slot] == target) g_slotFont[slot] = -1;
        font = Font{};
    }

    HDC dc = gl.wglGetCurrentDC();
    if (!dc) {
        why = "the GL context has no device context";
        return -1;
    }

    // Monospaced on purpose: a menu laid out in columns is arithmetic rather than
    // measurement, and a fallback substitution by GDI still keeps the metrics this asks
    // for. NONANTIALIASED, because wglUseFontBitmaps produces 1-bit bitmaps and asking for
    // smoothing it cannot deliver only changes which glyph shapes GDI picks.
    HFONT created = CreateFontA(-pixelHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                                "Consolas");
    if (!created) {
        why = "CreateFontA refused";
        if (!g_reportedFailure) {
            g_reportedFailure = true;
            logf("vr text: CreateFontA refused (error %lu) -- there is no menu text",
                 GetLastError());
        }
        return -1;
    }

    HGDIOBJ previous = SelectObject(dc, created);

    TEXTMETRICA metrics = {};
    GetTextMetricsA(dc, &metrics);
    GetCharWidth32A(dc, kFirstChar, kFirstChar + kCharCount - 1, font.widths);

    GLuint base = g_entries.genLists(kCharCount);
    bool   built = false;
    if (base) {
        built = g_entries.useFontBitmaps(dc, kFirstChar, kCharCount, base) != FALSE;
        if (!built) g_entries.deleteLists(base, kCharCount);
    }

    SelectObject(dc, previous);
    DeleteObject(created);

    if (!built) {
        why = base ? "wglUseFontBitmapsA refused the font" : "glGenLists gave no names";
        if (!g_reportedFailure) {
            g_reportedFailure = true;
            logf("vr text: %s -- there is no menu text", why);
        }
        return -1;
    }

    font.base = base;
    font.builtFor = pixelHeight;
    font.context = context;
    font.height = metrics.tmHeight;
    font.ascent = metrics.tmAscent;
    g_reportedFailure = false;
    logf("vr text: %d px font built (cell %d, ascent %d)", pixelHeight, font.height,
         font.ascent);
    return target;
}

}  // namespace

bool vrTextEnsure(const GlFunctions& gl, int slot, int pixelHeight) {
    if (!validSlot(slot)) return false;
    const char* why = nullptr;
    int         index = fontFor(gl, pixelHeight, why);
    if (index < 0) return false;
    g_slotFont[slot] = index;
    return true;
}

bool vrTextPrebuild(const GlFunctions& gl, int pixelHeight, const char*& why) {
    return fontFor(gl, pixelHeight, why) >= 0;
}

int vrTextBuiltCount() {
    int count = 0;
    for (const Font& font : g_pool)
        if (font.base) ++count;
    return count;
}

void vrTextRelease(const GlFunctions& gl, bool contextAlive) {
    for (Font& font : g_pool) {
        if (font.base && contextAlive && g_entries.deleteLists)
            g_entries.deleteLists(font.base, kCharCount);
        font = Font{};
    }
    for (int& index : g_slotFont) index = -1;
    (void)gl;
    (void)contextAlive;
}

int vrTextHeight(int slot) {
    const Font* font = validSlot(slot) ? slotFont(slot) : nullptr;
    return font ? font->height : 0;
}

int vrTextAscent(int slot) {
    const Font* font = validSlot(slot) ? slotFont(slot) : nullptr;
    return font ? font->ascent : 0;
}

int vrTextWidth(int slot, const char* text) {
    const Font* found = validSlot(slot) ? slotFont(slot) : nullptr;
    if (!found || !text) return 0;
    const Font& font = *found;
    int         width = 0;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
        const int index = static_cast<int>(*c) - kFirstChar;
        if (index < 0 || index >= kCharCount) continue;
        width += font.widths[index];
    }
    return width;
}

void vrTextDraw(const GlFunctions& gl, int slot, float x, float baselineY,
                const char* text) {
    const Font* font = validSlot(slot) ? slotFont(slot) : nullptr;
    if (!font || !text || !*text) return;
    if (!gl.pixelStorei || !gl.getIntegerv) return;

    const size_t length = strlen(text);
    if (!length) return;

    // The unpack state glBitmap reads. glPushAttrib does NOT save it -- it is client
    // state, under glPushClientAttrib -- so it is saved and restored by hand. The lists
    // wglUseFontBitmapsA built assume the GL defaults, and roglv2 sets its own alignment
    // for glDrawPixels, so leaving whatever it left would shear every glyph.
    GLint alignment = 4;
    GLint rowLength = 0;
    GLint skipRows = 0;
    GLint skipPixels = 0;
    GLint lsbFirst = 0;
    gl.getIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    gl.getIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    gl.getIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    gl.getIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    gl.getIntegerv(GL_UNPACK_LSB_FIRST, &lsbFirst);

    gl.pixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.pixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.pixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl.pixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.pixelStorei(GL_UNPACK_LSB_FIRST, 0);

    g_entries.rasterPos2f(x, baselineY);
    // The lists are indexed from kFirstChar, so the base is shifted and the string is
    // passed as its own bytes -- one call for the whole line.
    g_entries.listBase(font->base - kFirstChar);
    g_entries.callLists(static_cast<GLsizei>(length), GL_UNSIGNED_BYTE, text);
    g_entries.listBase(0);

    gl.pixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    gl.pixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    gl.pixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    gl.pixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
    gl.pixelStorei(GL_UNPACK_LSB_FIRST, lsbFirst);
}

}  // namespace gplvr
