#include "CursorOverlay.h"

#include "Hooks.h"
#include "Log.h"
#include "Stereo.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

// -- what gets drawn -------------------------------------------------------------------
//
// THE REAL CURSOR, not a drawing of one. GetCursorInfo hands back the HCURSOR Windows is
// showing at this instant; GetIconInfo turns that into a colour bitmap and a mask, and
// GetDIBits turns those into pixels. The result is the arrow the user actually has --
// their theme, their size, their scaling -- and it follows the shape as it changes,
// because the texture is rebuilt whenever the HCURSOR handle does.
//
// The hotspot comes from ICONINFO and is honoured, so the point that clicks is the point
// that is drawn at the pointer's position rather than the bitmap's top-left corner.
//
// A shape that cannot be read draws NOTHING, and says so once. A plain dot used to stand
// in here; it was scaffolding from the period when the mask handling was unproven, and a
// stand-in pointer that is not the user's own pointer is worse than an honest gap with a
// line in the log naming the cursor that could not be read.

struct Image {
    HCURSOR source = nullptr;
    GLuint  texture = 0;
    int     width = 0;       // the cursor's own pixels
    int     height = 0;
    int     texWidth = 0;    // padded up to powers of two, so a 48x48 large-pointer
    int     texHeight = 0;   // cursor cannot be refused by a driver without NPOT
    int     hotX = 0;
    int     hotY = 0;
    bool    premultiplied = false;
    bool    valid = false;
    HCURSOR failedFor = nullptr;  // the handle we already tried and could not convert
};

// A destination rectangle in the overlay's own coordinate space: viewport-relative,
// origin top-left, y down -- the same space the ortho below sets up and the same space
// GPL lays its menu out in.
struct Quad {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// -- the backing store -----------------------------------------------------------------
//
// GPL'S MENU DOES NOT REDRAW THE WHOLE BACK BUFFER, and roglv2 preserves its contents
// across the swap. So the first version of this painted like a brush: every cursor ever
// drawn stayed in the buffer forever and the pointer left a trail behind it. Measured in
// game, not predicted -- and it is a property of the destination, not of the drawing.
//
// The fix is a save/restore pair inside ONE frame:
//
//   1. before drawing, copy the rectangle the cursor is about to cover out of the
//      currently bound framebuffer into a scratch texture,
//   2. draw the cursor,
//   3. after the frame has gone to the headset and BEFORE the game presents, draw those
//      saved pixels straight back.
//
// The saved pixels are microseconds old and come from the same frame and the same
// framebuffer, so there is no staleness risk at all -- which is exactly why the restore
// belongs here and not at the start of the next frame.
//
// **The flat window therefore stops showing the overlay, and that is correct rather than
// a compromise.** Windows already draws the real pointer over the window; the only place
// that cannot see it is the headset, and the headset's copy is taken before the restore.
// Do not "fix" this by moving the restore after the present -- that puts the trail back.
//
// The rects are also CLIPPED to the viewport. The log shows the pointer reaching (639,
// 479) on a 640x480 surface, and a copy that ran off the edge would read outside the
// framebuffer and restore whatever the driver felt like returning.
struct Backup {
    GLuint texture = 0;
    int    texWidth = 0;   // powers of two, grown as needed and never shrunk
    int    texHeight = 0;
    int    x = 0;          // the saved rect, in the overlay's coordinate space
    int    y = 0;
    int    width = 0;
    int    height = 0;
    bool   held = false;
};

struct Pending {
    Backup slot[2];
    int    count = 0;
    GLuint framebuffer = 0;
    GLint  viewport[4] = {0, 0, 0, 0};
    bool   armed = false;  // a save is waiting for its restore
};

// Every reason this can decline to draw. Reported on CHANGE with a cap, never once: a
// one-shot report spends itself on a startup frame and says nothing about the frame
// under investigation, which has happened five times in this project.
enum class Refusal {
    None,
    OptionOff,
    NoEntryPoints,
    NoViewport,
    NoWindow,
    NoClientArea,
    NoCursorPos,
    PointerHidden,
    ShapeUnreadable,
};

struct State {
    Refusal refusal = Refusal::None;
    int     refusalReportsLeft = 12;

    bool   loggedEntry = false;  // the matrix mode found on the way in
    GLuint lastFramebuffer = 0xFFFFFFFFu;
    int    lastSurfaceW = 0;
    int    lastSurfaceH = 0;
    long   lastClientW = 0;
    long   lastClientH = 0;
    int    lastSideBySide = -1;
    int    lastEyeRects = -1;  // 1 = the stereo rects, 0 = halves of the viewport
    int    stateReportsLeft = 20;

    DWORD lastPositionReport = 0;
    long  lastX = MAXLONG;
    long  lastY = MAXLONG;
    int   positionReportsLeft = 30;
    bool  saidPositionReportsEnd = false;

    int shapeReportsLeft = 8;

    bool loggedSave = false;
    bool loggedRestore = false;
    int  orphanReportsLeft = 4;
    bool warnedNoRebind = false;

