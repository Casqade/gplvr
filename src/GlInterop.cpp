#include "GlInterop.h"

#include "FrameBudget.h"
#include "Log.h"

#include <d3d11.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gplvr {
namespace {

template <typename T>
void resolveExport(HMODULE module, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(module, name));
}

template <typename T>
void resolveExtension(const GlFunctions& gl, const char* name, T& out) {
    out = reinterpret_cast<T>(gl.wglGetProcAddress(name));
}

typedef const char*(WINAPI* WglGetExtensionsStringArbFn)(HDC);
typedef const char*(WINAPI* WglGetExtensionsStringExtFn)();

}  // namespace

bool glResolve(GlFunctions& gl) {
    // GetModuleHandle, not LoadLibrary: if opengl32 is not already in the process then
    // the D3D7 rasterizer is running and there is nothing here to talk to. Loading it
    // ourselves would only manufacture a context-less failure.
    gl.module = GetModuleHandleA("opengl32.dll");
    if (!gl.module) return false;

    resolveExport(gl.module, "wglGetCurrentContext", gl.wglGetCurrentContext);
    resolveExport(gl.module, "wglGetCurrentDC", gl.wglGetCurrentDC);
    resolveExport(gl.module, "wglGetProcAddress", gl.wglGetProcAddress);
    resolveExport(gl.module, "glGetString", gl.getString);
    resolveExport(gl.module, "glGetError", gl.getError);
    resolveExport(gl.module, "glFinish", gl.finish);
    resolveExport(gl.module, "glReadBuffer", gl.readBuffer);
    resolveExport(gl.module, "glReadPixels", gl.readPixels);
    resolveExport(gl.module, "glGenTextures", gl.genTextures);
    resolveExport(gl.module, "glDeleteTextures", gl.deleteTextures);
    resolveExport(gl.module, "glBindTexture", gl.bindTexture);
    resolveExport(gl.module, "glCopyTexSubImage2D", gl.copyTexSubImage2D);
    resolveExport(gl.module, "glGetTexLevelParameteriv", gl.getTexLevelParameteriv);
    resolveExport(gl.module, "glPixelStorei", gl.pixelStorei);
    resolveExport(gl.module, "glTexImage2D", gl.texImage2D);
    resolveExport(gl.module, "glTexParameteri", gl.texParameteri);
    resolveExport(gl.module, "glViewport", gl.viewport);
    resolveExport(gl.module, "glMatrixMode", gl.matrixMode);
    resolveExport(gl.module, "glLoadIdentity", gl.loadIdentity);
    resolveExport(gl.module, "glOrtho", gl.ortho);
    resolveExport(gl.module, "glTranslatef", gl.translatef);
    resolveExport(gl.module, "glClear", gl.clear);
    resolveExport(gl.module, "glClearColor", gl.clearColor);

    resolveExport(gl.module, "glEnable", gl.enable);
    resolveExport(gl.module, "glDisable", gl.disable);
    resolveExport(gl.module, "glIsEnabled", gl.isEnabled);
    resolveExport(gl.module, "glGetIntegerv", gl.getIntegerv);
    resolveExport(gl.module, "glScissor", gl.scissor);

    // The cursor overlay's. Deliberately NOT in the return expression below: a driver
    // that somehow lacks one of these should cost the pointer overlay and nothing else.
    // CursorOverlay.cpp checks them itself and names them if they are missing.
    resolveExport(gl.module, "glPushAttrib", gl.pushAttrib);
    resolveExport(gl.module, "glPopAttrib", gl.popAttrib);
    resolveExport(gl.module, "glPushMatrix", gl.pushMatrix);
    resolveExport(gl.module, "glPopMatrix", gl.popMatrix);
    resolveExport(gl.module, "glBegin", gl.begin);
    resolveExport(gl.module, "glEnd", gl.end);
    resolveExport(gl.module, "glVertex2f", gl.vertex2f);
    resolveExport(gl.module, "glVertex4f", gl.vertex4f);
    resolveExport(gl.module, "glTexCoord2f", gl.texCoord2f);
    resolveExport(gl.module, "glColor4f", gl.color4f);
    resolveExport(gl.module, "glPolygonMode", gl.polygonMode);
    resolveExport(gl.module, "glColorMask", gl.colorMask);
    resolveExport(gl.module, "glBlendFunc", gl.blendFunc);
    resolveExport(gl.module, "glTexEnvi", gl.texEnvi);
    resolveExport(gl.module, "glDepthMask", gl.depthMask);

    return gl.wglGetCurrentContext && gl.wglGetCurrentDC && gl.wglGetProcAddress &&
           gl.getString && gl.getError && gl.finish && gl.readPixels &&
           gl.copyTexSubImage2D && gl.genTextures && gl.deleteTextures &&
           gl.bindTexture && gl.readBuffer;
}

