#include "MenuScale.h"

#include "FlatTarget.h"
#include "Hooks.h"
#include "Log.h"
#include "RenderTarget.h"
#include "Window.h"

namespace gplvr {
namespace {

// One binding latch per call site. Two sites ask the same question at different moments
// in the frame -- the scene render and the deferred flush inside the present -- and a
// single latch would see them alternate and turn report-on-change into report-per-frame.
const int kBindingSites = 4;

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
    bool active = false;
    bool checkedSplit = false;
    bool splitBinding = false;
    bool verifiedBinding = false;
    bool cleared = false;

    // The rectangle the front end occupies inside the window client area, top-left
    // origin -- the blit and the pointer are computed from these same four numbers, so
    // they cannot drift apart.
    int destX = 0;
    int destY = 0;
    int destWidth = 0;
    int destHeight = 0;

    int  loggedWindowWidth = 0;
    int  loggedWindowHeight = 0;

    const char* bindingSite[kBindingSites] = {};
    GLint       bindingSeen[kBindingSites] = {-2, -2, -2, -2};

    // Why scaling is or is not running, reported when the answer changes. Every other
    // state machine the front end passes through -- the engine mode, the menu/world
    // verdict, the surface promotion -- already says so on change; this one did not,
    // and a framebuffer torn down and rebuilt on alternate frames looks from the log
    // exactly like one that was never touched.
    const char* reason = nullptr;
};

State g_state;

// -- FLUSHING ROGLV2'S QUEUE BEFORE WE UNBIND ------------------------------------------
//
// roglv2 does not rasterise a polygon when polyDraw is called. It APPENDS to the ZBuffer
// drawer's bins, and the flush that issues the GL work runs at exactly three moments: a
// 2D mode switch, polySetDest, or -- as a last resort -- inside screenDisplay itself.
//
// In the race lobby nothing flushes it in time. gpl_simFrameRender makes no drawDebugText
// call after gpl_sceneRender unless ShowTimes is on, and polySetDest is reached only from
// gpl_cameraBind, which is the START of a pass. So the whole TV feed was still queued when
// the blit below unbound the framebuffer, and roglv2's own end-of-frame flush then drew it
// with framebuffer 0 bound and its own glViewport(0, 0, 640, 480) still set -- the
// bottom-left corner of a 2560x1440 window, at 1:1, y-flipped. Measured at 1021 of 1301
// draw calls per lobby frame arriving that late.
//
// rogl_batchOpen is a MODE flag over one shared drawer: any 2D entry point switches modes
// and flushes the polygons on its way in. An empty string is the cheapest such call and
// draws nothing.
void** g_drawDebugTextSlot = nullptr;
void** g_backBufferSlot = nullptr;

typedef void(__cdecl* DrawDebugTextFn)(void* surface, unsigned colour, int x, int y,
                                       const char* text, int background);
typedef void*(__cdecl* ScreenGetBackBufferFn)();


// Pointer comparison, not strcmp: every caller passes a literal, so identical reasons
// are the identical pointer and a changed reason is a different one.
void reportScaling(const char* reason) {
    if (g_state.reason == reason) return;
    g_state.reason = reason;
    logf("menu: scaling %s", reason);
}

// Does this driver understand the GL 3.0 split of the framebuffer binding into READ and
// DRAW? The scaled blit is the one operation that needs it -- read the front end, draw
// the window, both at once -- and a bind that fails with INVALID_ENUM leaves the
// previous binding in place and then blits the window to itself.
bool splitBindingWorks(const GlFunctions& gl, GLuint framebuffer) {
    if (!gl.bindFramebuffer || !gl.getIntegerv || !gl.getError) return false;

    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}

    GLint bound = 0;
    gl.bindFramebuffer(kGlReadFramebuffer, framebuffer);
    gl.getIntegerv(kGlReadFramebufferBinding, &bound);
    bool took = static_cast<GLuint>(bound) == framebuffer && gl.getError() == GL_NO_ERROR;
    gl.bindFramebuffer(kGlReadFramebuffer, 0);
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}
    return took;
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
    s.active = false;
    s.verifiedBinding = false;
}