    GLenum lastError = GL_NO_ERROR;
    int    errorReportsLeft = 8;
    bool   loggedCleanDraw = false;
};

State   g_cursor;
Image   g_image;
Pending g_pending;

const char* refusalText(Refusal reason, char* scratch, size_t scratchSize) {
    switch (reason) {
        case Refusal::None:
            return "no reason";
        case Refusal::OptionOff:
            return "vrCursor = 0";
        case Refusal::NoEntryPoints:
            return "the immediate-mode GL entry points did not resolve -- glBegin, "
                   "glEnd, glVertex2f, glTexCoord2f, glColor4f, glBlendFunc, glTexEnvi, "
                   "glPushAttrib, glPushMatrix, glPolygonMode, glColorMask, "
                   "glCopyTexSubImage2D and glViewport all have to be there";
        case Refusal::NoViewport:
            return "the GL viewport has no area, so there is no surface to draw into";
        case Refusal::NoWindow:
            return "the game window could not be found, so a screen position cannot be "
                   "turned into a surface position";
        case Refusal::NoClientArea:
            return "the game window's client area has no area";
        case Refusal::NoCursorPos:
            _snprintf_s(scratch, scratchSize, _TRUNCATE,
                        "GetCursorPos failed (error %lu)", GetLastError());
            return scratch;
        case Refusal::PointerHidden:
            return "Windows reports the pointer HIDDEN, and vrCursor = 1 follows "
                   "Windows. If the menus are unusable and this is the line you are "
                   "reading, set vrCursor = 2 to draw it anyway";
        case Refusal::ShapeUnreadable:
            return "the cursor Windows is showing could not be turned into pixels -- the "
                   "'cursor shape' line above says which step refused. Nothing is drawn "
                   "rather than a stand-in that is not the real pointer";
    }
    return "unknown";
}

// One place, so a recovery is as loud as the failure. Silence on the way back in is how
// "it started working again" becomes invisible.
void noteRefusal(Refusal reason) {
    if (reason == g_cursor.refusal) return;

    const Refusal previous = g_cursor.refusal;
    g_cursor.refusal = reason;
    if (g_cursor.refusalReportsLeft <= 0) return;
    --g_cursor.refusalReportsLeft;

    char scratch[160] = {};
    const char* tail = g_cursor.refusalReportsLeft
                           ? ""
                           : " -- further cursor state changes will not be reported";
    if (reason == Refusal::None) {
        logf("cursor: drawing again (it was: %s)%s",
             refusalText(previous, scratch, sizeof(scratch)), tail);
        return;
    }
    logf("cursor: NOT drawing the pointer -- %s%s",
         refusalText(reason, scratch, sizeof(scratch)), tail);
}

bool entryPointsPresent(const GlFunctions& gl) {
    return gl.pushAttrib && gl.popAttrib && gl.pushMatrix && gl.popMatrix && gl.begin &&
           gl.end && gl.vertex2f && gl.texCoord2f && gl.color4f && gl.blendFunc &&
           gl.texEnvi && gl.polygonMode && gl.colorMask && gl.matrixMode &&
           gl.loadIdentity && gl.ortho && gl.enable && gl.disable && gl.getIntegerv &&
           gl.getError && gl.genTextures && gl.deleteTextures && gl.bindTexture &&
           gl.texImage2D && gl.texParameteri && gl.pixelStorei &&
           // the backing store's
           gl.copyTexSubImage2D && gl.viewport;
}

int nextPowerOfTwo(int value) {
    int power = 1;
    while (power < value) power <<= 1;
    return power;
}

void shapeLog(const char* format, ...) {
    if (g_cursor.shapeReportsLeft <= 0) return;
    --g_cursor.shapeReportsLeft;

    char line[512] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    logf("%s%s", line,
         g_cursor.shapeReportsLeft ? ""
                                   : " (further cursor shapes will not be reported)");
}

// Turn the HCURSOR Windows is showing into an RGBA texture.
//
// Three sources of alpha, in the order they are tried, because a cursor can be any of
// the three and guessing wrong is what makes an overlay look like a grey box:
//
//   1. a 32-bit colour bitmap that carries real alpha -- the modern case,
//   2. a colour bitmap with no alpha at all, where the AND mask supplies it -- legacy
//      colour cursors, whose alpha bytes come back as zero and would otherwise make the
//      whole cursor invisible,
//   3. no colour bitmap at all: a monochrome cursor, whose single mask bitmap is TWICE
//      the cursor's height -- the AND plane stacked on the XOR plane. AND=1,XOR=1 means
//      "invert what is underneath", which has no equivalent in a blended quad and is
//      drawn as opaque black.
//
// Which one it was is logged, along with the size and the hotspot, so a cursor that
// comes out wrong can be diagnosed from the log rather than from a screenshot.
bool buildCursorTexture(const GlFunctions& gl, HCURSOR cursor) {
    ICONINFO info = {};
    if (!GetIconInfo(cursor, &info)) {
        shapeLog("cursor shape %p: GetIconInfo failed (error %lu)", (void*)cursor,
                 GetLastError());
        return false;
    }

    BITMAP colourBitmap = {};
    BITMAP maskBitmap = {};
    const bool haveColour =
        info.hbmColor && GetObjectA(info.hbmColor, sizeof(BITMAP), &colourBitmap) != 0;
    const bool haveMask =
        info.hbmMask && GetObjectA(info.hbmMask, sizeof(BITMAP), &maskBitmap) != 0;

    const int width = haveColour ? colourBitmap.bmWidth
                                 : (haveMask ? maskBitmap.bmWidth : 0);
    const int height = haveColour ? colourBitmap.bmHeight
                                  : (haveMask ? maskBitmap.bmHeight / 2 : 0);

    bool built = false;
    std::vector<unsigned char> rgba;
    const char* alphaSource = "none";

    HDC screen = GetDC(nullptr);
    if (!screen) {
        shapeLog("cursor shape %p: no screen DC, so the bitmaps cannot be read",
                 (void*)cursor);
    } else if (width <= 0 || height <= 0 || width > 512 || height > 512) {
        shapeLog("cursor shape %p: %dx%d is not a cursor size this will draw",
                 (void*)cursor, width, height);
    } else {
        // Negative height asks for TOP-DOWN rows, which is the order glTexImage2D wants
        // and the order the ortho below draws in. Asking bottom-up and flipping later is
        // one more place for the vertical flip this project has already been bitten by.
        // A FRESH header per call. GetDIBits writes back into the BITMAPINFOHEADER it is
        // handed -- biSizeImage at least, and the documentation does not promise the
        // rest survives -- so reusing one structure for the second call would be asking
        // for whatever the first call left in it.
        auto makeRequest = [width](int rows) {
            BITMAPINFO request = {};
            request.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            request.bmiHeader.biWidth = width;
            request.bmiHeader.biHeight = -rows;  // negative: TOP-DOWN rows
            request.bmiHeader.biPlanes = 1;
            request.bmiHeader.biBitCount = 32;
            request.bmiHeader.biCompression = BI_RGB;
            return request;
        };

        const int maskRows = haveColour ? height : height * 2;
        std::vector<unsigned char> mask(static_cast<size_t>(width) * maskRows * 4, 0);
        BITMAPINFO maskRequest = makeRequest(maskRows);
        const bool gotMask =
            haveMask && GetDIBits(screen, info.hbmMask, 0, maskRows, mask.data(),
                                  &maskRequest, DIB_RGB_COLORS) != 0;

        std::vector<unsigned char> colour(static_cast<size_t>(width) * height * 4, 0);
        bool gotColour = false;
        if (haveColour) {
            BITMAPINFO colourRequest = makeRequest(height);
            gotColour = GetDIBits(screen, info.hbmColor, 0, height, colour.data(),
                                  &colourRequest, DIB_RGB_COLORS) != 0;
        }

        if (haveColour && !gotColour) {
            shapeLog("cursor shape %p: GetDIBits refused the colour bitmap (error %lu)",
                     (void*)cursor, GetLastError());
        } else if (!haveColour && !gotMask) {
            shapeLog("cursor shape %p: it is monochrome and GetDIBits refused the mask "
                     "(error %lu)",
                     (void*)cursor, GetLastError());
        } else {
            rgba.assign(static_cast<size_t>(width) * height * 4, 0);
            const size_t pixels = static_cast<size_t>(width) * height;

            int  inverted = 0;
            bool straight = false;  // any colour channel above its alpha

            if (gotColour) {
                unsigned char maxAlpha = 0;
                for (size_t i = 0; i < pixels; ++i)
                    if (colour[i * 4 + 3] > maxAlpha) maxAlpha = colour[i * 4 + 3];

                for (size_t i = 0; i < pixels; ++i) {
                    const unsigned char b = colour[i * 4 + 0];
                    const unsigned char g = colour[i * 4 + 1];
                    const unsigned char r = colour[i * 4 + 2];
                    unsigned char       a = colour[i * 4 + 3];
                    if (!maxAlpha) {
                        // No alpha channel: the AND mask is the alpha. A set AND bit
                        // means "leave the screen alone", i.e. transparent.
                        const bool andBit = gotMask && mask[i * 4] != 0;
                        a = andBit ? 0 : 255;
                    }
                    if (a && (r > a || g > a || b > a)) straight = true;
                    rgba[i * 4 + 0] = r;
                    rgba[i * 4 + 1] = g;
                    rgba[i * 4 + 2] = b;
                    rgba[i * 4 + 3] = a;
                }
                alphaSource = maxAlpha ? "its own alpha channel"
                                       : "the AND mask (the colour bitmap carries no "
                                         "alpha)";
                g_image.premultiplied = maxAlpha != 0 && !straight;
            } else {
                // Monochrome: AND plane on top, XOR plane below it.
                for (size_t i = 0; i < pixels; ++i) {
                    const bool andBit = mask[i * 4] != 0;
                    const bool xorBit = mask[(pixels + i) * 4] != 0;
                    unsigned char value = 0;
                    unsigned char alpha = 255;
                    if (andBit && !xorBit) {
                        alpha = 0;  // transparent
                    } else if (andBit && xorBit) {
                        ++inverted;  // "invert the screen"; drawn as black
                        value = 0;
                    } else {
                        value = xorBit ? 255 : 0;
                    }
                    rgba[i * 4 + 0] = value;
                    rgba[i * 4 + 1] = value;
                    rgba[i * 4 + 2] = value;
                    rgba[i * 4 + 3] = alpha;
                }
                alphaSource = "the AND/XOR planes of a monochrome cursor";
                g_image.premultiplied = false;
            }

            built = true;
            shapeLog("cursor shape %p: %dx%d, hotspot (%d,%d), %s, alpha from %s, "
                     "treated as %s alpha%s",
                     (void*)cursor, width, height, (int)info.xHotspot,
                     (int)info.yHotspot, gotColour ? "colour" : "monochrome",
                     alphaSource,
                     g_image.premultiplied ? "PREMULTIPLIED" : "straight",
                     inverted ? " (some pixels ask to invert the screen and are drawn "
                                "black)"
                              : "");
        }
    }

    if (screen) ReleaseDC(nullptr, screen);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (!built) return false;

    // Padded to powers of two. A 32x32 cursor is already one, but "large pointer" sizes
    // are 48x48 and 72x72, and a driver without ARB_texture_non_power_of_two would
    // refuse those with INVALID_VALUE -- which would arrive as an empty overlay and a GL
    // error blamed on the draw. The padding costs nothing and removes the question.
    const int texWidth = nextPowerOfTwo(width);
    const int texHeight = nextPowerOfTwo(height);
    std::vector<unsigned char> padded(static_cast<size_t>(texWidth) * texHeight * 4, 0);
    for (int y = 0; y < height; ++y) {
        memcpy(&padded[static_cast<size_t>(y) * texWidth * 4],
               &rgba[static_cast<size_t>(y) * width * 4],
               static_cast<size_t>(width) * 4);
    }

    if (!g_image.texture) gl.genTextures(1, &g_image.texture);
    if (!g_image.texture) {
        shapeLog("cursor shape %p: glGenTextures gave nothing back", (void*)cursor);
        return false;
    }

    // The unpack state is CLIENT state and glPushAttrib does not save it, so it is read,
    // set and put back by hand. roglv2 uploads textures of its own and a row length left
    // behind by one of them would shear ours.
    GLint unpackAlignment = 4;
    GLint unpackRowLength = 0;
    GLint unpackSkipPixels = 0;
    GLint unpackSkipRows = 0;
    gl.getIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
    gl.getIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
    gl.getIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels);
    gl.getIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows);

    GLint previousTexture = 0;
    gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    gl.pixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.pixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.pixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.pixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    gl.bindTexture(GL_TEXTURE_2D, g_image.texture);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texWidth, texHeight, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, padded.data());
    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

    gl.pixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    gl.pixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
    gl.pixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels);
    gl.pixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows);

    g_image.source = cursor;
    g_image.width = width;
    g_image.height = height;
    g_image.texWidth = texWidth;
    g_image.texHeight = texHeight;
    g_image.hotX = static_cast<int>(info.xHotspot);
    g_image.hotY = static_cast<int>(info.yHotspot);
    g_image.valid = true;
    g_image.failedFor = nullptr;
    return true;
}

