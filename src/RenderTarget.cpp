#include "RenderTarget.h"

#include "FrameBudget.h"
#include "Log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

typedef int(__cdecl* BitmapWidthFn)(int handle);
typedef int(__cdecl* BitmapHeightFn)(int handle);
typedef int(__cdecl* ScreenGetBackBufferFn)();

struct Entries {
    void** bitmapWidthSlot = nullptr;
    void** bitmapHeightSlot = nullptr;
    void** backBufferSlot = nullptr;
};

Entries g_entries;

// roglv2's screenDisplay, as it was BEFORE the frame counter replaced the global. The
// display object is decoded out of its code, and the global now points at our own hook,
// which contains no such reference.
//
// DELIBERATELY NOT IN Entries. installFrameCounter runs BEFORE renderTargetBind, and
// renderTargetBind opens with `g_entries = Entries{}` -- so anything handed over early
// and kept in there is wiped a moment later. That cost a run, and the failure was silent
// because the resolver's early exits said nothing. Both are fixed: this lives outside the
// struct that gets reset, and every exit below reports itself.
const void* g_screenDisplay = nullptr;

struct Surface {
    // The address of roglv2's handle-table POINTER, decoded out of the exported
    // bitmapWidth. Not the object: the object is reached through the table every time,
    // so a display recreated behind our back is followed rather than dangling.
    void* const* handleTable = nullptr;
    bool         resolved = false;
    bool         gaveUp = false;

    // The size the engine reported before we last overrode it. Re-learned whenever the
    // fields hold something other than what we wrote, which is how a mode change is
    // noticed without a hook on the mode change.
    int nativeWidth = 0;
    int nativeHeight = 0;
    int appliedWidth = 0;
    int appliedHeight = 0;
};

Surface g_surface;

// roglv2's DisplayOpenGL, and its width and height at +12/+16.
//
// These are NOT the same as the bitmap's, and the difference is the whole reason the
// first working picture came up distorted. rogl_displaySetDestination -- the vtable slot
// polyDraw calls on every destination change -- builds its ortho from the DISPLAY:
//
//     glOrtho(-dest.originX, display.width  - dest.originX,
//              display.height - dest.originY, -dest.originY, 0, 1);
//
// so vertex space spans the display's size while the viewport spans the framebuffer's.
// Leave the display at the mode's 2560x1440 and render into a 2536x1268 target and every
// vertex is squeezed by 2536/2560 across and 1268/1440 down -- a 12% vertical crush, and
// the second eye's origin lands twelve pixels early so its left edge spills into the
// first eye's half. Both were seen, exactly.
struct Display {
    void* const* pointer = nullptr;  // &rogl_displayCurrent, not the object
    bool         resolved = false;
    bool         gaveUp = false;
    int          nativeWidth = 0;
    int          nativeHeight = 0;
    int          appliedWidth = 0;
    int          appliedHeight = 0;
};

Display g_display;

struct Target {
    GLuint framebuffer = 0;
    GLuint colour = 0;
    GLuint depth = 0;
    // A texture belonging to somebody else, attached in place of `colour` for a frame that
    // renders straight into it. 0 means the target's own.
    GLuint borrowed = 0;
    int    width = 0;
    int    height = 0;

    int  requestedWidth = 0;
    int  requestedHeight = 0;
    bool resolved = false;
    bool gaveUp = false;
    bool bound = false;
    bool active = false;
    bool inMenuMode = false;
    bool loggedMonitor = false;
    bool splitBinding = false;   // GL_READ_FRAMEBUFFER / GL_DRAW_FRAMEBUFFER understood
    bool checkedSplit = false;
    bool verifiedBinding = false;
};

Target g_target;

// --- reaching the back buffer's width and height ------------------------------------
//
// bitmapWidth is four instructions:
//
//     A1 <imm32>        mov eax, [rogl_handleTable]
//     8B 48 08          mov ecx, [eax+8]
//     8B 04 CA          mov eax, [ecx+edx*8]      ; edx = the handle
//     8B 40 0C          mov eax, [eax+0Ch]        ; +12 = width, +16 = height
//
// and rogl_clipRectToBitmap clamps against the same two fields, which is what makes
// writing them enough. We want that imm32, and we take it the way everything else in
// this project takes an address: by scanning, with a cardinality assertion, and then
// proving it before using it.
//
// Every `A1 imm32` in the function is treated as a candidate -- there are two, because
// the SEH prologue starts `64 A1 00000000` -- and each is walked to a width and a height
// and checked against what the exported accessors actually return. Exactly one candidate
// must survive. A wrong guess cannot crash: every dereference goes through readable().

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD readableFlags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readableFlags)) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    const auto start = reinterpret_cast<uintptr_t>(info.BaseAddress);
    const auto end = start + info.RegionSize;
    const auto want = reinterpret_cast<uintptr_t>(address);
    return want >= start && want + bytes <= end;
}

template <typename T>
bool readAt(const void* address, T& out) {
    if (!readable(address, sizeof(T))) return false;
    std::memcpy(&out, address, sizeof(T));
    return true;
}

// table -> entries -> object, then one of the two size fields.
bool sizeFieldAddress(void* const* handleTable, int handle, unsigned offset,
                      int*& out) {
    void* manager = nullptr;
    if (!readAt(handleTable, manager) || !manager) return false;

    void* entries = nullptr;
    if (!readAt(static_cast<const unsigned char*>(manager) + 8, entries) || !entries)
        return false;

    void* object = nullptr;
    if (!readAt(static_cast<const unsigned char*>(entries) +
                    static_cast<size_t>(handle) * 8,
                object) ||
        !object)
        return false;

    auto field = reinterpret_cast<int*>(static_cast<unsigned char*>(object) + offset);
    if (!readable(field, sizeof(int))) return false;
    out = field;
    return true;
}

int backBufferHandle() {
    if (!g_entries.backBufferSlot || !*g_entries.backBufferSlot) return -1;
    auto fn = reinterpret_cast<ScreenGetBackBufferFn>(*g_entries.backBufferSlot);
    return fn ? fn() : -1;
}

