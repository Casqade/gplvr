#include "MirrorCache.h"

#include "GlInterop.h"
#include "Log.h"
#include "RenderTarget.h"

namespace gplvr {
namespace {

struct State {
    GlFunctions gl;
    bool        resolved = false;
    bool        gaveUp = false;
    HGLRC       context = nullptr;

    // What the copy-out was last seen writing into, and what the rasterizer says its size is.
    int bitmapHandle = 0;
    int width = 0;
    int height = 0;

    // GPL's texture as the last check found it, and the two copies of it.
    GLuint mirror = 0;
    GLuint eye[2] = {0, 0};
    bool   held[2] = {false, false};

    // read, then draw. Two, because the blit needs both bound at once.
    GLuint from = 0;
    GLuint to = 0;

    bool        engaged = false;
    const char* refusal = nullptr;
    unsigned    restores = 0;
};

State g_state;

// Pointer comparison: every caller passes a literal.
void refuse(const char* why) {
    if (g_state.refusal == why) return;
    g_state.refusal = why;
    g_state.engaged = false;
    g_state.held[0] = g_state.held[1] = false;
    logf("mirror: no per-eye copy of mirror.mip -- %s. mirrorUpdateInterval does nothing while "
         "per-eye mirrors on, because a skipped frame would leave the left eye looking at the "
         "right eye's reflection.",
         why);
}

// The separate READ and DRAW bindings, asked by binding and reading back. A bind that fails
// with INVALID_ENUM leaves the previous binding in place, so the blit that followed would copy
// a framebuffer onto itself and nothing would return an error.
bool splitBindingWorks(const GlFunctions& gl, GLuint framebuffer) {
    if (!gl.bindFramebuffer || !gl.getIntegerv || !gl.getError) return false;

    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}

    GLint bound = 0;
    gl.bindFramebuffer(kGlReadFramebuffer, framebuffer);
    gl.getIntegerv(kGlReadFramebufferBinding, &bound);
    bool took =
        static_cast<GLuint>(bound) == framebuffer && gl.getError() == GL_NO_ERROR;
    gl.bindFramebuffer(kGlReadFramebuffer, 0);
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}
    return took;
}

// Is `name` really the texture behind the handle? GL is asked how big it is, and the answer has
// to match what the rasterizer says the bitmap is. This is the whole verification of the walk
// `rasterBitmapGlTexture` does: a handle whose root is not a textured bitmap yields a name GL
// does not know, whose level 0 is 0x0, or somebody else's texture of a different size.
//
// `internalFormat` comes back so the copies can be made in the mirror's own format rather than
// a guess -- the mip is created empty by roglv2 with a format chosen from the display depth
// (report 37 section 2.1), and a blit between mismatched formats converts every pixel.
bool measureTexture(State& s, GLuint name, GLint& internalFormat) {
    const GlFunctions& gl = s.gl;
    GLint              width = 0;
    GLint              height = 0;
    GLint              previous = 0;

    internalFormat = 0;
    gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    gl.bindTexture(GL_TEXTURE_2D, name);
    gl.getTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    gl.getTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    gl.getTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));

    if (width == s.width && height == s.height && internalFormat) return true;
    logf("mirror: bitmap handle %d resolves to GL texture %u, which reads back as %dx%d format "
         "0x%04X -- but the rasterizer says that handle is %dx%d, so the walk to the name found "
         "something else",
         s.bitmapHandle, name, width, height, static_cast<unsigned>(internalFormat), s.width,
         s.height);
    return false;
}

bool buildCopies(State& s, GLint internalFormat) {
    const GlFunctions& gl = s.gl;
    GLint              previous = 0;

    gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    for (int eye = 0; eye < 2; ++eye) {
        if (!s.eye[eye]) gl.genTextures(1, &s.eye[eye]);
        if (!s.eye[eye]) {
            gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
            return false;
        }
        gl.bindTexture(GL_TEXTURE_2D, s.eye[eye]);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.texImage2D(GL_TEXTURE_2D, 0, internalFormat, s.width, s.height, 0, GL_RGB,
                      GL_UNSIGNED_BYTE, nullptr);
    }
    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    s.held[0] = s.held[1] = false;
    return true;
}