// The cursor bitmap, at `scale` surface pixels per screen pixel, with its hotspot at
// (x, y). Alpha blended, and the blend function follows what the pixels turned out to
// be: premultiplied ones need GL_ONE, straight ones GL_SRC_ALPHA, and using the wrong
// one darkens or halos every soft edge.
void drawCursorImage(const GlFunctions& gl, const Quad& quad) {
    const float left = quad.left;
    const float top = quad.top;
    const float right = quad.right;
    const float bottom = quad.bottom;
    const float u = static_cast<float>(g_image.width) / g_image.texWidth;
    const float v = static_cast<float>(g_image.height) / g_image.texHeight;

    gl.enable(GL_TEXTURE_2D);
    gl.bindTexture(GL_TEXTURE_2D, g_image.texture);
    gl.texEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gl.enable(GL_BLEND);
    gl.blendFunc(static_cast<GLenum>(g_image.premultiplied ? GL_ONE : GL_SRC_ALPHA),
                 static_cast<GLenum>(GL_ONE_MINUS_SRC_ALPHA));
    gl.color4f(1.0f, 1.0f, 1.0f, 1.0f);

    gl.begin(GL_QUADS);
    gl.texCoord2f(0.0f, 0.0f);
    gl.vertex2f(left, top);
    gl.texCoord2f(u, 0.0f);
    gl.vertex2f(right, top);
    gl.texCoord2f(u, v);
    gl.vertex2f(right, bottom);
    gl.texCoord2f(0.0f, v);
    gl.vertex2f(left, bottom);
    gl.end();

    gl.bindTexture(GL_TEXTURE_2D, 0);
    gl.disable(GL_TEXTURE_2D);
    gl.disable(GL_BLEND);
}