bool createFramebuffer(int width, int height) {
    State& s = g_state;
    destroyFramebuffer();

    // GL_NEAREST for the integer scale, so 640x480 art stays as crisp as it was drawn;
    // GL_LINEAR for the fitted one, where the ratio is not a whole number and nearest
    // would give unevenly sized pixels.
    GLint filter = g_options.menuFilter == 2 ? GL_LINEAR : GL_NEAREST;

    // The binding found here goes back afterwards: roglv2 keeps its own copy of the bound
    // texture and does not rebind one it believes is bound (display +0x38).
    GLint previousTexture = 0;
    if (s.gl.getIntegerv) s.gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    s.gl.genTextures(1, &s.colour);
    s.gl.bindTexture(GL_TEXTURE_2D, s.colour);
    s.gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, nullptr);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    s.gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    s.gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

    // The front end draws no depth, but roglv2's ZBufferOpenGL clears and tests one
    // regardless, and a framebuffer without a depth attachment answers those calls with
    // an error rather than by ignoring them.
    s.gl.genRenderbuffers(1, &s.depth);
    s.gl.bindRenderbuffer(kGlRenderbuffer, s.depth);
    s.gl.renderbufferStorage(kGlRenderbuffer, kGlDepthComponent24, width, height);
    s.gl.bindRenderbuffer(kGlRenderbuffer, 0);

    s.gl.genFramebuffers(1, &s.framebuffer);
    s.gl.bindFramebuffer(kGlFramebuffer, s.framebuffer);
    s.gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                              s.colour, 0);
    s.gl.framebufferRenderbuffer(kGlFramebuffer, kGlDepthAttachment, kGlRenderbuffer,
                                 s.depth);

    GLenum status = s.gl.checkFramebufferStatus(kGlFramebuffer);
    s.gl.bindFramebuffer(kGlFramebuffer, 0);

    if (status != kGlFramebufferComplete) {
        logf("menu: the %dx%d framebuffer is incomplete (0x%04X) -- the front end stays "
             "unscaled",
             width, height, status);
        destroyFramebuffer();
        return false;
    }

    s.width = width;
    s.height = height;
    s.cleared = false;

    if (!s.checkedSplit) {
        s.checkedSplit = true;
        s.splitBinding = splitBindingWorks(s.gl, s.framebuffer);
        if (!s.splitBinding)
            logf("menu: this driver does not accept GL_READ_FRAMEBUFFER, so the front "
                 "end cannot be scaled into the window");
    }
    if (!s.splitBinding) {
        destroyFramebuffer();
        return false;
    }

    logf("menu: the front end renders into a %dx%d framebuffer, %s into the window",
         width, height,
         g_options.menuFilter == 0   ? "scaled by a whole number, nearest-neighbour"
         : g_options.menuFilter == 1 ? "aspect-fitted, nearest-neighbour"
                                     : "aspect-fitted, linear");
    return true;
}

// Where the front end lands inside the window. One function, so the blit and the
// pointer cannot disagree.
bool computeDestination(int windowWidth, int windowHeight) {
    State& s = g_state;
    if (s.width <= 0 || s.height <= 0 || windowWidth <= 0 || windowHeight <= 0)
        return false;

    int destWidth = 0;
    int destHeight = 0;
    if (g_options.menuFilter == 0) {
        // WHOLE NUMBERS ONLY. Every source pixel becomes an exact NxN block, which is the
        // only way the 640x480 art keeps the shape it was drawn with -- at the cost of a
        // wider border, because 1440/480 is 3 and 3 x 480 is 1440 but 2560/640 is 4 and
        // the smaller of the two factors wins.
        int factor = windowWidth / s.width;
        int vertical = windowHeight / s.height;
        if (vertical < factor) factor = vertical;
        if (factor < 1) factor = 1;
        destWidth = s.width * factor;
        destHeight = s.height * factor;
    } else {
        double byWidth = static_cast<double>(windowWidth) / s.width;
        double byHeight = static_cast<double>(windowHeight) / s.height;
        double scale = byWidth < byHeight ? byWidth : byHeight;
        destWidth = static_cast<int>(s.width * scale);
        destHeight = static_cast<int>(s.height * scale);
    }
    if (destWidth <= 0 || destHeight <= 0) return false;

    s.destWidth = destWidth;
    s.destHeight = destHeight;
    s.destX = (windowWidth - destWidth) / 2;
    s.destY = (windowHeight - destHeight) / 2;

    if (windowWidth != s.loggedWindowWidth || windowHeight != s.loggedWindowHeight) {
        s.loggedWindowWidth = windowWidth;
        s.loggedWindowHeight = windowHeight;
        logf("menu: %dx%d front end -> %dx%d at %d,%d in a %dx%d window", s.width,
             s.height, destWidth, destHeight, s.destX, s.destY, windowWidth,
             windowHeight);
    }
    return true;
}

