#include "MirrorTarget.h"

#include "GlInterop.h"
#include "Hooks.h"
#include "Log.h"
#include "Stereo.h"

#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

constexpr GLenum kGlRgba8 = 0x8058;
constexpr GLenum kGlViewport = 0x0BA2;

struct State {
    GlFunctions gl;
    bool        resolved = false;
    bool        gaveUp = false;
    HGLRC       context = nullptr;

    GLuint framebuffer = 0;
    GLuint colour = 0;
    GLuint depth = 0;
    int    width = 0;
    int    height = 0;

    bool   active = false;
    GLuint restoreTo = 0;

    // Why the mirror is or is not in its own framebuffer, reported when the answer changes.
    const char* reason = nullptr;
};

State g_state;

// Pointer comparison: every caller passes a literal.
void report(const char* reason) {
    if (g_state.reason == reason) return;
    g_state.reason = reason;
    logf("mirror: %s", reason);
}

// `contextAlive` false when the context the names belong to is gone, so they are dropped
// rather than deleted in whatever context is current now.
void destroy(bool contextAlive) {
    State& s = g_state;
    if (contextAlive) {
        if (s.framebuffer && s.gl.deleteFramebuffers) s.gl.deleteFramebuffers(1, &s.framebuffer);
        if (s.colour && s.gl.deleteRenderbuffers) s.gl.deleteRenderbuffers(1, &s.colour);
        if (s.depth && s.gl.deleteRenderbuffers) s.gl.deleteRenderbuffers(1, &s.depth);
    }
    s.framebuffer = 0;
    s.colour = 0;
    s.depth = 0;
    s.width = 0;
    s.height = 0;
}

// Colour is a renderbuffer rather than a texture: creating a texture would move the
// GL_TEXTURE_2D binding under roglv2, which binds textures on its own schedule.
bool create(int width, int height) {
    State&       s = g_state;
    GlFunctions& gl = s.gl;
    destroy(true);

    GLint previous = 0;
    gl.getIntegerv(kGlFramebufferBinding, &previous);

    gl.genRenderbuffers(1, &s.colour);
    gl.bindRenderbuffer(kGlRenderbuffer, s.colour);
    gl.renderbufferStorage(kGlRenderbuffer, kGlRgba8, width, height);
    gl.genRenderbuffers(1, &s.depth);
    gl.bindRenderbuffer(kGlRenderbuffer, s.depth);
    gl.renderbufferStorage(kGlRenderbuffer, kGlDepthComponent24, width, height);
    gl.bindRenderbuffer(kGlRenderbuffer, 0);

    gl.genFramebuffers(1, &s.framebuffer);
    gl.bindFramebuffer(kGlFramebuffer, s.framebuffer);
    gl.framebufferRenderbuffer(kGlFramebuffer, kGlColorAttachment0, kGlRenderbuffer, s.colour);
    gl.framebufferRenderbuffer(kGlFramebuffer, kGlDepthAttachment, kGlRenderbuffer, s.depth);
    GLenum status = gl.checkFramebufferStatus(kGlFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previous));

    if (status != kGlFramebufferComplete) {
        logf("mirror: the %dx%d mirror framebuffer is incomplete (0x%04X)", width, height,
             status);
        destroy(true);
        return false;
    }
    s.width = width;
    s.height = height;
    logf("mirror: built a %dx%d framebuffer for the mirror pass (%.1f MB)", width, height,
         width * height * 7.0 / (1024.0 * 1024.0));
    return true;
}

void restore() {
    State& s = g_state;
    s.gl.bindFramebuffer(kGlFramebuffer, s.restoreTo);
    s.active = false;
}

}  // namespace

void mirrorTargetAfterBind(void* camera, const SceneParams* params) {
    State& s = g_state;
    bool isMirror = camera && params && camera == params->mirrorCamera;

    if (s.active) {
        if (!isMirror) restore();
        return;
    }
    if (!isMirror || !g_options.vrMirrorOffscreen || s.gaveUp) return;

    // Only the copy-out path renders into the frame. The direct path renders into the mip.
    if (!(params->mirrorFlags & 1u)) {
        report("GPL renders it straight into mirror.mip, so there is no corner to move");
        return;
    }

    if (!s.resolved) {
        if (!glResolve(s.gl) || !glResolveFramebuffer(s.gl) || !s.gl.getIntegerv ||
            !s.gl.depthMask || !s.gl.pushAttrib || !s.gl.popAttrib) {
            s.gaveUp = true;
            logf("mirror: framebuffer objects are unavailable -- the mirror stays in the "
                 "frame's top-left corner");
            return;
        }
        s.resolved = true;
    }

    HGLRC context = s.gl.wglGetCurrentContext();
    if (!context) return;
    if (context != s.context) {
        destroy(false);
        s.context = context;
    }

    // The mirror's size, from the camera: gpl_cameraCreate stores half its surface's extents.
    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
    std::memcpy(&halfWidth, static_cast<const char*>(camera) + kCameraHalfWExtent,
                sizeof(halfWidth));
    std::memcpy(&halfHeight, static_cast<const char*>(camera) + kCameraHalfHExtent,
                sizeof(halfHeight));
    int mirrorWidth = static_cast<int>(std::ceil(halfWidth * 2.0f)) + 2;
    int mirrorHeight = static_cast<int>(std::ceil(halfHeight * 2.0f)) + 2;

    // The display's viewport. Its height is what the ortho and the copy count rows from.
    GLint viewport[4] = {0, 0, 0, 0};
    s.gl.getIntegerv(kGlViewport, viewport);
    if (viewport[0] != 0 || viewport[1] != 0 || halfWidth < 1.0f || halfHeight < 1.0f ||
        mirrorHeight > viewport[3]) {
        report("the viewport is not the whole display, so the mirror stays in the frame's "
               "corner");
        return;
    }
    int width = mirrorWidth < viewport[2] ? mirrorWidth : viewport[2];
    int height = viewport[3];

    if (!s.framebuffer || width != s.width || height != s.height) {
        if (!create(width, height)) {
            s.gaveUp = true;
            logf("mirror: the mirror stays in the frame's top-left corner");
            return;
        }
    }

    GLint previous = 0;
    s.gl.getIntegerv(kGlFramebufferBinding, &previous);
    s.restoreTo = static_cast<GLuint>(previous);
    s.gl.bindFramebuffer(kGlFramebuffer, s.framebuffer);

    // In the frame the mirror pass drew over depth the frame's own clear left behind. Here
    // that clear never happens, so do it, whatever depth mask roglv2 has set.
    s.gl.pushAttrib(GL_DEPTH_BUFFER_BIT);
    s.gl.depthMask(GL_TRUE);
    s.gl.clear(GL_DEPTH_BUFFER_BIT);
    s.gl.popAttrib();

    s.active = true;
    report("rendered into its own framebuffer, not the frame's corner");
}

void mirrorTargetEndScene() {
    if (g_state.active) restore();
}

}  // namespace gplvr