// Copy the pixels a cursor is about to cover out of the bound framebuffer.
//
// The rect arrives in the overlay's coordinate space -- viewport-relative, y DOWN --
// and glCopyTexSubImage2D reads in framebuffer coordinates with the origin at the
// BOTTOM left, so the y is flipped here once and the restore flips its texture
// coordinates to match. One flip in each direction, both written down, rather than a
// sign that has to be rediscovered.
bool saveUnderCursor(const GlFunctions& gl, Backup& backup, const Quad& quad,
                     const GLint viewport[4]) {
    backup.held = false;

    // Whole pixels that fully contain the quad, then clipped to the viewport. One pixel
    // of margin either side, because the quad's edges are fractional and a fragment can
    // be touched by a partially covered pixel.
    int x0 = static_cast<int>(std::floor(quad.left)) - 1;
    int y0 = static_cast<int>(std::floor(quad.top)) - 1;
    int x1 = static_cast<int>(std::ceil(quad.right)) + 1;
    int y1 = static_cast<int>(std::ceil(quad.bottom)) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > viewport[2]) x1 = viewport[2];
    if (y1 > viewport[3]) y1 = viewport[3];

    const int width = x1 - x0;
    const int height = y1 - y0;
    if (width <= 0 || height <= 0) return false;  // entirely off the surface

    if (!backup.texture) {
        gl.genTextures(1, &backup.texture);
        if (!backup.texture) return false;
    }

    gl.bindTexture(GL_TEXTURE_2D, backup.texture);
    if (width > backup.texWidth || height > backup.texHeight) {
        // Grown, never shrunk, and always to a power of two -- the same NPOT caution as
        // the cursor bitmap, and it means a moving pointer reallocates at most a couple
        // of times a session rather than every frame.
        const int wanted = nextPowerOfTwo(width > backup.texWidth ? width
                                                                  : backup.texWidth);
        const int wantedH = nextPowerOfTwo(height > backup.texHeight ? height
                                                                     : backup.texHeight);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
        gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA, wanted, wantedH, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
        backup.texWidth = wanted;
        backup.texHeight = wantedH;
    }

    const int fbX = viewport[0] + x0;
    const int fbY = viewport[1] + (viewport[3] - y1);
    gl.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fbX, fbY, width, height);

    backup.x = x0;
    backup.y = y0;
    backup.width = width;
    backup.height = height;
    backup.held = true;
    return true;
}