bool engineSize(int handle, int& width, int& height) {
    if (!g_entries.bitmapWidthSlot || !*g_entries.bitmapWidthSlot) return false;
    if (!g_entries.bitmapHeightSlot || !*g_entries.bitmapHeightSlot) return false;
    width = reinterpret_cast<BitmapWidthFn>(*g_entries.bitmapWidthSlot)(handle);
    height = reinterpret_cast<BitmapHeightFn>(*g_entries.bitmapHeightSlot)(handle);
    return width > 0 && height > 0;
}

bool resolveSurface() {
    if (g_surface.resolved) return true;
    if (g_surface.gaveUp) return false;

    if (!g_entries.bitmapWidthSlot || !*g_entries.bitmapWidthSlot) return false;
    const int handle = backBufferHandle();
    if (handle < 0) return false;

    int engineW = 0;
    int engineH = 0;
    if (!engineSize(handle, engineW, engineH)) return false;

    const auto code = static_cast<const unsigned char*>(*g_entries.bitmapWidthSlot);
    if (!readable(code, 48)) {
        g_surface.gaveUp = true;
        logf("target: bitmapWidth at %p is not readable -- the off-screen path is off",
             (void*)code);
        return false;
    }

    void* const* found = nullptr;
    int          matches = 0;
    for (int i = 0; i + 5 <= 48; ++i) {
        if (code[i] != 0xA1) continue;

        void* const* candidate = nullptr;
        std::memcpy(&candidate, code + i + 1, sizeof(candidate));
        if (!candidate) continue;

        int* widthField = nullptr;
        int* heightField = nullptr;
        if (!sizeFieldAddress(candidate, handle, 12, widthField)) continue;
        if (!sizeFieldAddress(candidate, handle, 16, heightField)) continue;
        if (*widthField != engineW || *heightField != engineH) continue;

        ++matches;
        found = candidate;
    }

    if (matches != 1) {
        g_surface.gaveUp = true;
        logf("target: %d candidates in bitmapWidth reach the back buffer's %dx%d, "
             "expected exactly 1 -- the off-screen path is off",
             matches, engineW, engineH);
        return false;
    }

    g_surface.handleTable = found;
    g_surface.resolved = true;
    g_surface.nativeWidth = engineW;
    g_surface.nativeHeight = engineH;
    logf("target: the back buffer's size fields reached through %p; the engine reports "
         "%dx%d and the fields agree",
         (void*)found, engineW, engineH);
    return true;
}

// Two globals hold something display-shaped, and only one of them is the display.
//
//   dword_20080820  the ACTIVE POLY DRAWER. With RasterV2:DrawMethod = 1 -- the default
//                   -- the DisplayOpenGL constructor builds a PolyDrawerZBufferOpenGL,
//                   keeps the display at its +184, and installs itself here. polyDraw
//                   and polySetDest both go through it.
//   dword_20080808  the DisplayOpenGL itself, whose +12/+16 are the width and height
//                   rogl_displaySetDestination builds its ortho from.
//
// The first attempt scanned polySetDest, which only ever mentions the poly drawer, and
// read its +12/+16 as a size: "4x70386688". The chain matters --
// PolyDrawerZBufferOpenGL::SetDestination calls the display's own slot 0 with a NULL
// destination, and rogl_displaySetDestination then falls back to a1[7] and builds
// glOrtho(0, display.width, display.height, 0, 0, 1). So the DISPLAY is what has to be
// told the new size.
//
// screenDisplay mentions both, seven instructions apart, so scanning it and keeping the
// candidate whose size matches the viewport picks the display and rejects the drawer --
// by measurement rather than by knowing which is which.
bool resolveDisplay(int expectWidth, int expectHeight) {
    if (g_display.resolved) return true;
    if (g_display.gaveUp) return false;

    // Loud, both of them. A resolver that fails silently turns into "the off-screen path
    // is off" with no reason attached, which is exactly how the last run was spent.
    if (!g_screenDisplay) {
        g_display.gaveUp = true;
        logf("target: roglv2's original screenDisplay was never handed over, so the "
             "display object cannot be found -- the off-screen path is off");
        return false;
    }
    if (expectWidth <= 0 || expectHeight <= 0) {
        g_display.gaveUp = true;
        logf("target: the viewport reads %dx%d, so there is nothing to match the display "
             "against -- the off-screen path is off",
             expectWidth, expectHeight);
        return false;
    }

    const auto code = static_cast<const unsigned char*>(g_screenDisplay);
    if (!readable(code, 0x60)) {
        g_display.gaveUp = true;
        logf("target: screenDisplay at %p is not readable -- the display size cannot be "
             "matched to the off-screen target and the picture would only be distorted, "
             "so the off-screen path is off",
             (void*)code);
        return false;
    }

    // Report every candidate and what it holds, not just how many survived.
    //
    // The first version verified the display's size against the BACK BUFFER's and
    // rejected the only candidate there is, which produced "0 candidates" and no way to
    // tell whether the scan, the walk or the expectation was wrong. It was the
    // expectation. A count is not a measurement.
    void* const* found = nullptr;
    int          candidates = 0;
    int          matches = 0;
    for (int i = 0; i + 6 <= 0x60; ++i) {
        if (code[i] != 0x8B || code[i + 1] != 0x0D) continue;

        void* const* candidate = nullptr;
        std::memcpy(&candidate, code + i + 2, sizeof(candidate));
        if (!candidate) continue;
        ++candidates;

        void* display = nullptr;
        void* vtable = nullptr;
        int   width = -1;
        int   height = -1;
        const bool walked =
            readAt(candidate, display) && display && readAt(display, vtable) && vtable &&
            readAt(static_cast<const unsigned char*>(display) + 12, width) &&
            readAt(static_cast<const unsigned char*>(display) + 16, height);

        logf("target: screenDisplay+%02X reads %p -> object %p, vtable %p, size %dx%d "
             "(looking for %dx%d)",
             i, (void*)candidate, display, vtable, width, height, expectWidth,
             expectHeight);

        if (!walked) continue;
        if (width != expectWidth || height != expectHeight) continue;

        ++matches;
        found = candidate;
    }

    if (matches != 1) {
        g_display.gaveUp = true;
        logf("target: %d of %d candidates in screenDisplay reach a display of %dx%d, "
             "expected exactly 1 -- the off-screen path is off",
             matches, candidates, expectWidth, expectHeight);
        return false;
    }

    g_display.pointer = found;
    g_display.resolved = true;
    g_display.nativeWidth = expectWidth;
    g_display.nativeHeight = expectHeight;
    logf("target: the display's size fields reached through %p; it reports %dx%d, "
         "matching the viewport roglv2 set",
         (void*)found, g_display.nativeWidth, g_display.nativeHeight);
    return true;
}