// Everything that has to be true before a copy can run, checked in the order that makes each
// failure name itself. Cheap on the frames where nothing has changed: a name read and a
// comparison.
bool ensure(State& s) {
    if (s.gaveUp || !s.bitmapHandle) return false;

    if (!s.resolved) {
        if (!glResolve(s.gl) || !glResolveFramebuffer(s.gl) || !s.gl.blitFramebuffer ||
            !s.gl.getTexLevelParameteriv || !s.gl.getIntegerv || !s.gl.readBuffer) {
            s.gaveUp = true;
            refuse("the framebuffer or texture entry points it needs did not resolve");
            return false;
        }
        s.resolved = true;
    }

    // The names belong to the context that made them. A mode change gives GPL a new one.
    HGLRC context = s.gl.wglGetCurrentContext();
    if (!context) return false;
    if (context != s.context) {
        s.context = context;
        s.mirror = 0;
        s.eye[0] = s.eye[1] = 0;
        s.from = 0;
        s.to = 0;
        s.held[0] = s.held[1] = false;
    }

    if (!s.from) s.gl.genFramebuffers(1, &s.from);
    if (!s.to) s.gl.genFramebuffers(1, &s.to);
    if (!s.from || !s.to) {
        s.gaveUp = true;
        refuse("a framebuffer object could not be created");
        return false;
    }

    GLuint name = rasterBitmapGlTexture(s.bitmapHandle);
    if (!name) {
        refuse("mirror.mip's handle does not reach a GL texture through the handle table");
        return false;
    }
    if (name == s.mirror && s.eye[0] && s.eye[1]) return true;

    GLint internalFormat = 0;
    if (!measureTexture(s, name, internalFormat)) {
        s.gaveUp = true;
        refuse("the GL texture behind mirror.mip could not be identified");
        return false;
    }
    if (!splitBindingWorks(s.gl, s.from)) {
        s.gaveUp = true;
        refuse("this driver has no separate read and draw framebuffer bindings, so a blit "
               "between two of them would copy one onto itself");
        return false;
    }
    if (!buildCopies(s, internalFormat)) {
        s.gaveUp = true;
        refuse("the two copies of mirror.mip could not be created");
        return false;
    }

    s.mirror = name;
    s.refusal = nullptr;
    s.engaged = true;
    logf("mirror: each eye keeps its own copy of mirror.mip -- handle %d is %dx%d, format "
         "0x%04X, GL texture %u, copied to %u and %u. mirrorUpdateInterval now works with "
         "per-eye mirrors on.",
         s.bitmapHandle, s.width, s.height, static_cast<unsigned>(internalFormat), name,
         s.eye[0], s.eye[1]);
    return true;
}

// One whole-texture blit. On this path neither texture is bound, only attached, so roglv2's own
// idea of which texture is bound is left alone -- the reason MirrorTarget.cpp gives for using a
// renderbuffer applies here too. Only the setup above binds, and it puts the binding back.
bool copyTexture(State& s, GLuint fromName, GLuint toName) {
    const GlFunctions& gl = s.gl;

    GLint previousDraw = 0;
    GLint previousRead = 0;
    gl.getIntegerv(kGlFramebufferBinding, &previousDraw);
    gl.getIntegerv(kGlReadFramebufferBinding, &previousRead);

    gl.bindFramebuffer(kGlReadFramebuffer, s.from);
    gl.framebufferTexture2D(kGlReadFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, fromName,
                            0);
    gl.readBuffer(kGlColorAttachment0);
    gl.bindFramebuffer(kGlDrawFramebuffer, s.to);
    gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, toName, 0);

    GLenum readStatus = gl.checkFramebufferStatus(kGlReadFramebuffer);
    GLenum drawStatus = gl.checkFramebufferStatus(kGlDrawFramebuffer);
    bool   complete = readStatus == kGlFramebufferComplete &&
                    drawStatus == kGlFramebufferComplete;

    if (complete) {
        // The blit is clipped by the scissor box and masked by the colour mask, and roglv2
        // leaves both set for the eye it is drawing. Pushed and popped with the framebuffers
        // still bound, so what goes back is what the pass was using; without the attribute
        // stack the scissor is put back by hand, because leaving it off would let the pass's
        // next draw paint outside its own half of the frame.
        bool      saved = gl.pushAttrib && gl.popAttrib;
        GLboolean scissorWas = GL_FALSE;
        if (saved)
            gl.pushAttrib(GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT);
        else if (gl.isEnabled)
            scissorWas = gl.isEnabled(GL_SCISSOR_TEST);

        if (gl.disable) gl.disable(GL_SCISSOR_TEST);
        // Only under the attribute stack: a mask opened by hand and not closed would let the
        // rest of the pass write channels it had masked off, which is worse than a blit that
        // arrives masked.
        if (saved && gl.colorMask) gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.blitFramebuffer(0, 0, s.width, s.height, 0, 0, s.width, s.height,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);

        if (saved)
            gl.popAttrib();
        else if (scissorWas && gl.enable)
            gl.enable(GL_SCISSOR_TEST);
    }

    gl.framebufferTexture2D(kGlReadFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, 0, 0);
    gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, 0, 0);
    gl.bindFramebuffer(kGlReadFramebuffer, static_cast<GLuint>(previousRead));
    gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(previousDraw));

    if (!complete) {
        s.gaveUp = true;
        logf("mirror: read framebuffer 0x%04X, draw framebuffer 0x%04X with a %dx%d mirror "
             "texture attached", readStatus, drawStatus, s.width, s.height);
        refuse("this driver will not attach mirror.mip's format to a framebuffer");
    }
    return complete;
}

}  // namespace