// Put one saved rect back. GL_NEAREST and GL_REPLACE, so the texels land on the pixels
// they came from and nothing modulates them on the way -- the point is that the
// framebuffer ends up bit-identical to how it was found, not merely similar.
void restoreOneRect(const GlFunctions& gl, const Backup& backup) {
    const float left = static_cast<float>(backup.x);
    const float top = static_cast<float>(backup.y);
    const float right = left + backup.width;
    const float bottom = top + backup.height;
    const float u = static_cast<float>(backup.width) / backup.texWidth;
    const float v = static_cast<float>(backup.height) / backup.texHeight;

    gl.enable(GL_TEXTURE_2D);
    gl.bindTexture(GL_TEXTURE_2D, backup.texture);
    gl.texEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    gl.color4f(1.0f, 1.0f, 1.0f, 1.0f);

    // v is flipped against the cursor's own quad: the copy filled the texture from the
    // framebuffer, whose origin is at the BOTTOM, while this ortho has y going down.
    gl.begin(GL_QUADS);
    gl.texCoord2f(0.0f, v);
    gl.vertex2f(left, top);
    gl.texCoord2f(u, v);
    gl.vertex2f(right, top);
    gl.texCoord2f(u, 0.0f);
    gl.vertex2f(right, bottom);
    gl.texCoord2f(0.0f, 0.0f);
    gl.vertex2f(left, bottom);
    gl.end();

    gl.bindTexture(GL_TEXTURE_2D, 0);
    gl.disable(GL_TEXTURE_2D);
}

}  // namespace

void cursorOverlayReleaseGl() {
    // The names belong to a context that is going away, so they are dropped rather than
    // deleted -- glDeleteTextures against a dead or different context is either ignored
    // or destroys somebody else's object of the same name.
    if (g_image.texture || g_pending.slot[0].texture)
        logf("cursor: the GL context is going, so the cursor texture (%u) and the "
             "backing store (%u, %u) are dropped; they are rebuilt on the next frame "
             "that draws",
             g_image.texture, g_pending.slot[0].texture, g_pending.slot[1].texture);
    g_image = Image{};
    g_pending = Pending{};
}

void cursorOverlayRestore(const GlFunctions& gl) {
    if (!g_pending.armed) return;
    g_pending.armed = false;

    if (!entryPointsPresent(gl)) return;

    // The framebuffer the pixels came from -- which is NOT the one bound now. By this
    // point presenterEndFrame has unbound the off-screen target and the window blit has
    // been and gone, so the restore has to go back to where it saved from and put the
    // previous binding back afterwards. Restoring into the wrong buffer would paint a
    // patch of the off-screen frame into the window at coordinates that mean nothing
    // there.
    GLint previousFramebuffer = 0;
    if (gl.getIntegerv) gl.getIntegerv(kGlFramebufferBinding, &previousFramebuffer);
    const bool rebind = static_cast<GLuint>(previousFramebuffer) != g_pending.framebuffer;
    if (rebind && !gl.bindFramebuffer) {
        if (!g_cursor.warnedNoRebind) {
            g_cursor.warnedNoRebind = true;
            logf("cursor: the pixels under the pointer were saved from framebuffer %u but "
                 "%d is bound now and there is no glBindFramebuffer to go back with -- "
                 "the restore is skipped and the pointer will leave a trail",
                 g_pending.framebuffer, previousFramebuffer);
        }
        return;
    }

    // Drained before, read back after, on every frame that restores -- the same rule the
    // draw follows. Anything pending at this point has already been past the frame
    // transfer's own check, so nothing that would have been reported is being swallowed.
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {
    }

    if (rebind) gl.bindFramebuffer(kGlFramebuffer, g_pending.framebuffer);

    GLint savedMatrixMode = GL_PROJECTION;
    gl.getIntegerv(GL_MATRIX_MODE, &savedMatrixMode);

    // GL_VIEWPORT_BIT as well as the draw's set: the frame transfer and the window blit
    // both run between the save and here, and neither promises what it leaves the
    // viewport as. It is set from the value recorded at save time rather than read now,
    // so the coordinate space is the same one the rects were measured in.
    gl.pushAttrib(static_cast<GLbitfield>(
        GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
        GL_POLYGON_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT | GL_VIEWPORT_BIT));

    gl.viewport(g_pending.viewport[0], g_pending.viewport[1], g_pending.viewport[2],
                g_pending.viewport[3]);

    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    gl.ortho(0.0, static_cast<double>(g_pending.viewport[2]),
             static_cast<double>(g_pending.viewport[3]), 0.0, -1.0, 1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    gl.disable(GL_TEXTURE_1D);
    gl.disable(GL_BLEND);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_STENCIL_TEST);
    gl.disable(GL_SCISSOR_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_COLOR_LOGIC_OP);
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    int restored = 0;
    for (int i = 0; i < g_pending.count && i < 2; ++i) {
        if (!g_pending.slot[i].held) continue;
        restoreOneRect(gl, g_pending.slot[i]);
        ++restored;
    }

    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMatrixMode));

    if (rebind) gl.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previousFramebuffer));

    const GLenum error = gl.getError();
    if (!g_cursor.loggedRestore) {
        g_cursor.loggedRestore = true;
        logf("cursor: the pixels under the pointer were put back into framebuffer %u -- "
             "%d rect(s), the first %dx%d at (%d,%d), viewport %dx%d%s. The game's own "
             "buffer is left exactly as it was found, which is what stops the trail.",
             g_pending.framebuffer, restored, g_pending.slot[0].width,
             g_pending.slot[0].height, g_pending.slot[0].x, g_pending.slot[0].y,
             g_pending.viewport[2], g_pending.viewport[3],
             rebind ? " (re-bound to do it, and put back)" : "");
    }
    if (error != GL_NO_ERROR && g_cursor.errorReportsLeft > 0) {
        --g_cursor.errorReportsLeft;
        logf("cursor: GL error 0x%04X came out of the backing-store RESTORE -- the "
             "errors were drained first, so this one is ours%s",
             static_cast<unsigned>(error),
             g_cursor.errorReportsLeft ? "" : " (further GL errors will not be reported)");
    }
}