using ScreenToClientFn = BOOL(WINAPI*)(HWND, LPPOINT);
ScreenToClientFn g_originalScreenToClient = nullptr;

BOOL WINAPI screenToClientHook(HWND window, LPPOINT point) {
    auto original = g_originalScreenToClient
                        ? g_originalScreenToClient
                        : reinterpret_cast<ScreenToClientFn>(GetProcAddress(
                              GetModuleHandleW(L"user32.dll"), "ScreenToClient"));
    if (!original) return FALSE;

    BOOL ok = original(window, point);
    if (ok && point && !menuScaleMapClientPoint(*point)) flatTargetMapClientPoint(*point);
    return ok;
}

}  // namespace

void menuScaleInstall(const Binding& binding) {
    g_drawDebugTextSlot = rasterGlobal(binding.image, "drawDebugText");
    g_backBufferSlot = binding.raster.screenGetBackBuffer;
    if (!g_drawDebugTextSlot || !g_backBufferSlot) {
        logf("menu: drawDebugText or screenGetBackBuffer could not be resolved, so the "
             "front end's polygons cannot be flushed before the blit -- the lobby's TV "
             "feed will land in the window's bottom-left corner");
    }

    void* original = nullptr;
    if (patchModuleImport(GetModuleHandleA(nullptr), "ScreenToClient",
                          reinterpret_cast<void*>(&screenToClientHook), &original)) {
        if (original)
            g_originalScreenToClient = reinterpret_cast<ScreenToClientFn>(original);
        logf("menu: gplc67's ScreenToClient is hooked -- its one call site is inside "
             "gpl_mouseReadClientDelta, so this reaches the pointer and nothing else");
    } else {
        logf("menu: gplc67's ScreenToClient could not be hooked -- the front end will "
             "scale but the pointer will not follow it");
    }
}

void rasterFlushPending() {
    if (!g_drawDebugTextSlot || !*g_drawDebugTextSlot) return;
    if (!g_backBufferSlot || !*g_backBufferSlot) return;

    void* surface =
        reinterpret_cast<ScreenGetBackBufferFn>(*g_backBufferSlot)();
    if (!surface) return;

    reinterpret_cast<DrawDebugTextFn>(*g_drawDebugTextSlot)(surface, 0, 0, 0, "", 0);
}

bool menuScaleActive() { return g_state.active && g_state.framebuffer != 0; }

bool menuScaleClientRect(RECT& out) {
    State& s = g_state;
    if (!menuScaleActive()) return false;
    if (s.destWidth <= 0 || s.destHeight <= 0) return false;

    out.left = s.destX;
    out.top = s.destY;
    out.right = s.destX + s.destWidth;
    out.bottom = s.destY + s.destHeight;
    return true;
}

void menuScaleReportBinding(const char* when) {
    State& s = g_state;
    if (!s.active || !s.framebuffer || !s.gl.getIntegerv) return;

    // Pointer comparison: every caller passes a literal, so a site is its address.
    int slot = 0;
    while (slot < kBindingSites && s.bindingSite[slot] && s.bindingSite[slot] != when)
        ++slot;
    if (slot == kBindingSites) return;
    s.bindingSite[slot] = when;

    GLint bound = -1;
    s.gl.getIntegerv(kGlFramebufferBinding, &bound);
    if (bound == s.bindingSeen[slot]) return;
    s.bindingSeen[slot] = bound;

    logf("menu: at %s the bound framebuffer is %d and the front end's is %u%s", when,
         bound, s.framebuffer,
         static_cast<GLuint>(bound) == s.framebuffer
             ? ""
             : " -- this draw is going STRAIGHT TO THE WINDOW, where 640x480 of content "
               "lands in the bottom-left corner because GL counts from there");
}

