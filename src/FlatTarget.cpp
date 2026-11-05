#include "FlatTarget.h"

#include "GlInterop.h"
#include "Hooks.h"
#include "Log.h"
#include "Window.h"

#include <windows.h>

namespace gplvr {
namespace {

struct State {
    GlFunctions gl;
    bool        resolved = false;
    bool        gaveUp = false;

    GLuint framebuffer = 0;
    GLuint colour = 0;
    GLuint depth = 0;
    int    width = 0;
    int    height = 0;

    bool bound = false;
    bool verifiedBinding = false;

    // Why the off-screen frame is or is not running, reported when the answer changes.
    const char* reason = nullptr;

    // The last fit logged, so a window that changes size says so once.
    int loggedWindowWidth = 0;
    int loggedWindowHeight = 0;

    // Where the last copy put the frame in the window, top-left origin, while it was scaled;
    // the pointer is mapped through the same four numbers. False at 1:1.
    bool scaled = false;
    int  destX = 0;
    int  destY = 0;
    int  destWidth = 0;
    int  destHeight = 0;

    unsigned long long copies = 0;
    int                errorReports = 4;
};

State g_state;

using SwapBuffersFn = BOOL(WINAPI*)(HDC);
SwapBuffersFn g_realSwapBuffers = nullptr;

// Pointer comparison: every caller passes a literal.
void reportState(const char* reason) {
    if (g_state.reason == reason) return;
    g_state.reason = reason;
    logf("flat frame: off-screen %s", reason);
}

void destroyFramebuffer() {
    State& s = g_state;
    if (s.framebuffer && s.gl.deleteFramebuffers) s.gl.deleteFramebuffers(1, &s.framebuffer);
    if (s.depth && s.gl.deleteRenderbuffers) s.gl.deleteRenderbuffers(1, &s.depth);
    if (s.colour && s.gl.deleteTextures) s.gl.deleteTextures(1, &s.colour);
    s.framebuffer = 0;
    s.depth = 0;
    s.colour = 0;
    s.width = 0;
    s.height = 0;
    s.bound = false;
    s.verifiedBinding = false;
    s.scaled = false;
}

// GL errors raised here must not reach roglv2: rogl_displayPresent follows its glFinish with
// `if (glGetError()) throw XOpenGL`, and the next frame's would take ours as its own.
void drainErrors(const char* where) {
    State& s = g_state;
    if (!s.gl.getError) return;
    for (int i = 0; i < 16; ++i) {
        GLenum error = s.gl.getError();
        if (error == GL_NO_ERROR) return;
        if (s.errorReports > 0) {
            --s.errorReports;
            logf("flat frame: GL error 0x%04X %s -- cleared so roglv2 does not throw on it",
                 error, where);
        }
    }
}

bool createFramebuffer(int width, int height) {
    State& s = g_state;
    destroyFramebuffer();

    // roglv2 keeps its own copy of the bound texture and does not rebind one it believes is
    // bound, so the binding found here goes back afterwards.
    GLint previousTexture = 0;
    if (s.gl.getIntegerv) s.gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    s.gl.genTextures(1, &s.colour);
    s.gl.bindTexture(GL_TEXTURE_2D, s.colour);
    s.gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                    nullptr);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    s.gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

    s.gl.genRenderbuffers(1, &s.depth);
    s.gl.bindRenderbuffer(kGlRenderbuffer, s.depth);
    s.gl.renderbufferStorage(kGlRenderbuffer, kGlDepthComponent24, width, height);
    s.gl.bindRenderbuffer(kGlRenderbuffer, 0);

    s.gl.genFramebuffers(1, &s.framebuffer);
    s.gl.bindFramebuffer(kGlFramebuffer, s.framebuffer);
    s.gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, s.colour, 0);
    s.gl.framebufferRenderbuffer(kGlFramebuffer, kGlDepthAttachment, kGlRenderbuffer, s.depth);
    GLenum status = s.gl.checkFramebufferStatus(kGlFramebuffer);

    // A new framebuffer starts black: GPL paints every pixel of a world frame, but the first
    // one is drawn over whatever the allocation held.
    if (status == kGlFramebufferComplete) {
        s.gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        s.gl.clear(GL_COLOR_BUFFER_BIT);
    }
    s.gl.bindFramebuffer(kGlFramebuffer, 0);
    drainErrors("building the framebuffer");