void mirrorCacheNoteTexture(int bitmapHandle, int width, int height) {
    State& s = g_state;
    if (bitmapHandle <= 0 || width < 1 || height < 1) return;
    if (s.bitmapHandle == bitmapHandle && s.width == width && s.height == height) return;

    s.bitmapHandle = bitmapHandle;
    s.width = width;
    s.height = height;
    s.mirror = 0;
    s.held[0] = s.held[1] = false;
}

bool mirrorCacheReady() {
    const State& s = g_state;
    return s.engaged && s.held[0] && s.held[1];
}

void mirrorCacheSave(int eye) {
    State& s = g_state;
    if (eye < 0 || eye > 1) return;
    if (!ensure(s)) return;
    s.held[eye] = copyTexture(s, s.mirror, s.eye[eye]);
}

bool mirrorCacheRestore(int eye) {
    State& s = g_state;
    if (eye < 0 || eye > 1 || !s.held[eye]) return false;
    if (!ensure(s)) return false;
    if (!copyTexture(s, s.eye[eye], s.mirror)) return false;
    ++s.restores;
    return true;
}

bool mirrorCacheBlackout() {
    State& s = g_state;
    if (!ensure(s)) return false;

    const GlFunctions& gl = s.gl;
    GLint              previousDraw = 0;
    gl.getIntegerv(kGlFramebufferBinding, &previousDraw);

    gl.bindFramebuffer(kGlDrawFramebuffer, s.to);
    gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, s.mirror,
                            0);
    bool complete = gl.checkFramebufferStatus(kGlDrawFramebuffer) == kGlFramebufferComplete;

    if (complete) {
        // Same care the blit takes: roglv2 leaves a scissor and a colour mask set for the eye
        // it is drawing, and a clear obeys both. Without lifting them this would black out a
        // rectangle of the texture rather than the texture.
        bool      saved = gl.pushAttrib && gl.popAttrib;
        GLboolean scissorWas = GL_FALSE;
        if (saved)
            gl.pushAttrib(GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT);
        else if (gl.isEnabled)
            scissorWas = gl.isEnabled(GL_SCISSOR_TEST);
        if (gl.disable) gl.disable(GL_SCISSOR_TEST);
        if (saved && gl.colorMask) gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl.clear(GL_COLOR_BUFFER_BIT);

        if (saved)
            gl.popAttrib();
        else if (scissorWas && gl.enable)
            gl.enable(GL_SCISSOR_TEST);
    }

    gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, 0, 0);
    gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(previousDraw));

    // Both saved copies are now wrong: they hold the reflection from before the mirror was
    // turned off, and a later restore would put it back on the glass.
    s.held[0] = s.held[1] = false;
    return complete;
}

unsigned mirrorCacheTakeRestores() {
    unsigned count = g_state.restores;
    g_state.restores = 0;
    return count;
}

namespace {

// The binding around GPL's copy-out, with GL of its own: the cache's needs the framebuffer entry
// points, and this must work wherever the copy does.
struct CopyBinding {
    GlFunctions gl;
    bool        ready = false;
    bool        failed = false;
    bool        saved = false;
    GLint       name = 0;
    unsigned    copies = 0;
    unsigned    rebound = 0;
};

CopyBinding g_copy;

}  // namespace

void mirrorCacheCopyBegin() {
    CopyBinding& c = g_copy;
    c.saved = false;
    if (!c.ready) {
        if (c.failed) return;
        if (!glResolve(c.gl) || !c.gl.getIntegerv || !c.gl.bindTexture) {
            c.failed = true;
            logf("mirror: glGetIntegerv or glBindTexture did not resolve; the copy-out's texture "
                 "binding is left as GPL leaves it (the copy-out rebind does nothing)");
            return;
        }
        c.ready = true;
    }
    c.gl.getIntegerv(GL_TEXTURE_BINDING_2D, &c.name);
    c.saved = true;
}

void mirrorCacheCopyEnd() {
    CopyBinding& c = g_copy;
    if (!c.saved) return;
    c.saved = false;
    ++c.copies;
    GLint now = 0;
    c.gl.getIntegerv(GL_TEXTURE_BINDING_2D, &now);
    if (now == c.name) return;
    ++c.rebound;
    c.gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(c.name));
}

void mirrorCacheTakeCopies(unsigned& copies, unsigned& rebound) {
    copies = g_copy.copies;
    rebound = g_copy.rebound;
    g_copy.copies = 0;
    g_copy.rebound = 0;
}

void mirrorCacheRelease() {
    State& s = g_state;
    s.bitmapHandle = 0;
    s.width = 0;
    s.height = 0;
    s.mirror = 0;
    s.held[0] = s.held[1] = false;
    s.engaged = false;
}

}  // namespace gplvr