// The display object is re-read every time: roglv2 builds a new one on every mode change,
// and the pointer we hold is the GLOBAL, not the object.
bool displaySizeFields(int*& width, int*& height) {
    if (!g_display.resolved) return false;

    void* display = nullptr;
    if (!readAt(g_display.pointer, display) || !display) return false;

    auto w = reinterpret_cast<int*>(static_cast<unsigned char*>(display) + 12);
    auto h = reinterpret_cast<int*>(static_cast<unsigned char*>(display) + 16);
    if (!readable(w, sizeof(int)) || !readable(h, sizeof(int))) return false;
    width = w;
    height = h;
    return true;
}

bool applyDisplaySize(int width, int height, int expectWidth, int expectHeight) {
    if (!resolveDisplay(expectWidth, expectHeight)) return false;

    int* widthField = nullptr;
    int* heightField = nullptr;
    if (!displaySizeFields(widthField, heightField)) return false;

    if (*widthField != g_display.appliedWidth ||
        *heightField != g_display.appliedHeight) {
        g_display.nativeWidth = *widthField;
        g_display.nativeHeight = *heightField;
    }

    *widthField = width;
    *heightField = height;
    g_display.appliedWidth = width;
    g_display.appliedHeight = height;
    return true;
}

// UNDO ONLY WHAT WE WROTE, and only if it is still there.
//
// The engine rebuilds its display on every mode change, and this runs on the frame after
// one -- so the fields may already hold the new mode's size rather than ours. Writing the
// remembered "native" size then stamps the OLD mode over a freshly built display: a
// 2560-wide ortho into a 640-wide viewport, only part of the buffer written, and
// alternate presents showing different stale content. That is what the menu flickering
// was.
void restoreDisplaySize() {
    if (!g_display.resolved || !g_display.appliedWidth) return;

    int* widthField = nullptr;
    int* heightField = nullptr;
    if (!displaySizeFields(widthField, heightField)) {
        g_display.appliedWidth = 0;
        g_display.appliedHeight = 0;
        return;
    }

    const bool stillOurs = *widthField == g_display.appliedWidth &&
                           *heightField == g_display.appliedHeight;
    if (stillOurs && g_display.nativeWidth > 0 && g_display.nativeHeight > 0) {
        *widthField = g_display.nativeWidth;
        *heightField = g_display.nativeHeight;
    }
    g_display.appliedWidth = 0;
    g_display.appliedHeight = 0;
}

// What the ENGINE believes the back buffer is, adopting anything that is not our own
// last write as the new native size. That is how a mode change is picked up: roglv2
// rebuilds its display, the fields hold the new mode, and we learn it without hooking
// the rebuild.
bool learnNativeSize(int& width, int& height) {
    if (!resolveSurface()) return false;

    const int handle = backBufferHandle();
    if (handle < 0) return false;

    int* widthField = nullptr;
    int* heightField = nullptr;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 12, widthField)) return false;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 16, heightField)) return false;

    if (*widthField != g_surface.appliedWidth ||
        *heightField != g_surface.appliedHeight) {
        if (*widthField != g_surface.nativeWidth ||
            *heightField != g_surface.nativeHeight) {
            logf("target: the engine's back buffer is now %dx%d (was %dx%d)", *widthField,
                 *heightField, g_surface.nativeWidth, g_surface.nativeHeight);
        }
        g_surface.nativeWidth = *widthField;
        g_surface.nativeHeight = *heightField;
    }

    width = g_surface.nativeWidth;
    height = g_surface.nativeHeight;
    return width > 0 && height > 0;
}

// 640x480 is the MENU, and the menu is left alone.
//
// GPL runs its front end in a 640x480 mode and the sim in the desktop mode -- roglv2
// makes the same distinction itself, in rogl_screenSetMode, which raises a flag for
// every mode that is not exactly 640x480. The menu's art and its layout are in surface
// pixels at that size, so enlarging the surface underneath it would leave the whole
// front end in the corner of a much larger buffer at its native pixel size, and the
// player could not reach the track to see the thing this was built for.
bool isMenuMode(int width, int height) { return width == 640 && height == 480; }

// Write the two fields. Returns false when nothing could be written, which is what
// turns the whole path off.
bool applySurfaceSize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (!resolveSurface()) return false;

    const int handle = backBufferHandle();
    if (handle < 0) return false;

    int* widthField = nullptr;
    int* heightField = nullptr;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 12, widthField)) return false;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 16, heightField)) return false;

    *widthField = width;
    *heightField = height;
    g_surface.appliedWidth = width;
    g_surface.appliedHeight = height;
    return true;
}

void restoreSurfaceSize() {
    if (!g_surface.resolved) return;
    if (!g_surface.nativeWidth || !g_surface.nativeHeight) return;
    if (!g_surface.appliedWidth) return;

    const int handle = backBufferHandle();
    if (handle < 0) return;

    int* widthField = nullptr;
    int* heightField = nullptr;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 12, widthField)) return;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 16, heightField)) return;

    // Same rule as restoreDisplaySize: only undo what is still ours. A mode change
    // between the write and this call means the engine has already put its own size
    // there, and the remembered one is a whole mode out of date.
    const bool stillOurs = *widthField == g_surface.appliedWidth &&
                           *heightField == g_surface.appliedHeight;
    if (stillOurs) {
        *widthField = g_surface.nativeWidth;
        *heightField = g_surface.nativeHeight;
    }
    g_surface.appliedWidth = 0;
    g_surface.appliedHeight = 0;
}