void menuScaleSuspend() {
    State& s = g_state;
    if (!s.resolved) return;
    if (s.bound && s.gl.bindFramebuffer) s.gl.bindFramebuffer(kGlFramebuffer, 0);
    destroyFramebuffer();
}

void menuScaleBeginFrame() {
    State& s = g_state;

    if (s.gaveUp) {
        reportScaling("off -- a framebuffer failure earlier in the session");
        if (s.framebuffer) menuScaleSuspend();
        return;
    }
    if (!windowInFrontEnd()) {
        reportScaling("off -- the engine is in a world mode");
        if (s.framebuffer) menuScaleSuspend();
        return;
    }
    // The off-screen render target owns the sim mode and this owns the front end; they
    // are mutually exclusive by construction, and if that ever stops being true the
    // second bind would silently win.
    if (renderTargetActive()) {
        reportScaling("off -- the off-screen sim target owns this frame");
        if (s.framebuffer) menuScaleSuspend();
        return;
    }

    if (!s.resolved) {
        if (!glResolve(s.gl) || !glResolveFramebuffer(s.gl)) {
            s.gaveUp = true;
            logf("menu: framebuffer objects are unavailable, so the front end stays at "
                 "640x480 in the corner of the window");
            return;
        }
        s.resolved = true;
    }

    int modeWidth = 0;
    int modeHeight = 0;
    windowEngineModeSize(modeWidth, modeHeight);
    if (modeWidth <= 0 || modeHeight <= 0) return;

    int windowWidth = 0;
    int windowHeight = 0;
    if (!glDrawableSize(s.gl, windowWidth, windowHeight)) return;

    // Nothing to scale, and scaling anyway would cost a full-frame blit to produce the
    // same pixels. This is the ordinary case at a 640x480 window, where the game has
    // already put the desktop into 640x480.
    if (windowWidth == modeWidth && windowHeight == modeHeight) {
        reportScaling("off -- the window already matches the front end pixel for pixel");
        if (s.framebuffer) menuScaleSuspend();
        return;
    }

    if (s.width != modeWidth || s.height != modeHeight) {
        if (!createFramebuffer(modeWidth, modeHeight)) {
            s.gaveUp = true;
            return;
        }
    }

    s.gl.bindFramebuffer(kGlFramebuffer, s.framebuffer);

    // Read the binding back the first time rather than trusting the call: a rejected
    // bind leaves the previous one in place and reports nothing to anyone who does not
    // ask, and the whole front end would then go to the window while the blit read an
    // empty target -- black, with a GL error surfacing several calls later on something
    // innocent.
    if (!s.verifiedBinding) {
        GLint bound = -1;
        if (s.gl.getIntegerv) s.gl.getIntegerv(kGlFramebufferBinding, &bound);
        if (s.gl.getIntegerv && static_cast<GLuint>(bound) != s.framebuffer) {
            s.gaveUp = true;
            logf("menu: bound framebuffer %u but the driver reports %d -- the front end "
                 "stays unscaled",
                 s.framebuffer, bound);
            s.gl.bindFramebuffer(kGlFramebuffer, 0);
            destroyFramebuffer();
            return;
        }
        s.verifiedBinding = true;
    }

    // A NEW FRAMEBUFFER STARTS BLACK.
    //
    // It used to start as a copy of the window's bottom-left mode-sized corner, on the
    // theory that the front end does not redraw its whole surface and would otherwise
    // show widgets floating on nothing. The theory may even be true, but the seed was
    // demonstrably worse than the problem: the corner it copies is a piece of the
    // PREVIOUS scaled frame, not the previous front end, so entering the race lobby
    // pasted a shrunken ghost of the last menu into the new framebuffer and left it
    // there -- which is what the doubled television in the lobby turned out to be.
    //
    // Copying an undefined buffer to guard against a redraw nobody has confirmed is two
    // guesses stacked on each other. A clear is one fact.
    if (!s.cleared) {
        s.cleared = true;
        s.gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        s.gl.clear(GL_COLOR_BUFFER_BIT);
    }

    // roglv2 sized its viewport and its ortho from the mode when it built the display,
    // and the framebuffer is that same size -- so unlike the off-screen sim target,
    // nothing here has to be re-asserted and the matrix state is left exactly as the
    // rasterizer set it up.
    reportScaling("on");
    s.bound = true;
    s.active = true;
}