bool glResolveInterop(GlFunctions& gl, HDC dc) {
    const char* extensions = nullptr;
    WglGetExtensionsStringArbFn arb = nullptr;
    resolveExtension(gl, "wglGetExtensionsStringARB", arb);
    if (arb) {
        extensions = arb(dc);
    } else {
        WglGetExtensionsStringExtFn ext = nullptr;
        resolveExtension(gl, "wglGetExtensionsStringEXT", ext);
        if (ext) extensions = ext();
    }

    const bool advertised =
        extensions && (std::strstr(extensions, "WGL_NV_DX_interop2") ||
                       std::strstr(extensions, "WGL_NV_DX_interop"));
    if (!advertised) {
        logf("gl: WGL_NV_DX_interop is %s",
             extensions ? "NOT advertised by this driver" : "unknown -- no WGL "
                                                            "extension string");
        return false;
    }

    resolveExtension(gl, "wglDXOpenDeviceNV", gl.dxOpenDevice);
    resolveExtension(gl, "wglDXCloseDeviceNV", gl.dxCloseDevice);
    resolveExtension(gl, "wglDXRegisterObjectNV", gl.dxRegisterObject);
    resolveExtension(gl, "wglDXUnregisterObjectNV", gl.dxUnregisterObject);
    resolveExtension(gl, "wglDXLockObjectsNV", gl.dxLockObjects);
    resolveExtension(gl, "wglDXUnlockObjectsNV", gl.dxUnlockObjects);

    if (!gl.dxOpenDevice || !gl.dxCloseDevice || !gl.dxRegisterObject ||
        !gl.dxUnregisterObject || !gl.dxLockObjects || !gl.dxUnlockObjects) {
        logf("gl: WGL_NV_DX_interop is advertised but its entry points did not resolve");
        return false;
    }
    return true;
}

bool glResolveFramebuffer(GlFunctions& gl) {
    if (!gl.wglGetProcAddress) return false;

    // ARB/core name first, EXT second. Both spellings take the same arguments; the only
    // reason to prefer the core one is that a core context can refuse the EXT entry
    // points on some drivers, and the reverse has never been true.
    struct Pair {
        const char* core;
        const char* ext;
        void**      slot;
    };
    const Pair wanted[] = {
        {"glGenFramebuffers", "glGenFramebuffersEXT", (void**)&gl.genFramebuffers},
        {"glDeleteFramebuffers", "glDeleteFramebuffersEXT", (void**)&gl.deleteFramebuffers},
        {"glBindFramebuffer", "glBindFramebufferEXT", (void**)&gl.bindFramebuffer},
        {"glFramebufferTexture2D", "glFramebufferTexture2DEXT",
         (void**)&gl.framebufferTexture2D},
        {"glGenRenderbuffers", "glGenRenderbuffersEXT", (void**)&gl.genRenderbuffers},
        {"glDeleteRenderbuffers", "glDeleteRenderbuffersEXT",
         (void**)&gl.deleteRenderbuffers},
        {"glBindRenderbuffer", "glBindRenderbufferEXT", (void**)&gl.bindRenderbuffer},
        {"glRenderbufferStorage", "glRenderbufferStorageEXT",
         (void**)&gl.renderbufferStorage},
        {"glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT",
         (void**)&gl.framebufferRenderbuffer},
        {"glCheckFramebufferStatus", "glCheckFramebufferStatusEXT",
         (void**)&gl.checkFramebufferStatus},
        {"glBlitFramebuffer", "glBlitFramebufferEXT", (void**)&gl.blitFramebuffer},
    };

    bool complete = true;
    int  core = 0;
    int  ext = 0;
    for (const Pair& entry : wanted) {
        void* fn = reinterpret_cast<void*>(gl.wglGetProcAddress(entry.core));
        if (fn) {
            ++core;
        } else {
            fn = reinterpret_cast<void*>(gl.wglGetProcAddress(entry.ext));
            if (fn) ++ext;
        }
        *entry.slot = fn;
        if (!fn) {
            logf("gl: %s / %s did not resolve", entry.core, entry.ext);
            complete = false;
        }
    }
    gl.framebufferIsExt = ext > 0;

    // Say which spelling won. The two are not interchangeable at the edges -- the EXT
    // entry points come from EXT_framebuffer_object, which has one framebuffer binding,
    // and the separate READ and DRAW targets the window blit needs arrived later. When
    // this line says EXT, the split is the first thing to suspect.
    logf("gl: framebuffer entry points resolved -- %d core, %d EXT", core, ext);

    // The core GL 1.1 calls come from opengl32 directly and are resolved in glResolve;
    // check them here too, because an off-screen target that cannot set its own viewport
    // would render the whole frame into the bottom-left corner and look like a bug in
    // the projection rather than a missing entry point.
    if (!gl.viewport || !gl.matrixMode || !gl.loadIdentity || !gl.ortho ||
        !gl.translatef || !gl.texImage2D || !gl.texParameteri || !gl.clear ||
        !gl.clearColor) {
        logf("gl: the core entry points the off-screen target needs are incomplete");
        complete = false;
    }
    return complete;
}