// --- the framebuffer ------------------------------------------------------------------

// Does this driver understand the GL 3.0 split of the framebuffer binding into a READ
// and a DRAW target?
//
// The window blit is the one operation that genuinely needs it -- read from the
// off-screen frame, draw to the window, both at once. Everything else uses plain
// GL_FRAMEBUFFER. Asked once, by binding and reading back, because a bind that fails
// with INVALID_ENUM leaves the previous binding in place and then quietly blits the
// window to itself: black, with no call having returned an error the caller checked.
bool splitBindingWorks(const GlFunctions& gl, GLuint framebuffer) {
    if (!gl.bindFramebuffer || !gl.getIntegerv) return false;

    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}

    GLint bound = 0;
    gl.bindFramebuffer(kGlReadFramebuffer, framebuffer);
    gl.getIntegerv(kGlReadFramebufferBinding, &bound);
    const bool took = static_cast<GLuint>(bound) == framebuffer &&
                      gl.getError() == GL_NO_ERROR;
    gl.bindFramebuffer(kGlReadFramebuffer, 0);
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}
    return took;
}

void destroyFramebuffer(const GlFunctions& gl) {
    if (g_target.framebuffer && gl.deleteFramebuffers)
        gl.deleteFramebuffers(1, &g_target.framebuffer);
    if (g_target.depth && gl.deleteRenderbuffers)
        gl.deleteRenderbuffers(1, &g_target.depth);
    if (g_target.colour && gl.deleteTextures) gl.deleteTextures(1, &g_target.colour);

    g_target.framebuffer = 0;
    g_target.depth = 0;
    g_target.colour = 0;
    g_target.borrowed = 0;
    g_target.width = 0;
    g_target.height = 0;
    g_target.bound = false;
    g_target.active = false;
}

// What this driver will actually allow, asked once and reported.
//
// Added after vrRenderScale 1.5 (9516x4758) rendered into the top-left corner and then
// crashed, while 1.25 (7930x3965) was fine. The point is not the guess -- it is that the
// log could not distinguish "GL refused the size" from "the size was accepted and
// something downstream mis-laid it out", and those need opposite fixes. Now it says.
bool sizeWithinGlLimits(const GlFunctions& gl, int width, int height) {
    if (!gl.getIntegerv) return true;  // cannot ask; do not invent a refusal

    GLint maxTexture = 0;
    GLint maxRenderbuffer = 0;
    GLint maxViewport[2] = {0, 0};
    gl.getIntegerv(0x0D33 /* GL_MAX_TEXTURE_SIZE */, &maxTexture);
    gl.getIntegerv(0x84E8 /* GL_MAX_RENDERBUFFER_SIZE */, &maxRenderbuffer);
    gl.getIntegerv(0x0D3A /* GL_MAX_VIEWPORT_DIMS */, maxViewport);

    static bool reported = false;
    if (!reported) {
        reported = true;
        logf("target: this driver allows textures to %d, renderbuffers to %d and "
             "viewports to %dx%d",
             maxTexture, maxRenderbuffer, maxViewport[0], maxViewport[1]);
    }

    const int longest = width > height ? width : height;
    int       limit = 0;
    const char* which = nullptr;
    if (maxTexture && longest > maxTexture) {
        limit = maxTexture;
        which = "a texture";
    } else if (maxRenderbuffer && longest > maxRenderbuffer) {
        limit = maxRenderbuffer;
        which = "a renderbuffer";
    } else if (maxViewport[0] && (width > maxViewport[0] || height > maxViewport[1])) {
        limit = maxViewport[0] < maxViewport[1] ? maxViewport[0] : maxViewport[1];
        which = "a viewport";
    }
    if (!which) return true;

    // Say what would fit, because the only useful response is to lower vrRenderScale and
    // the reader should not have to do the arithmetic. The target is two eyes wide, so
    // the per-eye ceiling is half the limit on the width and the whole of it on height.
    logf("target: %dx%d is past what this driver allows as %s (%d) -- staying on the "
         "window's back buffer, which costs resolution but nothing else. The most that "
         "fits is %d per eye across and %d down; lower vrRenderScale to about %.2f of "
         "what you asked for.",
         width, height, which, limit, limit / 2, limit,
         width > 0 ? static_cast<double>(limit) / width : 0.0);
    return false;
}

bool createFramebuffer(const GlFunctions& gl, int width, int height) {
    destroyFramebuffer(gl);

    // Before allocating 180 MB and finding out the hard way.
    if (!sizeWithinGlLimits(gl, width, height)) return false;

    gl.genTextures(1, &g_target.colour);
    gl.bindTexture(GL_TEXTURE_2D, g_target.colour);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, nullptr);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    gl.bindTexture(GL_TEXTURE_2D, 0);

    // 24-bit depth, where the window's own buffer is 16: DisplayOpenGL asks for
    // cDepthBits = 16 once for the whole process, and an FBO is not bound by that. Free
    // precision, and GPL's draw distance is long enough to notice.
    gl.genRenderbuffers(1, &g_target.depth);
    gl.bindRenderbuffer(kGlRenderbuffer, g_target.depth);
    gl.renderbufferStorage(kGlRenderbuffer, kGlDepthComponent24, width, height);
    gl.bindRenderbuffer(kGlRenderbuffer, 0);

    gl.genFramebuffers(1, &g_target.framebuffer);
    gl.bindFramebuffer(kGlFramebuffer, g_target.framebuffer);
    gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                            g_target.colour, 0);
    gl.framebufferRenderbuffer(kGlFramebuffer, kGlDepthAttachment, kGlRenderbuffer,
                               g_target.depth);

    const GLenum status = gl.checkFramebufferStatus(kGlFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, 0);

    if (status != kGlFramebufferComplete) {
        logf("target: the %dx%d off-screen framebuffer is incomplete (0x%04X) -- "
             "falling back to the window's back buffer",
             width, height, status);
        destroyFramebuffer(gl);
        return false;
    }

    g_target.width = width;
    g_target.height = height;
    g_target.verifiedBinding = false;

    if (!g_target.checkedSplit) {
        g_target.checkedSplit = true;
        g_target.splitBinding = splitBindingWorks(gl, g_target.framebuffer);
        if (!g_target.splitBinding)
            logf("target: this driver does not accept GL_READ_FRAMEBUFFER, so the game "
                 "window cannot show the off-screen frame -- the headset is unaffected");
    }

    logf("target: rendering off-screen at %dx%d (%.1f MB colour, 24-bit depth, split "
         "binding %s)",
         width, height, static_cast<double>(width) * height * 4.0 / (1024.0 * 1024.0),
         g_target.splitBinding ? "yes" : "NO");
    return true;
}