    if (status != kGlFramebufferComplete) {
        logf("flat frame: the %dx%d framebuffer is incomplete (0x%04X) -- the game draws into "
             "the window as before",
             width, height, status);
        destroyFramebuffer();
        return false;
    }
    s.width = width;
    s.height = height;
    logf("flat frame: the world is drawn into a %dx%d framebuffer and copied into the window "
         "at the swap",
         width, height);
    return true;
}

// The one write the window's back buffer gets in a frame: the finished frame, 1:1 when the
// window is the frame's size, aspect-fitted and linearly filtered when it is not (a window
// moved to another monitor mid-race), with the bars cleared.
void copyToWindow() {
    State& s = g_state;
    s.bound = false;

    int windowWidth = 0;
    int windowHeight = 0;
    if (!glDrawableSize(s.gl, windowWidth, windowHeight) || s.width <= 0 || s.height <= 0) {
        s.gl.bindFramebuffer(kGlFramebuffer, 0);
        drainErrors("unbinding without a window size");
        return;
    }

    bool exact = windowWidth == s.width && windowHeight == s.height;
    int  destX = 0;
    int  destY = 0;
    int  destWidth = windowWidth;
    int  destHeight = windowHeight;
    if (!exact) {
        double byWidth = static_cast<double>(windowWidth) / s.width;
        double byHeight = static_cast<double>(windowHeight) / s.height;
        double scale = byWidth < byHeight ? byWidth : byHeight;
        destWidth = static_cast<int>(s.width * scale);
        destHeight = static_cast<int>(s.height * scale);
        destX = (windowWidth - destWidth) / 2;
        destY = (windowHeight - destHeight) / 2;
    }
    s.scaled = !exact && destWidth > 0 && destHeight > 0;
    s.destX = destX;
    s.destY = destY;
    s.destWidth = destWidth;
    s.destHeight = destHeight;
    if (windowWidth != s.loggedWindowWidth || windowHeight != s.loggedWindowHeight) {
        s.loggedWindowWidth = windowWidth;
        s.loggedWindowHeight = windowHeight;
        if (exact)
            logf("flat frame: %dx%d copied 1:1 into the window", s.width, s.height);
        else
            logf("flat frame: %dx%d scaled to %dx%d at %d,%d in a %dx%d window -- the window "
                 "changed size since the race was entered; re-entering renders at its size",
                 s.width, s.height, destWidth, destHeight, destX, destY, windowWidth,
                 windowHeight);
    }

    s.gl.bindFramebuffer(kGlDrawFramebuffer, 0);
    s.gl.bindFramebuffer(kGlReadFramebuffer, s.framebuffer);

    // A blit honours the scissor test and a clear honours the write mask, and roglv2 or
    // pribluda may have left either set.
    bool      saved = s.gl.pushAttrib && s.gl.popAttrib;
    GLboolean scissorWas = GL_FALSE;
    if (saved)
        s.gl.pushAttrib(GL_COLOR_BUFFER_BIT | GL_SCISSOR_BIT);
    else if (s.gl.isEnabled)
        scissorWas = s.gl.isEnabled(GL_SCISSOR_TEST);
    if (s.gl.disable) s.gl.disable(GL_SCISSOR_TEST);

    if (!exact) {
        if (saved && s.gl.colorMask) s.gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        s.gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        s.gl.clear(GL_COLOR_BUFFER_BIT);
    }

    // Both sides count rows from the bottom; the fit is centred, so the offsets match.
    int glDestY = windowHeight - destY - destHeight;
    s.gl.blitFramebuffer(0, 0, s.width, s.height, destX, glDestY, destX + destWidth,
                         glDestY + destHeight, GL_COLOR_BUFFER_BIT,
                         exact ? GL_NEAREST : GL_LINEAR);

    if (saved)
        s.gl.popAttrib();
    else if (scissorWas && s.gl.enable)
        s.gl.enable(GL_SCISSOR_TEST);
    s.gl.bindFramebuffer(kGlReadFramebuffer, 0);
    drainErrors("copying the frame into the window");

    if (++s.copies == 1) logf("flat frame: first frame copied into the window at the swap");
}