void cursorOverlayDraw(const GlFunctions& gl, bool sideBySide) {
    if (g_options.vrCursor <= 0) {
        noteRefusal(Refusal::OptionOff);
        return;
    }
    if (!entryPointsPresent(gl)) {
        noteRefusal(Refusal::NoEntryPoints);
        return;
    }

    // Windows' own opinion of whether there is a pointer to show. vrCursor = 1 follows
    // it, so nothing is painted over the driving view once the game hides the cursor;
    // vrCursor = 2 ignores it, which is the escape hatch if it turns out GPL never sets
    // the flag in the first place. Which of the two happened is in the log either way,
    // so it is one ini digit rather than a rebuild.
    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    const bool asked = GetCursorInfo(&info) != FALSE;
    const bool showing = asked && (info.flags & CURSOR_SHOWING) != 0;
    if (g_options.vrCursor == 1 && asked && !showing) {
        noteRefusal(Refusal::PointerHidden);
        return;
    }

    // The viewport, not a remembered size. It is what the engine last set from its own
    // display fields, and it is true of the framebuffer that is bound right now --
    // whichever of the two that is, the 640x480 menu back buffer or the off-screen
    // target. A size carried in from elsewhere would be a second copy of a number that
    // already exists here, and two copies of one number eventually disagree.
    GLint viewport[4] = {0, 0, 0, 0};
    gl.getIntegerv(GL_VIEWPORT, viewport);
    const int surfaceW = viewport[2];
    const int surfaceH = viewport[3];
    if (surfaceW <= 0 || surfaceH <= 0) {
        noteRefusal(Refusal::NoViewport);
        return;
    }

    // Which framebuffer this frame went into -- asked BEFORE the drain below, on
    // purpose. A driver with no framebuffer objects raises INVALID_ENUM on this query
    // rather than answering, and asking after the drain would leave that error to be
    // read back at the end and blamed on the overlay's own drawing. 0 -- the window's
    // own back buffer -- is the right answer on such a driver anyway.
    GLuint framebuffer = 0;
    {
        GLint bound = 0;
        gl.getIntegerv(kGlFramebufferBinding, &bound);
        framebuffer = static_cast<GLuint>(bound < 0 ? 0 : bound);
    }

    HWND window = gameWindow();
    if (!window) {
        noteRefusal(Refusal::NoWindow);
        return;
    }

    RECT client = {};
    GetClientRect(window, &client);
    const long clientW = client.right - client.left;
    const long clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) {
        noteRefusal(Refusal::NoClientArea);
        return;
    }

    POINT screen = {};
    if (!GetCursorPos(&screen)) {
        noteRefusal(Refusal::NoCursorPos);
        return;
    }
    POINT local = screen;
    ScreenToClient(window, &local);

    // Clamped rather than dropped when it is outside. ClipCursor normally keeps the
    // pointer inside the window, but blockClipCursor = 1 turns that off, and a cursor
    // pinned to the edge it left by is more use than one that vanishes -- it says "the
    // pointer is off that side", which is the question being asked.
    float fx = static_cast<float>(local.x) / static_cast<float>(clientW);
    float fy = static_cast<float>(local.y) / static_cast<float>(clientH);
    if (fx < 0.0f) fx = 0.0f;
    if (fx > 1.0f) fx = 1.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fy > 1.0f) fy = 1.0f;

    // A GL error is sticky until it is read, and reading it CLEARS it -- so a check is a
    // mutation, and one that runs on some frames and not others hands its own errors to
    // whoever looks next. Drained here on every frame that draws, and read back on every
    // frame that draws, so the verdict at the bottom can only ever be about these calls.
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {
    }

    // The cursor SHAPE, rebuilt only when the handle changes -- which is what makes this
    // affordable: an I-beam or a resize arrow is a new HCURSOR and gets a new texture,
    // and the arrow the user stares at for an hour is read once.
    //
    // A shape that cannot be read draws NOTHING, and the failure is remembered so the
    // conversion is not retried every frame. The refusal below says the pointer is gone;
    // the "cursor shape" line above says which step refused.
    bool haveImage = false;
    if (info.hCursor) {
        if (g_image.valid && g_image.source == info.hCursor) {
            haveImage = true;
        } else if (info.hCursor != g_image.failedFor) {
            haveImage = buildCursorTexture(gl, info.hCursor);
            if (!haveImage) {
                g_image.valid = false;
                g_image.failedFor = info.hCursor;
            }
        }
    }
    if (!haveImage) {
        noteRefusal(Refusal::ShapeUnreadable);
        return;
    }

    // Where the cursors go. One per eye when the frame holds two, so the pointer is at
    // the same place in both and fuses at the depth of the rest of the image instead of
    // sitting in one eye only.
    int  rects[2][4] = {};
    int  count = 1;
    bool usedStereoRects = false;
    if (sideBySide) {
        count = 2;
        if (stereoEyeRect(0, rects[0]) && stereoEyeRect(1, rects[1])) {
            usedStereoRects = true;
        } else {
            // The two halves, when the exact rects are unavailable. Not a guess to be
            // quiet about -- it is logged below, because a wrong half puts the pointer
            // in the other eye.
            const int half = surfaceW / 2;
            rects[0][0] = 0;
            rects[0][1] = 0;
            rects[0][2] = half;
            rects[0][3] = surfaceH;
            rects[1][0] = half;
            rects[1][1] = 0;
            rects[1][2] = surfaceW - half;
            rects[1][3] = surfaceH;
        }
    } else {
        rects[0][0] = 0;
        rects[0][1] = 0;
        rects[0][2] = surfaceW;
        rects[0][3] = surfaceH;
    }

    // The matrix mode is READ, not assumed, and put back at the end.
    //
    // rogl_displaySetDestination -- the vtable slot polyDraw calls on every destination
    // change -- issues glLoadIdentity and glOrtho with no glMatrixMode of its own, and
    // relies on rogl_displaySetupGlState having left the mode at GL_PROJECTION. Leaving
    // it at GL_MODELVIEW made every later ortho load into the modelview matrix and cost
    // nine test runs. Restoring exactly what was found is correct whatever it was, and
    // the value is logged once so the assumption becomes a measurement.
    GLint savedMatrixMode = GL_PROJECTION;
    gl.getIntegerv(GL_MATRIX_MODE, &savedMatrixMode);

    // GL_PIXEL_MODE_BIT is in there for the backing store: the READ buffer lives in that
    // group, not in GL_COLOR_BUFFER_BIT, and the save sets it explicitly.
    gl.pushAttrib(static_cast<GLbitfield>(
        GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
        GL_POLYGON_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT | GL_PIXEL_MODE_BIT));

    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    // ORIGIN TOP LEFT, matching the glOrtho(0, w, h, 0, ...) in rogl_displaySetupGlState
    // and therefore matching how the engine lays the menu out. GL's own framebuffer
    // origin is bottom left, and getting this backwards is a documented bug in this
    // project -- the flipped picture reads as "touching the bottom border" rather than
    // as garbage, which is why it survives review.
    //
    // Near and far are -1 and 1, not the engine's 0 and 1: at 0 the z = 0 our vertices
    // carry sits exactly on the near plane, and there is no reason to draw on a clip
    // boundary when this projection is ours alone.
    gl.ortho(0.0, static_cast<double>(surfaceW), static_cast<double>(surfaceH), 0.0, -1.0,
             1.0);

    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    // Everything that can silently eat the draw. The lesson these come from is the depth
    // stamp that wrote nothing because roglv2 leaves glDepthMask FALSE, and whose blank
    // result was then read as a fact about the framebuffer: a write that can be
    // suppressed has to have its suppressors turned off, not assumed off.
    gl.disable(GL_TEXTURE_2D);
    gl.disable(GL_TEXTURE_1D);
    gl.disable(GL_BLEND);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_STENCIL_TEST);
    gl.disable(GL_SCISSOR_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_COLOR_LOGIC_OP);
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // THE GEOMETRY FIRST, THEN THE SAVE, THEN THE DRAW. All three passes rather than one
    // loop, because the backing store has to capture clean pixels: with two eyes drawn
    // in one pass, the second eye's save could otherwise read a rect the first eye had
    // already been drawn into if the two ever overlapped.
    Quad  quads[2];
    float drawnX[2] = {0.0f, 0.0f};
    float drawnY[2] = {0.0f, 0.0f};
    float drawnSize = 0.0f;
    for (int i = 0; i < count; ++i) {
        drawnX[i] = static_cast<float>(rects[i][0]) + fx * static_cast<float>(rects[i][2]);
        drawnY[i] = static_cast<float>(rects[i][1]) + fy * static_cast<float>(rects[i][3]);

        // Surface pixels per SCREEN pixel, so the cursor keeps the same relative size in
        // the headset that it has on the desktop. Sizing it in surface pixels instead
        // would make a 32-pixel arrow invisible the moment the off-screen target goes to
        // 3172 lines an eye.
        float scale = static_cast<float>(rects[i][3]) / static_cast<float>(clientH);
        if (g_image.height * scale < 8.0f)
            scale = 8.0f / static_cast<float>(g_image.height);
        drawnSize = g_image.height * scale;
        quads[i].left = drawnX[i] - g_image.hotX * scale;
        quads[i].top = drawnY[i] - g_image.hotY * scale;
        quads[i].right = quads[i].left + g_image.width * scale;
        quads[i].bottom = quads[i].top + g_image.height * scale;
    }

    // Save what is under each cursor BEFORE anything is drawn over it.
    //
    // Done unconditionally, for the off-screen world frame as well as the menu. The FBO
    // is redrawn by the scene render every frame and almost certainly does not need
    // this, but "almost certainly" is not a thing to hang a conditional on, and a 32x32
    // texture copy is far cheaper than the run it would cost to find out.
    //
    // The texture names are kept across the reset; only the rects are invalidated.
    //
    // A still-armed save here means the previous one was never put back, which would be
    // the trail again. It cannot happen while every draw is paired with a restore in the
    // same block -- so if it ever does, a later reordering broke the pairing, and that
    // has to be loud rather than a mystery.
    if (g_pending.armed && g_cursor.orphanReportsLeft > 0) {
        --g_cursor.orphanReportsLeft;
        logf("cursor: a saved backing-store rect was never restored before the next draw "
             "-- the pointer will leave a trail. Every cursorOverlayDraw must be followed "
             "by a cursorOverlayRestore in the same frame%s",
             g_cursor.orphanReportsLeft ? "" : " (not reported again)");
    }
    g_pending.count = 0;
    g_pending.armed = false;
    g_pending.framebuffer = framebuffer;
    g_pending.slot[0].held = false;
    g_pending.slot[1].held = false;
    for (int i = 0; i < 4; ++i) g_pending.viewport[i] = viewport[i];

    // Which buffer the copy reads, said rather than inherited. GL_BACK is not a legal
    // read buffer for a framebuffer object -- that exact mistake cost three runs in the
    // frame transfer -- so the two cases are named separately.
    if (gl.readBuffer)
        gl.readBuffer(framebuffer ? kGlColorAttachment0 : static_cast<GLenum>(GL_BACK));

    for (int i = 0; i < count; ++i) {
        if (saveUnderCursor(gl, g_pending.slot[i], quads[i], viewport)) {
            g_pending.count = count;
            g_pending.armed = true;
        }
    }

    if (!g_cursor.loggedSave && g_pending.armed) {
        g_cursor.loggedSave = true;
        logf("cursor: backing store ON -- %d rect(s) saved out of framebuffer %u before "
             "drawing, the first %dx%d at (%d,%d) in a %dx%d texture. GPL's menu does "
             "not redraw the whole back buffer and roglv2 preserves it across the swap, "
             "so without this the pointer paints a trail. It is put back after the frame "
             "goes to the headset, which is why the FLAT WINDOW no longer shows the "
             "overlay -- Windows already draws the real pointer there.",
             g_pending.count, g_pending.framebuffer, g_pending.slot[0].width,
             g_pending.slot[0].height, g_pending.slot[0].x, g_pending.slot[0].y,
             g_pending.slot[0].texWidth, g_pending.slot[0].texHeight);
    }

    for (int i = 0; i < count; ++i) drawCursorImage(gl, quads[i]);

    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMatrixMode));

    noteRefusal(Refusal::None);

    // -- evidence ---------------------------------------------------------------------

    if (!g_cursor.loggedEntry) {
        g_cursor.loggedEntry = true;
        logf("cursor: the matrix mode on entry was 0x%04X (%s), and it is put back on "
             "exit -- roglv2 leaves GL_PROJECTION current and rogl_displaySetDestination "
             "depends on it",
             static_cast<unsigned>(savedMatrixMode),
             savedMatrixMode == GL_PROJECTION
                 ? "GL_PROJECTION, as expected"
                 : (savedMatrixMode == GL_MODELVIEW
                        ? "GL_MODELVIEW -- NOT what roglv2 is documented to leave"
                        : "neither PROJECTION nor MODELVIEW"));
    }

    const int sideBySideNow = sideBySide ? 1 : 0;
    const int eyeRectsNow = sideBySide ? (usedStereoRects ? 1 : 0) : -1;
    if ((framebuffer != g_cursor.lastFramebuffer || surfaceW != g_cursor.lastSurfaceW ||
         surfaceH != g_cursor.lastSurfaceH || clientW != g_cursor.lastClientW ||
         clientH != g_cursor.lastClientH || sideBySideNow != g_cursor.lastSideBySide ||
         eyeRectsNow != g_cursor.lastEyeRects) &&
        g_cursor.stateReportsLeft > 0) {
        --g_cursor.stateReportsLeft;
        g_cursor.lastFramebuffer = framebuffer;
        g_cursor.lastSurfaceW = surfaceW;
        g_cursor.lastSurfaceH = surfaceH;
        g_cursor.lastClientW = clientW;
        g_cursor.lastClientH = clientH;
        g_cursor.lastSideBySide = sideBySideNow;
        g_cursor.lastEyeRects = eyeRectsNow;
        logf("cursor: drawing the system cursor bitmap into framebuffer %u (%s), "
             "viewport %dx%d, from a %ldx%ld client area -- %s%s",
             framebuffer,
             framebuffer ? "the off-screen target" : "the window's own back buffer",
             surfaceW, surfaceH, clientW, clientH,
             sideBySide ? (usedStereoRects
                               ? "two eyes, one in each of the stereo eye rects"
                               : "two eyes, but the eye rects were unavailable so the "
                                 "halves of the viewport are used")
                        : "one image for both eyes, one cursor",
             g_cursor.stateReportsLeft
                 ? ""
                 : " (further layout changes will not be reported)");
    }

    // DWORD arithmetic, so the 49-day wrap subtracts correctly rather than producing a
    // gap of six weeks.
    const DWORD now = GetTickCount();
    if (g_cursor.positionReportsLeft > 0 &&
        (local.x != g_cursor.lastX || local.y != g_cursor.lastY) &&
        now - g_cursor.lastPositionReport >= 2000) {
        --g_cursor.positionReportsLeft;
        g_cursor.lastPositionReport = now;
        g_cursor.lastX = local.x;
        g_cursor.lastY = local.y;

        char second[64] = {};
        if (count > 1)
            _snprintf_s(second, sizeof(second), _TRUNCATE, " and (%.0f, %.0f)", drawnX[1],
                        drawnY[1]);
        logf("cursor: screen (%ld, %ld) -> client (%ld, %ld) -> surface (%.0f, %.0f)%s, "
             "%.0f px tall, Windows says the pointer is %s",
             screen.x, screen.y, local.x, local.y, drawnX[0], drawnY[0], second,
             drawnSize, showing ? "showing" : (asked ? "hidden" : "unknown"));
    } else if (g_cursor.positionReportsLeft <= 0 && !g_cursor.saidPositionReportsEnd) {
        g_cursor.saidPositionReportsEnd = true;
        logf("cursor: that is enough pointer positions -- the mapping is either right by "
             "now or it is not, and the rest would only be noise");
    }

    // Every frame that draws, without exception. A check that ran on a sample of frames
    // would drain the error on those and let the rest inherit it, which in this project
    // produced one good frame and then black from an instrument meant to be passive.
    const GLenum error = gl.getError();
    if (error != g_cursor.lastError) {
        g_cursor.lastError = error;
        if (error != GL_NO_ERROR && g_cursor.errorReportsLeft > 0) {
            --g_cursor.errorReportsLeft;
            logf("cursor: GL error 0x%04X came out of drawing the overlay -- the errors "
                 "were drained first, so this one is ours%s",
                 static_cast<unsigned>(error),
                 g_cursor.errorReportsLeft
                     ? ""
                     : " (further GL errors from the overlay will not be reported)");
        }
    }
    if (error == GL_NO_ERROR && !g_cursor.loggedCleanDraw) {
        g_cursor.loggedCleanDraw = true;
        logf("cursor: the overlay drew with no GL error, and the state it touched was "
             "pushed and popped around it");
    }
}

}  // namespace gplvr