bool glDrawableSize(const GlFunctions& gl, int& width, int& height) {
    if (!gl.wglGetCurrentDC) return false;
    const HDC dc = gl.wglGetCurrentDC();
    if (!dc) return false;

    RECT client = {};
    if (!GetClientRect(WindowFromDC(dc), &client)) return false;
    width = client.right - client.left;
    height = client.bottom - client.top;
    return width > 0 && height > 0;
}

HANDLE glInteropOpenDevice(const GlFunctions& gl, ID3D11Device* device) {
    if (!gl.dxOpenDevice || !device) return nullptr;
    HANDLE interopDevice = gl.dxOpenDevice(device);
    if (!interopDevice)
        logf("gl: wglDXOpenDeviceNV failed (GL error 0x%04X)", gl.getError());
    return interopDevice;
}

void glInteropCloseDevice(const GlFunctions& gl, HANDLE interopDevice) {
    if (interopDevice && gl.dxCloseDevice) gl.dxCloseDevice(interopDevice);
}

bool glInteropRegister(const GlFunctions& gl, HANDLE interopDevice,
                       ID3D11Texture2D* texture, GlSharedTexture& out) {
    if (!interopDevice || !texture) return false;

    out.texture = texture;

    // The legacy share handle, not an NT handle. It is valid in any process on the same
    // adapter, which is exactly what lets a 32-bit producer hand a texture to a 64-bit
    // presenter without duplicating anything.
    IDXGIResource* resource = nullptr;
    if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), (void**)&resource))) {
        resource->GetSharedHandle(&out.shareHandle);
        resource->Release();
    }
    if (!out.shareHandle) {
        logf("gl: the texture has no share handle, so the presenter could not open it");
        return false;
    }

    gl.genTextures(1, &out.name);
    out.interopObject = gl.dxRegisterObject(interopDevice, texture, out.name,
                                            GL_TEXTURE_2D, kWglAccessWriteDiscardNv);
    if (!out.interopObject) {
        logf("gl: wglDXRegisterObjectNV failed (GL error 0x%04X)", gl.getError());
        gl.deleteTextures(1, &out.name);
        out.name = 0;
        return false;
    }

    // The destination framebuffer for the blit path. Only the NAME is made here: the
    // texture is attached inside the copy, while the interop object is locked, because
    // GL may not touch a shared object outside a lock.
    if (gl.genFramebuffers) gl.genFramebuffers(1, &out.framebuffer);
    return true;
}

void glInteropUnregister(const GlFunctions& gl, HANDLE interopDevice,
                         GlSharedTexture& shared) {
    if (shared.framebuffer && gl.deleteFramebuffers)
        gl.deleteFramebuffers(1, &shared.framebuffer);
    shared.framebuffer = 0;

    if (shared.interopObject && gl.dxUnregisterObject)
        gl.dxUnregisterObject(interopDevice, shared.interopObject);
    shared.interopObject = nullptr;

    if (shared.name && gl.deleteTextures) gl.deleteTextures(1, &shared.name);
    shared.name = 0;

    // The texture itself is owned by the caller; only the registration is ours.
    shared.texture = nullptr;
    shared.shareHandle = nullptr;
}