BOOL WINAPI swapBuffersThunk(HDC dc) {
    if (g_state.bound) copyToWindow();
    return g_realSwapBuffers ? g_realSwapBuffers(dc) : FALSE;
}

}  // namespace

void flatTargetInstall() {
    HMODULE roglv2 = GetModuleHandleA("roglv2.dll");
    void*   original = nullptr;
    if (!roglv2 || !patchModuleImport(roglv2, "SwapBuffers",
                                      reinterpret_cast<void*>(&swapBuffersThunk), &original)) {
        logf("flat frame: roglv2's SwapBuffers import could not be hooked -- flatOffscreenFrame "
             "does nothing");
        return;
    }
    if (original) g_realSwapBuffers = reinterpret_cast<SwapBuffersFn>(original);
}

void flatTargetSuspend() {
    State& s = g_state;
    if (!s.resolved) return;
    if (s.bound && s.gl.bindFramebuffer) s.gl.bindFramebuffer(kGlFramebuffer, 0);
    destroyFramebuffer();
    drainErrors("dropping the framebuffer");
}

void flatTargetBeginFrame() {
    State& s = g_state;
    if (!g_options.flatOffscreenFrame || g_options.vr) {
        reportState("off -- flatOffscreenFrame = 0, or VR");
        if (s.framebuffer) flatTargetSuspend();
        return;
    }
    if (!g_realSwapBuffers) {
        reportState("off -- roglv2's SwapBuffers is not hooked");
        return;
    }
    if (s.gaveUp) {
        reportState("off -- a framebuffer failure earlier in the session");
        return;
    }
    if (windowInFrontEnd()) {
        reportState("off -- the front end (MenuScale scales it)");
        if (s.framebuffer) flatTargetSuspend();
        return;
    }

    if (!s.resolved) {
        if (!glResolve(s.gl) || !glResolveFramebuffer(s.gl) || !s.gl.blitFramebuffer) {
            s.gaveUp = true;
            logf("flat frame: framebuffer objects are unavailable -- the game draws into the "
                 "window as before");
            return;
        }
        s.resolved = true;
    }

    int modeWidth = 0;
    int modeHeight = 0;
    windowEngineModeSize(modeWidth, modeHeight);
    if (modeWidth <= 0 || modeHeight <= 0) return;

    if (s.width != modeWidth || s.height != modeHeight) {
        if (!createFramebuffer(modeWidth, modeHeight)) {
            s.gaveUp = true;
            return;
        }
    }

    s.gl.bindFramebuffer(kGlFramebuffer, s.framebuffer);

    // Read back once: a rejected bind leaves the window bound and says nothing, and the copy
    // at the swap would then put an empty framebuffer over a finished frame.
    if (!s.verifiedBinding) {
        GLint bound = -1;
        if (s.gl.getIntegerv) s.gl.getIntegerv(kGlFramebufferBinding, &bound);
        if (s.gl.getIntegerv && static_cast<GLuint>(bound) != s.framebuffer) {
            s.gaveUp = true;
            logf("flat frame: bound framebuffer %u but the driver reports %d -- the game draws "
                 "into the window as before",
                 s.framebuffer, bound);
            s.gl.bindFramebuffer(kGlFramebuffer, 0);
            destroyFramebuffer();
            drainErrors("after a rejected bind");
            return;
        }
        s.verifiedBinding = true;
    }
    s.bound = true;
    reportState("on");
}

bool flatTargetActive() { return g_state.bound; }

bool flatTargetMapClientPoint(POINT& point) {
    State& s = g_state;
    if (!s.framebuffer || !s.scaled || windowInFrontEnd()) return false;

    long x = (point.x - s.destX) * s.width / s.destWidth;
    long y = (point.y - s.destY) * s.height / s.destHeight;
    // A point over a bar is held off the frame, as the front end's mapping does, so a click
    // beside a button does not land on it.
    if (x < 0 || x > s.width - 1) x = -1;
    if (y < 0 || y > s.height - 1) y = -1;
    point.x = x;
    point.y = y;
    return true;
}

}  // namespace gplvr