// roglv2's own state setup, reproduced -- INCLUDING THE ORDER, which is load-bearing.
//
// The 0.375 translate is a pixel-centre convention the whole rasterizer is written
// against, and dropping it would shift every polygon by half a pixel. That much was
// obvious. What was not:
//
// rogl_displaySetupGlState does MODELVIEW first and PROJECTION second, so it LEAVES THE
// MATRIX MODE AT GL_PROJECTION -- and DisplayOpenGL::SetDestination, the vtable slot
// polyDraw calls on every destination change, then does
//
//     glLoadIdentity();
//     glOrtho(-originX, width - originX, height - originY, -originY, 0, 1);
//
// with NO glMatrixMode of its own. The trailing mode is part of the contract between
// those two functions, and nothing in either says so.
//
// The first version of this function made the same calls in the opposite order and left
// GL_MODELVIEW current. Every SetDestination after that loaded its ortho into the
// MODELVIEW matrix instead, the transform became ortho times ortho, and every vertex
// landed far outside the clip volume. The signature was exact and took nine runs to
// corner: clears landed (matrices do not affect them), so the depth stamp went 0.5 -> 1.0
// and proved the framebuffer was bound and live, while 236,000 polygon submissions a
// second produced not one pixel.
void applyViewport(const GlFunctions& gl, int width, int height) {
    gl.viewport(0, 0, width, height);

    gl.matrixMode(GL_MODELVIEW);
    gl.loadIdentity();
    gl.translatef(0.375f, 0.375f, 0.0f);

    // LAST, and the mode is deliberately left here.
    gl.matrixMode(GL_PROJECTION);
    gl.loadIdentity();
    gl.ortho(0.0, static_cast<double>(width), static_cast<double>(height), 0.0, 0.0, 1.0);
}

}  // namespace

void renderTargetBind(const Binding& binding) {
    g_entries = Entries{};
    g_entries.bitmapWidthSlot = binding.raster.bitmapWidth;
    g_entries.bitmapHeightSlot = binding.raster.bitmapHeight;
    g_entries.backBufferSlot = binding.raster.screenGetBackBuffer;
}

void renderTargetSetScreenDisplay(const void* original) {
    g_screenDisplay = original;
    logf("target: roglv2's original screenDisplay is at %p, which is where the display "
         "object is decoded from",
         original);
}

bool renderTargetResolve(GlFunctions& gl) {
    g_target.resolved = glResolveFramebuffer(gl);
    if (!g_target.resolved)
        logf("target: framebuffer objects are unavailable, so the render size stays "
             "tied to the window");
    return g_target.resolved;
}

void renderTargetRequestSize(int width, int height) {
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    if (width == g_target.requestedWidth && height == g_target.requestedHeight) return;
    g_target.requestedWidth = width;
    g_target.requestedHeight = height;
    logf("target: asked to render at %dx%d", width, height);
}