bool glReadBackBufferBgra(const GlFunctions& gl, int width, int height, void* pixels) {
    if (!gl.readPixels || !gl.readBuffer || !gl.pixelStorei || !gl.finish ||
        !gl.getError || !pixels || width <= 0 || height <= 0)
        return false;

    // Drain anything the game left behind, or the check below would blame us for it.
    // Bounded, because a context in a bad state can report the same error forever.
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}

    gl.readBuffer(GL_BACK);
    gl.pixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.readPixels(0, 0, width, height, kGlBgraExt, GL_UNSIGNED_BYTE, pixels);
    gl.finish();
    return gl.getError() == GL_NO_ERROR;
}

bool glReadRectPacked(const GlFunctions& gl, int x, int y, int width, int height, GLenum format,
                      void* pixels) {
    if (!gl.readPixels || !gl.pixelStorei || !gl.getIntegerv) return false;
    GLint alignment = 4;
    GLint rowLength = 0;
    GLint skipRows = 0;
    GLint skipPixels = 0;
    gl.getIntegerv(GL_PACK_ALIGNMENT, &alignment);
    gl.getIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
    gl.getIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    gl.getIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
    gl.pixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.pixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl.pixelStorei(GL_PACK_SKIP_ROWS, 0);
    gl.pixelStorei(GL_PACK_SKIP_PIXELS, 0);
    gl.readPixels(x, y, width, height, format, GL_UNSIGNED_BYTE, pixels);
    gl.pixelStorei(GL_PACK_ALIGNMENT, alignment);
    gl.pixelStorei(GL_PACK_ROW_LENGTH, rowLength);
    gl.pixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    gl.pixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
    return true;
}

namespace {

bool panelUsable(const GlFunctions& gl, const GlPanelCopy* panel) {
    return panel && panel->shared && panel->shared->interopObject &&
           panel->shared->framebuffer && panel->sourceFramebuffer && panel->width > 0 &&
           panel->height > 0 && gl.blitFramebuffer && gl.bindFramebuffer &&
           gl.framebufferTexture2D;
}

// The panel's blit into its shared texture, under a lock the caller has already taken.
// Returns the name of the call that failed, or null.
//
// Both transfer paths use it, and they lock differently: the copy takes the frame and the
// panel in one wglDXLockObjectsNV, while the direct path locks the panel on its own because
// the frame's lock was taken before its first draw call.
const char* copyPanelLocked(const GlFunctions& gl, const GlPanelCopy& panel,
                            GLint previousDraw, GLenum& failedWith) {
    const char* failedAt = nullptr;
    auto        step = [&](const char* what) {
        if (failedAt) return;
        const GLenum error = gl.getError();
        if (error != GL_NO_ERROR) {
            failedAt = what;
            failedWith = error;
        }
    };

    // The same attach, blit, detach order as the frame's, for the same reasons: an
    // incomplete destination makes the blit a silent no-op, and a texture still attached at
    // the unlock makes the unlock fail.
    frameBudgetSwitch(kSpanTransferPanel);
    const GlSharedTexture& target = *panel.shared;
    gl.bindFramebuffer(kGlDrawFramebuffer, target.framebuffer);
    gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                            target.name, 0);
    step("glFramebufferTexture2D(panel shared texture)");
    if (gl.checkFramebufferStatus) {
        const GLenum status = gl.checkFramebufferStatus(kGlDrawFramebuffer);
        if (status != kGlFramebufferComplete && !failedAt) {
            failedAt = "the panel's shared framebuffer is incomplete";
            failedWith = status;
        }
    }
    gl.bindFramebuffer(kGlReadFramebuffer, panel.sourceFramebuffer);
    gl.readBuffer(kGlColorAttachment0);
    step("glReadBuffer(COLOR_ATTACHMENT0) on the panel framebuffer");
    if (!failedAt) {
        gl.blitFramebuffer(0, 0, panel.width, panel.height, 0, 0, panel.width, panel.height,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
        step("glBlitFramebuffer(panel)");
    }
    gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, 0, 0);
    step("glFramebufferTexture2D(panel detach)");
    gl.bindFramebuffer(kGlReadFramebuffer, 0);
    gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(previousDraw));
    gl.readBuffer(GL_BACK);
    step("restoring the bindings after the panel");
    frameBudgetSwitch(kSpanTransfer);
    return failedAt;
}

