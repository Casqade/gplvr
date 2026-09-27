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
    bool   reportedFailure = false;
};

Entries g_entries;
Font    g_fonts[kVrTextSlots];

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

}  // namespace

bool vrTextEnsure(const GlFunctions& gl, int slot, int pixelHeight) {
    if (!validSlot(slot)) return false;
    if (!resolveEntries(gl)) return false;
    if (!gl.wglGetCurrentContext || !gl.wglGetCurrentDC) return false;

    if (pixelHeight < 8) pixelHeight = 8;
    if (pixelHeight > 96) pixelHeight = 96;

    Font& font = g_fonts[slot];

    HGLRC context = gl.wglGetCurrentContext();
    if (!context) return false;

    if (font.base && font.builtFor == pixelHeight && font.context == context) return true;

    // A context change means the old lists are somebody else's now. Forget them rather
    // than deleting names that no longer belong to us.
    if (font.base && font.context != context) font.base = 0;
    if (font.base) {
        g_entries.deleteLists(font.base, kCharCount);
        font.base = 0;
    }

    HDC dc = gl.wglGetCurrentDC();
    if (!dc) return false;

    // Monospaced on purpose: a menu laid out in columns is arithmetic rather than
    // measurement, and a fallback substitution by GDI still keeps the metrics this asks
    // for. NONANTIALIASED, because wglUseFontBitmaps produces 1-bit bitmaps and asking for
    // smoothing it cannot deliver only changes which glyph shapes GDI picks.
    HFONT created = CreateFontA(-pixelHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                                "Consolas");
    if (!created) {
        if (!font.reportedFailure) {
            font.reportedFailure = true;
            logf("vr text: CreateFontA refused (error %lu) -- there is no menu text",
                 GetLastError());
        }
        return false;
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
        if (!font.reportedFailure) {
            font.reportedFailure = true;
            logf("vr text: %s -- there is no menu text",
                 base ? "wglUseFontBitmapsA refused the font" : "glGenLists gave no names");
        }
        return false;
    }

    font.base = base;
    font.builtFor = pixelHeight;
    font.context = context;
    font.height = metrics.tmHeight;
    font.ascent = metrics.tmAscent;
    font.reportedFailure = false;
    logf("vr text: slot %d built at %d px (cell %d, ascent %d)", slot, pixelHeight,
         font.height, font.ascent);
    return true;
}

void vrTextRelease(const GlFunctions& gl, bool contextAlive) {
    for (Font& font : g_fonts) {
        if (font.base && contextAlive && g_entries.deleteLists)
            g_entries.deleteLists(font.base, kCharCount);
        font.base = 0;
        font.height = 0;
        font.ascent = 0;
        font.builtFor = 0;
        font.context = nullptr;
    }
    (void)gl;
    (void)contextAlive;
}

int vrTextHeight(int slot) { return validSlot(slot) ? g_fonts[slot].height : 0; }

int vrTextAscent(int slot) { return validSlot(slot) ? g_fonts[slot].ascent : 0; }

int vrTextWidth(int slot, const char* text) {
    if (!validSlot(slot) || !text || !g_fonts[slot].base) return 0;
    const Font& font = g_fonts[slot];
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
    if (!validSlot(slot) || !text || !*text || !g_fonts[slot].base) return;
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
    g_entries.listBase(g_fonts[slot].base - kFirstChar);
    g_entries.callLists(static_cast<GLsizei>(length), GL_UNSIGNED_BYTE, text);
    g_entries.listBase(0);

    gl.pixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    gl.pixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    gl.pixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    gl.pixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
    gl.pixelStorei(GL_UNPACK_LSB_FIRST, lsbFirst);
}

}  // namespace gplvr