void renderTargetBeginFrame(const GlFunctions& gl) {
    frameBudgetSwitch(kSpanRebindSize);
    const bool wanted = g_target.resolved && !g_target.gaveUp &&
                        g_target.requestedWidth > 0 && g_target.requestedHeight > 0;
    if (!wanted) {
        if (g_target.framebuffer || g_surface.appliedWidth) renderTargetRelease(gl);
        return;
    }

    // Ask the engine what it thinks its back buffer is before touching anything. This
    // also re-learns the native size across a mode change, which is the moment the
    // answer below flips.
    int nativeW = 0;
    int nativeH = 0;
    if (!learnNativeSize(nativeW, nativeH)) {
        // Two different silences. Before the rasterizer table is filled in and before
        // the first back buffer exists this simply is not ready yet, and giving up here
        // would make the off-screen path depend on which frame it was first asked
        // about. Only resolveSurface's own verdict is permanent.
        if (g_surface.gaveUp && !g_target.gaveUp) {
            g_target.gaveUp = true;
            logf("target: the engine's back buffer size could not be reached -- the "
                 "off-screen path is off and the window's own buffer is used");
        }
        destroyFramebuffer(gl);
        return;
    }

    if (isMenuMode(nativeW, nativeH)) {
        // Not a failure and not permanent: the next mode change puts us back in the sim.
        // Report the transition, not the state, or this is a line every frame.
        if (!g_target.inMenuMode) {
            g_target.inMenuMode = true;
            logf("target: the back buffer is %dx%d, which is the menu -- rendering "
                 "off-screen is suspended until the sim mode comes back",
                 nativeW, nativeH);
        }
        renderTargetRelease(gl);
        return;
    }
    if (g_target.inMenuMode) {
        g_target.inMenuMode = false;
        logf("target: the back buffer is %dx%d -- resuming off-screen rendering",
             nativeW, nativeH);
    }

    if (g_target.width != g_target.requestedWidth ||
        g_target.height != g_target.requestedHeight) {
        if (!createFramebuffer(gl, g_target.requestedWidth, g_target.requestedHeight)) {
            g_target.gaveUp = true;
            restoreSurfaceSize();
            return;
        }
    }

    // The engine has to believe the back buffer is this size BEFORE it lays out a frame
    // -- rogl_clipRectToBitmap clamps every eye instance against these two fields, and
    // gpl_cameraCreate snapshots them into the camera's half extents. If this fails
    // there is no point binding the framebuffer: the frame would be drawn at the old
    // size into the corner of a larger target.
    //
    // Skipped entirely when the diagnostic option asks for the framebuffer without the
    // rewrite, which is the one configuration that separates the two.
    // The DISPLAY has to agree too, and for a different reason than the bitmap: the
    // bitmap's size decides where the eye rects are carved, the display's decides the
    // ortho that rogl_displaySetDestination rebuilds on every destination change. Set one
    // without the other and the picture is geometrically WRONG rather than absent --
    // squeezed by the ratio between them, with the second eye's origin landing early
    // enough that its left edge spills into the first eye's half. Both were seen.
    // What roglv2 itself put in the viewport IS the display's size -- SetupGLState sets
    // it from those very fields and nothing else writes it until we do, a few lines
    // below. So this is true by construction, where the back buffer's size only happened
    // to look like it should match.
    GLint roglViewport[4] = {0, 0, 0, 0};
    frameBudgetSwitch(kSpanRebindQuery);
    if (gl.getIntegerv) gl.getIntegerv(0x0BA2 /* GL_VIEWPORT */, roglViewport);
    frameBudgetSwitch(kSpanRebindSize);

    if (!applyDisplaySize(g_target.width, g_target.height, roglViewport[2],
                          roglViewport[3])) {
        if (!g_target.gaveUp) {
            g_target.gaveUp = true;
            logf("target: the display's size could not be set, and rendering off-screen "
                 "without it only distorts the picture -- the off-screen path is off");
        }
        restoreSurfaceSize();
        destroyFramebuffer(gl);
        return;
    }

    if (!applySurfaceSize(g_target.width, g_target.height)) {
        if (!g_target.gaveUp) {
            g_target.gaveUp = true;
            logf("target: the engine's back buffer size could not be set -- the "
                 "off-screen path is off and the window's own buffer is used");
        }
        restoreDisplaySize();
        destroyFramebuffer(gl);
        return;
    }

    frameBudgetSwitch(kSpanRebindBind);
    gl.bindFramebuffer(kGlFramebuffer, g_target.framebuffer);

    // Read the binding back the first time, rather than trusting the call.
    //
    // A bind that is rejected leaves the previous binding untouched and reports nothing
    // to anyone who does not ask. The whole frame then goes to the window's own back
    // buffer while every downstream step reads from an empty off-screen target -- which
    // is a black headset AND a black window, with a GL error surfacing several calls
    // later on something innocent. Ask once, and refuse to continue if the answer is
    // wrong.
    if (!g_target.verifiedBinding) {
        GLint bound = -1;
        if (gl.getIntegerv) gl.getIntegerv(kGlFramebufferBinding, &bound);
        if (gl.getIntegerv && static_cast<GLuint>(bound) != g_target.framebuffer) {
            g_target.gaveUp = true;
            logf("target: bound framebuffer %u but the driver reports %d -- the "
                 "off-screen path is off and the window's own buffer is used",
                 g_target.framebuffer, bound);
            gl.bindFramebuffer(kGlFramebuffer, 0);
            restoreSurfaceSize();
            destroyFramebuffer(gl);
            return;
        }
        g_target.verifiedBinding = true;
        logf("target: the off-screen framebuffer is bound and the driver agrees (%u)",
             g_target.framebuffer);
    }

    // Re-asserted every frame on purpose. rogl_displaySetupGlState runs again whenever
    // roglv2 rebuilds its display -- every menu-to-sim transition -- and would put the
    // viewport and the ortho back to the window's size underneath us.
    frameBudgetSwitch(kSpanRebindViewport);
    applyViewport(gl, g_target.width, g_target.height);

    g_target.bound = true;
    g_target.active = true;
    frameBudgetSwitch(kSpanRebind);
}

void renderTargetEndFrame(const GlFunctions& gl) {
    if (!g_target.bound) return;
    gl.bindFramebuffer(kGlFramebuffer, 0);
    g_target.bound = false;
}

bool renderTargetActive() { return g_target.active && g_target.framebuffer != 0; }

GLuint renderTargetFramebuffer() {
    return renderTargetActive() ? g_target.framebuffer : 0;
}

bool renderTargetSize(int& width, int& height) {
    if (!renderTargetActive()) return false;
    width = g_target.width;
    height = g_target.height;
    return true;
}

bool renderTargetAttachColour(const GlFunctions& gl, GLuint name) {
    if (!g_target.framebuffer || !gl.bindFramebuffer || !gl.framebufferTexture2D)
        return false;

    GLuint wanted = name ? name : g_target.colour;
    if (!wanted) return false;

    // The binding is saved and put back rather than left at the target's own: this runs
    // both between frames, where nothing is bound, and in the middle of the extra-render
    // tail, where the target is bound and the frame after this one keeps drawing into it.
    GLint previous = 0;
    if (gl.getIntegerv) gl.getIntegerv(kGlFramebufferBinding, &previous);
    gl.bindFramebuffer(kGlFramebuffer, g_target.framebuffer);
    gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, wanted, 0);

    GLenum status = gl.checkFramebufferStatus ? gl.checkFramebufferStatus(kGlFramebuffer)
                                              : kGlFramebufferComplete;
    if (status != kGlFramebufferComplete) {
        // Put our own back before returning, so a refusal leaves a target that still
        // renders rather than one that is incomplete for the rest of the session.
        gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                                g_target.colour, 0);
        gl.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previous));
        g_target.borrowed = 0;
        logf("target: texture %u will not serve as colour attachment 0 of the %dx%d "
             "off-screen framebuffer (0x%04X) -- the target's own is attached again",
             name, g_target.width, g_target.height, status);
        return false;
    }

    gl.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previous));
    g_target.borrowed = name;
    return true;
}

GLuint renderTargetAttachedColour() { return g_target.borrowed; }