// Report on CHANGE, with a cap, shared by both paths so the budget is one budget.
void reportPanel(const GlPanelCopy* panel, const char* failedAt, GLenum failedWith,
                 const char* how) {
    static const char* reported = nullptr;
    static int         reportsLeft = 8;
    if (!panel || !panel->shared) return;
    if (failedAt == reported || reportsLeft <= 0) return;
    --reportsLeft;
    reported = failedAt;
    if (failedAt)
        logf("gl: the HUD panel copy failed at %s with 0x%04X -- %dx%d, source framebuffer "
             "%u, shared texture %u; the headset keeps the last panel it had",
             failedAt, failedWith, panel->width, panel->height, panel->sourceFramebuffer,
             panel->shared->name);
    else
        logf("gl: the HUD panel reaches the headset -- %dx%d, copied %s", panel->width,
             panel->height, how);
}

// The menu panel's own report, with a budget of its own. Sharing reportPanel's statics
// would let a HUD failure spend the menu's eight lines and leave the menu silent, which
// is the one thing a panel that does not appear must not be.
void reportMenuPanel(const GlPanelCopy& panel, const char* failedAt, GLenum failedWith) {
    static const char* reported = nullptr;
    static int         reportsLeft = 8;
    if (failedAt == reported || reportsLeft <= 0) return;
    --reportsLeft;
    reported = failedAt;
    if (failedAt)
        logf("gl: the VR menu panel copy failed at %s with 0x%04X -- %dx%d, source "
             "framebuffer %u, shared texture %u; the headset keeps the last image it had",
             failedAt, failedWith, panel.width, panel.height, panel.sourceFramebuffer,
             panel.shared->name);
    else
        logf("gl: the VR menu panel reaches the headset -- %dx%d, under a lock of its own",
             panel.width, panel.height);
}

}  // namespace

bool glInteropPublishPanel(const GlFunctions& gl, HANDLE interopDevice,
                           GlPanelCopy& panel) {
    panel.copied = false;
    if (!gl.dxLockObjects || !gl.dxUnlockObjects || !gl.getError) return false;
    if (!panelUsable(gl, &panel)) return false;

    // Drain first, for the same reason the frame paths do: a sticky error from somebody
    // else's call would be read as this one's and send the next round chasing it.
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {
    }

    HANDLE object = panel.shared->interopObject;
    if (!gl.dxLockObjects(interopDevice, 1, &object)) {
        for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {
        }
        reportMenuPanel(panel, "wglDXLockObjectsNV on the menu panel texture", GL_NO_ERROR);
        return false;
    }

    GLint previousDraw = 0;
    if (gl.getIntegerv) gl.getIntegerv(kGlFramebufferBinding, &previousDraw);
    GLenum      failedWith = GL_NO_ERROR;
    const char* failedAt = copyPanelLocked(gl, panel, previousDraw, failedWith);

    int  unlocking = frameBudgetEnter(kSpanWaitUnlock);
    BOOL unlocked = gl.dxUnlockObjects(interopDevice, 1, &object);
    frameBudgetLeave(unlocking);
    if (!unlocked && !failedAt) {
        failedAt = "wglDXUnlockObjectsNV returned FALSE";
        failedWith = GL_NO_ERROR;
    }

    panel.copied = !failedAt;
    reportMenuPanel(panel, failedAt, failedWith);
    return panel.copied;
}