void menuScaleEndFrameAndBlit() {
    State& s = g_state;
    if (!s.bound) return;

    // FIRST, while the framebuffer is still ours. Everything below unbinds it, and
    // anything roglv2 still has queued at that point is drawn into the window instead.
    rasterFlushPending();

    s.bound = false;

    int windowWidth = 0;
    int windowHeight = 0;
    if (!glDrawableSize(s.gl, windowWidth, windowHeight) ||
        !computeDestination(windowWidth, windowHeight)) {
        s.gl.bindFramebuffer(kGlFramebuffer, 0);
        s.active = false;
        return;
    }

    GLboolean scissor = s.gl.isEnabled ? s.gl.isEnabled(GL_SCISSOR_TEST) : GL_FALSE;
    if (scissor && s.gl.disable) s.gl.disable(GL_SCISSOR_TEST);

    s.gl.bindFramebuffer(kGlDrawFramebuffer, 0);
    s.gl.bindFramebuffer(kGlReadFramebuffer, s.framebuffer);
    s.gl.readBuffer(kGlColorAttachment0);

    if (s.destWidth < windowWidth || s.destHeight < windowHeight) {
        s.gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        s.gl.clear(GL_COLOR_BUFFER_BIT);
    }

    // Both sides are GL framebuffers with a bottom-left origin, so the destination's y
    // is measured from the bottom while everything else here counts from the top.
    int glDestY = windowHeight - s.destY - s.destHeight;
    s.gl.blitFramebuffer(0, 0, s.width, s.height, s.destX, glDestY,
                         s.destX + s.destWidth, glDestY + s.destHeight,
                         GL_COLOR_BUFFER_BIT,
                         g_options.menuFilter == 2 ? GL_LINEAR : GL_NEAREST);

    // Unbind BEFORE naming GL_BACK: GL_BACK is not a valid read buffer for a framebuffer
    // object, so the other order raises INVALID_OPERATION.
    s.gl.bindFramebuffer(kGlReadFramebuffer, 0);
    s.gl.readBuffer(GL_BACK);
    if (scissor && s.gl.enable) s.gl.enable(GL_SCISSOR_TEST);
}

bool menuScaleMapClientPoint(POINT& point) {
    State& s = g_state;
    if (!menuScaleActive()) return false;
    if (s.destWidth <= 0 || s.destHeight <= 0) return false;

    long x = (point.x - s.destX) * s.width / s.destWidth;
    long y = (point.y - s.destY) * s.height / s.destHeight;

    // A POINT OUTSIDE THE MENU IS HELD OUTSIDE IT.
    //
    // This used to clamp to the nearest edge, on the reading that the game clamps the
    // same quantity a step later anyway. It does -- and that is the defect. Clamping
    // moves a point that is over a black bar onto the menu's edge at the SAME ROW, so a
    // click in the bar beside a corner button lands on the button. Pinning the axis that
    // is out of range to -1 keeps the row but takes the point off the widget, and the
    // hit test fails the way it does for any other point that is not on a widget.
    //
    // The cursor clip in Window.cpp is what normally stops the pointer getting here at
    // all; this is the backstop for the moments the clip is not in force -- an unfocused
    // window, a mode change that released it, or the VR pointer, which is drawn rather
    // than owned by Windows.
    if (x < 0 || x > s.width - 1) x = -1;
    if (y < 0 || y > s.height - 1) y = -1;

    point.x = x;
    point.y = y;
    return true;
}

}  // namespace gplvr