GLuint rasterBitmapGlTexture(int handle) {
    if (handle <= 0 || !resolveSurface()) return 0;

    // The same walk as sizeFieldAddress, then two more fields, every one of them read
    // through readable(). A handle mistaken for a pointer is what this function exists to
    // make impossible, and the mistake it replaces was an access violation on the first
    // frame (report 37 §1.3 gives the offsets for the OBJECT, which a handle is not).
    void* manager = nullptr;
    if (!readAt(g_surface.handleTable, manager) || !manager) return 0;

    void* entries = nullptr;
    if (!readAt(static_cast<const unsigned char*>(manager) + 8, entries) || !entries)
        return 0;

    void* object = nullptr;
    if (!readAt(static_cast<const unsigned char*>(entries) + static_cast<size_t>(handle) * 8,
                object) ||
        !object)
        return 0;

    // +32 is the ROOT bitmap, which for a top-level one is the object itself and for an
    // instance is the bitmap it is a sub-rect of. The GL name is the root's, at +84, which
    // is where rogl_blit_GLtoGL_copyTexSubImage reads it from. A root that is not a textured
    // bitmap has something else at +84 -- the display's is a heap tag -- so the caller has to
    // check the answer against what it expects rather than trust it.
    void* root = nullptr;
    if (!readAt(static_cast<const unsigned char*>(object) + 32, root) || !root) return 0;

    GLuint name = 0;
    if (!readAt(static_cast<const unsigned char*>(root) + 84, name)) return 0;
    return name;
}

void renderTargetOverrideSurface(void* surfaceHandle) {
    if (!renderTargetActive()) return;
    if (!g_surface.resolved) return;

    const int handle = static_cast<int>(reinterpret_cast<intptr_t>(surfaceHandle));
    if (handle < 0) return;

    int* widthField = nullptr;
    int* heightField = nullptr;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 12, widthField)) return;
    if (!sizeFieldAddress(g_surface.handleTable, handle, 16, heightField)) return;

    if (*widthField == g_target.width && *heightField == g_target.height) return;

    // Only a FULL-SCREEN view gets promoted.
    //
    // The racing lobby draws a TV feed into a 373x280 surface, and on the way out of a
    // race that surface arrived here one frame before the menu rule noticed the mode had
    // changed. Resizing it to the off-screen size made a menu viewport look exactly like
    // a world render to the eye-target code, and the front end went stereo. The same 7/8
    // margin Stereo uses to tell those apart, applied here so the two agree by
    // construction rather than by luck.
    if (*widthField * 8 < g_surface.nativeWidth * 7 ||
        *heightField * 8 < g_surface.nativeHeight * 7)
        return;

    static int lastFrom[2] = {0, 0};
    if (*widthField != lastFrom[0] || *heightField != lastFrom[1]) {
        lastFrom[0] = *widthField;
        lastFrom[1] = *heightField;
        logf("target: the view surface %d was %dx%d, resized to the off-screen %dx%d",
             handle, *widthField, *heightField, g_target.width, g_target.height);
    }
    *widthField = g_target.width;
    *heightField = g_target.height;
}

namespace {

// The window rect the last blit fitted into, so a full clear happens only when it moves.
struct WindowFit {
    int windowW = 0;
    int windowH = 0;
    int rect[4] = {0, 0, 0, 0};
    int reports = 8;
};

WindowFit g_fit;

// The picture the current frame's blit drew, for anything drawn over it afterwards.
WindowPicture g_picture;
bool          g_pictureValid = false;

void clearRect(const GlFunctions& gl, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    gl.scissor(x, y, w, h);
    gl.clear(GL_COLOR_BUFFER_BIT);
}

}  // namespace

bool renderTargetNativeSize(int& width, int& height) {
    if (g_surface.nativeWidth <= 0 || g_surface.nativeHeight <= 0) return false;
    width = g_surface.nativeWidth;
    height = g_surface.nativeHeight;
    return true;
}

bool renderTargetWindowPicture(WindowPicture& out) {
    if (!g_pictureValid) return false;
    out = g_picture;
    return true;
}