bool glInteropCopyBackBuffer(const GlFunctions& gl, HANDLE interopDevice,
                             const GlSharedTexture& shared, int width, int height,
                             GLuint sourceFramebuffer, bool finish, GlPanelCopy* panel) {
    if (!shared.interopObject) return false;
    if (panel) panel->copied = false;

    // Drain first, and this is not tidiness.
    //
    // This function reports failure on ANY pending GL error, and a pending error is
    // sticky until read. So an error raised by something else -- the game, or our own
    // window blit a few calls earlier -- was being blamed on the copy, which then
    // refused to publish the frame. Every frame. Forever. A black headset caused by an
    // error the copy did not make and could not fix.
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}

    // Name the failing call rather than the function.
    //
    // Two runs were lost to "the interop copy left GL error 0x0502", which says which
    // rule was broken but not by whom -- and the wrong call was guessed both times.
    //
    // This runs on EVERY frame, not on a sample of them, and that is the whole point.
    // The first version only checked for a few frames after a texture recreation, and
    // because reading an error also CLEARS it, those few frames drained the error and
    // published while every later frame inherited it and did not. An instrument that
    // changes the thing it measures produces exactly one real frame and then black --
    // which is what the second run showed and what the report of "one frame, then it
    // went black" describes.
    static const char* reported = nullptr;
    static int         reportsLeft = 8;

    const char* failedAt = nullptr;
    GLenum      failedWith = GL_NO_ERROR;
    auto step = [&](const char* what) {
        if (failedAt) return;
        const GLenum error = gl.getError();
        if (error != GL_NO_ERROR) {
            failedAt = what;
            failedWith = error;
        }
    };

    // The panel goes under the same lock as the frame when there is one to copy. If the pair
    // is refused the frame is locked alone, so a panel problem never costs the headset its
    // picture.
    const char* panelFailedAt = nullptr;
    GLenum      panelFailedWith = GL_NO_ERROR;

    bool panelWanted = panelUsable(gl, panel);

    HANDLE objects[2] = {shared.interopObject,
                         panelWanted ? panel->shared->interopObject : nullptr};
    int    lockedCount = panelWanted ? 2 : 1;
    if (!gl.dxLockObjects(interopDevice, lockedCount, objects)) {
        if (!panelWanted) return false;
        for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}
        panelFailedAt = "wglDXLockObjectsNV on the frame and panel textures together";
        panelFailedWith = GL_NO_ERROR;
        panelWanted = false;
        lockedCount = 1;
        if (!gl.dxLockObjects(interopDevice, 1, objects)) return false;
    }
    step("wglDXLockObjectsNV");

    GLint previousDraw = 0;
    if (gl.getIntegerv) gl.getIntegerv(kGlFramebufferBinding, &previousDraw);

    if (panelWanted)
        panelFailedAt = copyPanelLocked(gl, *panel, previousDraw, panelFailedWith);

    const bool blitPath = sourceFramebuffer != 0 && shared.framebuffer != 0 &&
                          gl.blitFramebuffer && gl.bindFramebuffer &&
                          gl.framebufferTexture2D;

    if (blitPath) {
        // glCopyTexSubImage2D with an off-screen read source returns INVALID_OPERATION on
        // this driver, with the errors drained beforehand so it could only have been its
        // own. A framebuffer-to-framebuffer blit does the same job, converts the format
        // the same way, and is the path every modern engine uses to fill a shared
        // surface.
        gl.bindFramebuffer(kGlDrawFramebuffer, shared.framebuffer);
        step("glBindFramebuffer(DRAW, shared)");
        gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                                shared.name, 0);
        step("glFramebufferTexture2D(shared texture)");

        // Checked every frame, not sampled. An incomplete destination makes the blit a
        // silent no-op rather than an error -- a black headset with nothing in the log.
        if (gl.checkFramebufferStatus) {
            const GLenum status = gl.checkFramebufferStatus(kGlDrawFramebuffer);
            if (status != kGlFramebufferComplete && !failedAt) {
                failedAt = "the shared texture's framebuffer is incomplete";
                failedWith = status;
            }
        }

        gl.bindFramebuffer(kGlReadFramebuffer, sourceFramebuffer);
        gl.readBuffer(kGlColorAttachment0);
        step("glReadBuffer(COLOR_ATTACHMENT0) on the render target");

        gl.blitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                           GL_NEAREST);
        step("glBlitFramebuffer");

        // DETACH the shared texture before the unlock below. This is the bug that made
        // the race black.
        //
        // wglDXUnlockObjectsNV hands the object back to Direct3D, and GL must not still
        // be holding it. The copy path satisfied that by accident -- it ends with
        // glBindTexture(GL_TEXTURE_2D, 0) -- while the blit path left the texture sitting
        // in a framebuffer's colour attachment. Every step of the blit reported success
        // and then the unlock raised INVALID_OPERATION, which the function read at the end
        // and turned into "no frame this time". Every frame, behind a log line saying the
        // transfer worked.
        gl.framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, 0,
                                0);
        step("glFramebufferTexture2D(detach)");

        // Unbind BEFORE naming GL_BACK. GL_BACK is not a valid read buffer for a
        // framebuffer object, so setting it while the read binding is still the render
        // target is itself an INVALID_OPERATION -- which would have arrived at the unlock
        // looking exactly like the bug above and sent the next round chasing it again.
        gl.bindFramebuffer(kGlReadFramebuffer, 0);
        gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(previousDraw));
        gl.readBuffer(GL_BACK);
        step("restoring the framebuffer bindings");
    } else {
        // The binding found here goes back afterwards -- roglv2 keeps its own copy of the
        // bound texture and does not rebind one it believes is bound (display +0x38) -- and
        // the shared texture is never left bound, which the unlock below requires.
        GLint previousTexture = 0;
        if (gl.getIntegerv) gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        if (static_cast<GLuint>(previousTexture) == shared.name) previousTexture = 0;
        gl.readBuffer(GL_BACK);
        gl.bindTexture(GL_TEXTURE_2D, shared.name);
        step("glBindTexture(shared)");
        gl.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        step("glCopyTexSubImage2D");
        gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    }

    // One unlock for everything locked above. On NVIDIA this is where the CPU waits for the
    // GL work on those textures to complete, so the frame budget books it as a wait.
    int unlocking = frameBudgetEnter(kSpanWaitUnlock);
    BOOL unlocked = gl.dxUnlockObjects(interopDevice, lockedCount, objects);
    frameBudgetLeave(unlocking);
    step("wglDXUnlockObjectsNV");
    if (!unlocked && !failedAt) {
        failedAt = "wglDXUnlockObjectsNV returned FALSE";
        failedWith = GL_NO_ERROR;
    }

    if (panelWanted) {
        if (!panelFailedAt && (failedAt || !unlocked))
            panelFailedAt = "the frame's own transfer failed under the shared lock";
        panel->copied = !panelFailedAt;
    }
    reportPanel(panel, panelFailedAt, panelFailedWith,
                "under the frame transfer's own lock -- one "
                "wglDXLockObjectsNV/wglDXUnlockObjectsNV pair for both textures");

    // Report on CHANGE, with a cap. Every frame would be a flood and one frame would be
    // a startup sample; what is wanted is a line each time the answer becomes different.
    if (failedAt != reported && reportsLeft > 0) {
        --reportsLeft;
        reported = failedAt;
        if (failedAt)
            logf("gl: the frame transfer failed at %s with 0x%04X -- %s path, %dx%d, "
                 "source framebuffer %u, shared texture %u, its framebuffer %u",
                 failedAt, failedWith, blitPath ? "blit" : "copy", width, height,
                 sourceFramebuffer, shared.name, shared.framebuffer);
        else
            logf("gl: the frame transfer works -- %s path, %dx%d, source framebuffer %u",
                 blitPath ? "blit" : "copy", width, height, sourceFramebuffer);
    }

    // The transfer only queues the work. Without a synchronisation the frame would be
    // published while it was still in flight and the presenter could read a half-written
    // texture -- which would look like tearing and be blamed on the protocol.
    //
    // NV_DX_interop makes the unlock above that synchronisation: it returns the objects to
    // Direct3D, and NVIDIA's implementation completes the GL work on them first (measured as
    // the ~1 ms the panel's own unlock used to wait). This glFinish then drains whatever else
    // is queued, and vrTransferFinish = 0 skips it to find out whether that matters.
    //
    // Time in here is the CPU waiting for the GPU, so the frame budget books it as a wait.
    if (finish) {
        int waiting = frameBudgetEnter(kSpanWaitTransfer);
        gl.finish();
        frameBudgetLeave(waiting);
    }

    // The step checks above are the verdict. Reading glGetError again here would find
    // nothing, because reading an error clears it -- which is exactly how the first
    // version ended up with two different answers on alternate frames.
    if (failedAt) return false;

    // A backstop only. Every call this function makes is checked by step() above, so
    // anything arriving here was raised by glFinish or by the driver behind our back --
    // and the last time a bare check like this stood in front of an unchecked sequence,
    // it reported a rule number for three runs while the culprit went unnamed.
    const GLenum error = gl.getError();
    if (error) {
        static GLenum lastError = 0;
        if (error != lastError) {
            lastError = error;
            logf("gl: GL error 0x%04X after the frame transfer, raised by no call this "
                 "function makes -- one or two around a resolution change is expected, a "
                 "steady stream is not",
                 error);
        }
        return false;
    }
    return true;
}

