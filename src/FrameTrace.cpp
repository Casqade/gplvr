#include "FrameTrace.h"

#include "GlInterop.h"
#include "Log.h"

#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

constexpr GLenum kViewport = 0x0BA2;
constexpr GLenum kScissorBox = 0x0C10;
constexpr GLenum kScissorTest = 0x0C11;
constexpr GLenum kDrawFramebufferBinding = 0x8CA6;
constexpr int    kWindows = 16;

struct EyePass {
    bool  ran;
    void* surface;
    int   size[2];
    int   rect[4];
    GLint viewport[4];
    GLint scissor[4];
    bool  scissorOn;
    GLint framebuffer;
};

struct State {
    int   left = 0;
    int   windows = kWindows;
    int   frame = 0;
    char  why[128] = "";
    EyePass eye[2] = {};
    bool  staticDrawn = false;
    void* staticCamera = nullptr;
    void* staticSurface = nullptr;
    int   staticSize[2] = {};
    const char* staticWhat = "";
    bool  presenterChecked = false;
    bool  presenterHeld = false;
    int   rendered[2] = {};
    int   wanted[2] = {};
};

State       g;
GlFunctions g_gl;
bool        g_glTried = false;
bool        g_glOk = false;

void clearFrame() {
    std::memset(g.eye, 0, sizeof(g.eye));
    g.staticDrawn = false;
    g.staticCamera = nullptr;
    g.staticSurface = nullptr;
    g.staticSize[0] = g.staticSize[1] = 0;
    g.staticWhat = "";
    g.presenterChecked = false;
    g.presenterHeld = false;
}

int formatEye(char* out, size_t size, int index, const EyePass& e) {
    if (!e.ran) return sprintf_s(out, size, " | eye %d did not render", index);
    return sprintf_s(out, size,
                     " | eye %d surface %p %dx%d for rect %d,%d %dx%d, viewport %d,%d %dx%d, "
                     "scissor %s %d,%d %dx%d, draw framebuffer %d",
                     index, e.surface, e.size[0], e.size[1], e.rect[0], e.rect[1], e.rect[2],
                     e.rect[3], e.viewport[0], e.viewport[1], e.viewport[2], e.viewport[3],
                     e.scissorOn ? "ON" : "off", e.scissor[0], e.scissor[1], e.scissor[2],
                     e.scissor[3], e.framebuffer);
}

}  // namespace

void frameTraceArm(int frames, const char* why) {
    if (g.left > 0) {
        if (frames > g.left) g.left = frames;
        return;
    }
    if (g.windows <= 0) return;
    --g.windows;
    g.left = frames;
    g.frame = 0;
    sprintf_s(g.why, "%s", why);
    clearFrame();
    logf("frame trace: the next %d frames are recorded -- %s%s", frames, why,
         g.windows ? "" : " (the last window this session)");
}

bool frameTraceArmed() { return g.left > 0; }

void frameTraceEye(int eye, void* surface, int surfaceWidth, int surfaceHeight, const int rect[4]) {
    if (g.left <= 0 || eye < 0 || eye > 1) return;
    if (!g_glTried) {
        g_glTried = true;
        g_glOk = glResolve(g_gl) && g_gl.getIntegerv && g_gl.isEnabled;
    }
    EyePass& e = g.eye[eye];
    e.ran = true;
    e.surface = surface;
    e.size[0] = surfaceWidth;
    e.size[1] = surfaceHeight;
    for (int i = 0; i < 4; ++i) e.rect[i] = rect ? rect[i] : 0;
    if (g_glOk) {
        g_gl.getIntegerv(kViewport, e.viewport);
        g_gl.getIntegerv(kScissorBox, e.scissor);
        e.scissorOn = g_gl.isEnabled(kScissorTest) != 0;
        g_gl.getIntegerv(kDrawFramebufferBinding, &e.framebuffer);
    }
}

void frameTraceStatic(void* camera, void* surface, int width, int height, const char* what) {
    if (g.left <= 0) return;
    g.staticDrawn = true;
    g.staticCamera = camera;
    g.staticSurface = surface;
    g.staticSize[0] = width;
    g.staticSize[1] = height;
    g.staticWhat = what ? what : "";
}

void frameTracePresenter(int width, int height, int wantWidth, int wantHeight, bool held) {
    if (g.left <= 0) return;
    g.presenterChecked = true;
    g.presenterHeld = held;
    g.rendered[0] = width;
    g.rendered[1] = height;
    g.wanted[0] = wantWidth;
    g.wanted[1] = wantHeight;
}

void frameTracePublish(const char* outcome, bool sideBySide, bool stretch) {
    if (g.left <= 0) return;
    char presenter[160] = "the presenter's size check did not run";
    if (g.presenterChecked)
        sprintf_s(presenter, "rendered %dx%d against %dx%d wanted%s", g.rendered[0], g.rendered[1],
                  g.wanted[0], g.wanted[1],
                  g.presenterHeld ? " -- HELD BY THE PRESENTER, the headset kept its last frame"
                                  : "");
    char staticText[192] = "no static";
    if (g.staticDrawn)
        sprintf_s(staticText, "STATIC drawn through camera %p on surface %p %dx%d (%s)",
                  g.staticCamera, g.staticSurface, g.staticSize[0], g.staticSize[1],
                  g.staticWhat);
    logf("frame trace: #%d %s, %s, stretch %s; %s; %s", g.frame, outcome,
         sideBySide ? "side by side" : "one image", stretch ? "on" : "OFF", presenter, staticText);

    char eyes[640];
    int  used = 0;
    for (int eye = 0; eye < 2 && used < static_cast<int>(sizeof(eyes)) - 1; ++eye)
        used += formatEye(eyes + used, sizeof(eyes) - used, eye, g.eye[eye]);
    logf("frame trace: #%d%s%s", g.frame, eyes, g_glOk ? "" : " (GL state not readable)");

    clearFrame();
    ++g.frame;
    if (--g.left == 0) logf("frame trace: window over (%s)", g.why);
}

}  // namespace gplvr