void renderTargetBlitToWindow(const GlFunctions& gl, int monitorEye) {
    g_pictureValid = false;
    if (!renderTargetActive()) return;

    // vrMonitorEye = 0: not one GL call into the window. roglv2 still presents it, so it
    // shows whatever its back buffer holds with pribluda's monitor overlay on top.
    if (monitorEye == 0) {
        if (!g_target.loggedMonitor) {
            g_target.loggedMonitor = true;
            logf("target: vrMonitorEye = 0 -- gplvr draws nothing into the game window. It "
                 "shows whatever its back buffer held, with pribluda's monitor overlay "
                 "drawn over it every frame and never wiped.");
        }
        return;
    }

    if (!gl.blitFramebuffer) return;
    // Without the read/draw split this would blit the window to itself.
    if (!g_target.splitBinding) return;

    int windowW = 0;
    int windowH = 0;
    if (!glDrawableSize(gl, windowW, windowH)) return;

    // Which part of the off-screen frame the window shows. The halves are the two eyes,
    // exactly as the presenter samples them, so picking one here needs no agreement with
    // anything else -- the split is the same split.
    const int halfW = g_target.width / 2;
    int       srcX = 0;
    int       srcW = g_target.width;
    if (monitorEye == 2) {
        srcX = 0;
        srcW = halfW;
    } else if (monitorEye == 3) {
        srcX = halfW;
        srcW = g_target.width - halfW;
    }
    if (srcW <= 0) return;

    // Aspect fit, never stretch. A single eye is roughly square and the window is not,
    // so a straight blit would make the picture visibly wrong in exactly the way this
    // whole phase was spent removing.
    const double scale =
        (static_cast<double>(windowW) / srcW < static_cast<double>(windowH) / g_target.height)
            ? static_cast<double>(windowW) / srcW
            : static_cast<double>(windowH) / g_target.height;
    const int dstW = static_cast<int>(srcW * scale);
    const int dstH = static_cast<int>(g_target.height * scale);
    const int dstX = (windowW - dstW) / 2;
    const int dstY = (windowH - dstH) / 2;

    gl.bindFramebuffer(kGlDrawFramebuffer, 0);
    gl.bindFramebuffer(kGlReadFramebuffer, g_target.framebuffer);
    gl.readBuffer(kGlColorAttachment0);

    // The scissor state and everything the clear touches -- clear colour, colour write
    // mask, draw buffer -- go back exactly as they were found. Pushed with the window's
    // framebuffer bound, so the draw buffer put back is the window's own.
    bool saved = gl.pushAttrib && gl.popAttrib;
    GLboolean scissorWas = GL_FALSE;
    if (saved)
        gl.pushAttrib(GL_COLOR_BUFFER_BIT | GL_SCISSOR_BIT);
    else if (gl.isEnabled)
        scissorWas = gl.isEnabled(GL_SCISSOR_TEST);

    bool refit = windowW != g_fit.windowW || windowH != g_fit.windowH ||
                 dstX != g_fit.rect[0] || dstY != g_fit.rect[1] || dstW != g_fit.rect[2] ||
                 dstH != g_fit.rect[3];
    if (refit) {
        // The state this blit inherits, read only when the fit changes so the state
        // queries stay off the per-frame path.
        GLint mask[4] = {-1, -1, -1, -1};
        GLint drawBuffer = 0;
        if (gl.getIntegerv) {
            gl.getIntegerv(GL_COLOR_WRITEMASK, mask);
            gl.getIntegerv(GL_DRAW_BUFFER, &drawBuffer);
        }
        bool scissorOn = gl.isEnabled && gl.isEnabled(GL_SCISSOR_TEST);
        if (g_fit.reports > 0) {
            --g_fit.reports;
            logf("target: the window picture is %dx%d at %d,%d in a %dx%d window -- the "
                 "whole window is cleared this frame and only the bars after. State found: "
                 "scissor %s, colour write mask %d%d%d%d, draw buffer 0x%04X. A closed "
                 "mask or a draw buffer other than GL_BACK (0x0405) keeps a clear from "
                 "reaching the bars.",
                 dstW, dstH, dstX, dstY, windowW, windowH, scissorOn ? "ON" : "off",
                 mask[0], mask[1], mask[2], mask[3], static_cast<unsigned>(drawBuffer));
        }
        g_fit.windowW = windowW;
        g_fit.windowH = windowH;
        g_fit.rect[0] = dstX;
        g_fit.rect[1] = dstY;
        g_fit.rect[2] = dstW;
        g_fit.rect[3] = dstH;
    }

    // THE LETTERBOX BARS ARE CLEARED EVERY FRAME, because of pribluda.
    //
    // In VR with the panel live, pribluda draws nothing into the window and the
    // panel is drawn inside the picture after this blit (presenterBlitToWindow), so the
    // bar-only clear is all the window needs. Otherwise:
    //
    // Its monitor overlay is drawn inside roglv2's present: pribluda's DllMain patches the
    // call at roglv2+0x9B83, between rogl_displayPresent's glFinish and its SwapBuffers. So
    // its text lands in this back buffer AFTER this blit, over the picture and over the
    // bars alike. The next blit replaces every pixel of the picture; nothing replaces the
    // bars, so any pixel there that a frame's clear does not reach keeps the text drawn
    // into it, and text that moves or changes leaves trails. roglv2's pixel format asks
    // for no PFD_SWAP_COPY either, so what the buffer holds after a swap is undefined.
    //
    // When the fit is unchanged, clearing the four bars is the same result as clearing
    // the window, for less fill. The write mask is opened because glClear honours it.
    if (dstW < windowW || dstH < windowH) {
        gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        if (saved && gl.colorMask) gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (refit || !saved || !gl.scissor || !gl.enable) {
            if (gl.disable) gl.disable(GL_SCISSOR_TEST);
            gl.clear(GL_COLOR_BUFFER_BIT);
        } else {
            gl.enable(GL_SCISSOR_TEST);
            clearRect(gl, 0, 0, dstX, windowH);                              // left
            clearRect(gl, dstX + dstW, 0, windowW - dstX - dstW, windowH);   // right
            clearRect(gl, dstX, 0, dstW, dstY);                              // bottom
            clearRect(gl, dstX, dstY + dstH, dstW, windowH - dstY - dstH);   // top
        }
    }

    // glBlitFramebuffer honours the scissor test too.
    if (gl.disable) gl.disable(GL_SCISSOR_TEST);

    // Both sides are GL framebuffers with the same bottom-left origin, so a straight
    // blit preserves the orientation -- no flip, and no need to know which way up the
    // engine thinks it is drawing.
    gl.blitFramebuffer(srcX, 0, srcX + srcW, g_target.height, dstX, dstY, dstX + dstW,
                       dstY + dstH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    g_picture.windowWidth = windowW;
    g_picture.windowHeight = windowH;
    g_picture.srcX = srcX;
    g_picture.srcWidth = srcW;
    g_picture.srcHeight = g_target.height;
    g_picture.dstX = dstX;
    g_picture.dstY = dstY;
    g_picture.dstWidth = dstW;
    g_picture.dstHeight = dstH;
    g_pictureValid = true;

    // Unbind BEFORE naming GL_BACK: GL_BACK is not a valid read buffer for a
    // framebuffer object, so the other order raises INVALID_OPERATION. The same mistake
    // was in the frame transfer and it took three runs to find there.
    gl.bindFramebuffer(kGlReadFramebuffer, 0);
    gl.readBuffer(GL_BACK);
    if (saved)
        gl.popAttrib();
    else if (scissorWas && gl.enable)
        gl.enable(GL_SCISSOR_TEST);

    if (!g_target.loggedMonitor) {
        g_target.loggedMonitor = true;
        logf("target: the game window is showing %s of the %dx%d off-screen frame, "
             "aspect-fitted into %dx%d",
             monitorEye == 2 ? "the LEFT eye"
                             : (monitorEye == 3 ? "the RIGHT eye" : "both eyes"),
             g_target.width, g_target.height, windowW, windowH);
    }
}

void renderTargetRelease(const GlFunctions& gl) {
    restoreSurfaceSize();
    if (g_target.bound && gl.bindFramebuffer) gl.bindFramebuffer(kGlFramebuffer, 0);
    destroyFramebuffer(gl);
    // The next blit clears the whole window again, whatever it held meanwhile.
    g_fit.windowW = 0;
    g_fit.windowH = 0;
    // inMenuMode is deliberately NOT reset here: this is called every frame while the
    // menu is up, and clearing the latch would turn one transition line into a pair of
    // them per frame.
    g_target.loggedMonitor = false;
}

}  // namespace gplvr