bool glInteropLockDirect(const GlFunctions& gl, HANDLE interopDevice,
                         const GlSharedTexture& shared) {
    if (!shared.interopObject || !shared.name || !gl.dxLockObjects) return false;

    // Drained first, for the reason the copy gives at length: a pending error is sticky,
    // and one raised by the game would otherwise be read here and blamed on the lock.
    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}

    HANDLE object = shared.interopObject;
    if (!gl.dxLockObjects(interopDevice, 1, &object)) {
        logf("gl: wglDXLockObjectsNV refused the frame's shared texture %u (GL error "
             "0x%04X)",
             shared.name, gl.getError());
        return false;
    }
    GLenum error = gl.getError();
    if (error)
        logf("gl: wglDXLockObjectsNV took the frame's shared texture %u but left GL error "
             "0x%04X",
             shared.name, error);
    return true;
}

bool glInteropPublishDirect(const GlFunctions& gl, HANDLE interopDevice,
                            const GlSharedTexture& shared, bool finish,
                            GlPanelCopy* panel) {
    if (!shared.interopObject || !gl.dxUnlockObjects) return false;
    if (panel) panel->copied = false;

    for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}

    const char* panelFailedAt = nullptr;
    GLenum      panelFailedWith = GL_NO_ERROR;

    // The frame's object has been locked since before its first draw call, so only the
    // panel's is taken here -- and both are handed back in one unlock, which keeps the
    // frame's cost at the one interop synchronisation it has always been.
    HANDLE objects[2] = {shared.interopObject, nullptr};
    int    lockedCount = 1;
    if (panelUsable(gl, panel)) {
        HANDLE panelObject = panel->shared->interopObject;
        if (gl.dxLockObjects(interopDevice, 1, &panelObject)) {
            objects[1] = panelObject;
            lockedCount = 2;
            GLint previousDraw = 0;
            if (gl.getIntegerv) gl.getIntegerv(kGlFramebufferBinding, &previousDraw);
            panelFailedAt = copyPanelLocked(gl, *panel, previousDraw, panelFailedWith);
        } else {
            for (int i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {}
            panelFailedAt = "wglDXLockObjectsNV on the panel texture";
        }
    }

    // Where the CPU waits for the GPU on NVIDIA, as it does for the copy -- with the
    // difference that the work it waits for is the frame itself rather than a blit of it.
    int  unlocking = frameBudgetEnter(kSpanWaitUnlock);
    BOOL unlocked = gl.dxUnlockObjects(interopDevice, lockedCount, objects);
    frameBudgetLeave(unlocking);

    const char* failedAt = nullptr;
    GLenum      failedWith = gl.getError();
    if (!unlocked)
        failedAt = "wglDXUnlockObjectsNV returned FALSE";
    else if (failedWith != GL_NO_ERROR)
        failedAt = "wglDXUnlockObjectsNV";

    if (lockedCount == 2) {
        if (!panelFailedAt && failedAt) panelFailedAt = "the frame's own unlock failed";
        panel->copied = !panelFailedAt;
    }
    reportPanel(panel, panelFailedAt, panelFailedWith,
                "under its own lock, handed back with the frame's");

    static const char* reported = nullptr;
    static int         reportsLeft = 8;
    if (failedAt != reported && reportsLeft > 0) {
        --reportsLeft;
        reported = failedAt;
        if (failedAt)
            logf("gl: publishing the directly rendered frame failed at %s with 0x%04X -- "
                 "shared texture %u. The most likely cause is the texture still being "
                 "attached to a framebuffer: GL may not hold a shared object at the unlock.",
                 failedAt, failedWith, shared.name);
        else
            logf("gl: the frame is rendered straight into shared texture %u, and the unlock "
                 "hands it over with no copy",
                 shared.name);
    }

    if (finish) {
        int waiting = frameBudgetEnter(kSpanWaitTransfer);
        gl.finish();
        frameBudgetLeave(waiting);
    }

    return failedAt == nullptr;
}

}  // namespace gplvr
