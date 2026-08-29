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
    return true;
}

void glInteropUnregister(const GlFunctions& gl, HANDLE interopDevice,
                         GlSharedTexture& shared) {
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
                             const GlSharedTexture& shared, int width, int height) {
    if (!shared.interopObject) return false;

    HANDLE object = shared.interopObject;
    if (!gl.dxLockObjects(interopDevice, 1, &object)) return false;

    gl.readBuffer(GL_BACK);
    gl.bindTexture(GL_TEXTURE_2D, shared.name);
    gl.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    gl.bindTexture(GL_TEXTURE_2D, 0);
    gl.dxUnlockObjects(interopDevice, 1, &object);

    // glCopyTexSubImage2D only queues the work. Without this the frame would be
    // published while the copy was still in flight and the presenter could read a
    // half-written texture -- which would look like tearing and be blamed on the
    // protocol.
    gl.finish();

    const GLenum error = gl.getError();
    if (error) {
        // Expected for a frame or two around a mode change: the drawable has already
        // been resized while GetClientRect still reports the old size, so the copy asks
        // for a rectangle the texture no longer matches and GL returns INVALID_VALUE.
        // The caller skips publishing that frame and the next one rebuilds at the new
        // size, so it is self-correcting. Log the first of a run and then stay quiet
        // rather than filling the file if it ever becomes persistent.
        static GLenum lastError = 0;
        if (error != lastError) {
            lastError = error;
            logf("gl: the interop copy left GL error 0x%04X -- harmless for a frame or "
                 "two around a resolution change; a steady stream of these is not",
                 error);
        }
        return false;
    }
    return true;
}

}  // namespace gplvr
