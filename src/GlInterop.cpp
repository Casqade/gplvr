#include "GlInterop.h"

#include "Log.h"

#include <d3d11.h>

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

bool glInteropCopyBackBuffer(const GlFunctions& gl, HANDLE interopDevice,
                             const GlSharedTexture& shared, int width, int height,
                             GLuint sourceFramebuffer) {
    if (!shared.interopObject) return false;

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

    HANDLE object = shared.interopObject;
    if (!gl.dxLockObjects(interopDevice, 1, &object)) return false;
    step("wglDXLockObjectsNV");

    GLint previousDraw = 0;
    if (gl.getIntegerv) gl.getIntegerv(kGlFramebufferBinding, &previousDraw);

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
        gl.readBuffer(GL_BACK);
        gl.bindTexture(GL_TEXTURE_2D, shared.name);
        step("glBindTexture(shared)");
        gl.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        step("glCopyTexSubImage2D");
        gl.bindTexture(GL_TEXTURE_2D, 0);
    }

    gl.dxUnlockObjects(interopDevice, 1, &object);
    step("wglDXUnlockObjectsNV");

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

    // The transfer only queues the work. Without this the frame would be published
    // while it was still in flight and the presenter could read a half-written texture
    // -- which would look like tearing and be blamed on the protocol.
    gl.finish();

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

}  // namespace gplvr
