#pragma once

// roglv2's OpenGL back buffer, reachable as a D3D11 texture.
//
// Extracted from the Phase 2 transport probe once it stopped being a probe. The measured
// result it exists to exploit: copying the back buffer into a shared D3D11 texture
// through WGL_NV_DX_interop2 costs 0.21 ms at 14 MB a frame, against 1.36 ms for
// glReadPixels, and barely grows with resolution because it never leaves the GPU.
//
// Everything is resolved by name. Linking opengl32 or d3d11 would put imports in
// gplvr.dll and load both into the game whether or not VR is switched on -- and with the
// D3D7 rasterizer selected there is no GL context at all, which has to stay a logged
// note rather than a failure to load.

#include <windows.h>
#include <GL/gl.h>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace gplvr {

typedef HGLRC(WINAPI* WglGetCurrentContextFn)();
typedef HDC(WINAPI* WglGetCurrentDcFn)();
typedef PROC(WINAPI* WglGetProcAddressFn)(LPCSTR);

typedef const GLubyte*(WINAPI* GlGetStringFn)(GLenum);
typedef GLenum(WINAPI* GlGetErrorFn)();
typedef void(WINAPI* GlFinishFn)();
typedef void(WINAPI* GlReadBufferFn)(GLenum);
typedef void(WINAPI* GlReadPixelsFn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                     void*);
typedef void(WINAPI* GlGenTexturesFn)(GLsizei, GLuint*);
typedef void(WINAPI* GlDeleteTexturesFn)(GLsizei, const GLuint*);
typedef void(WINAPI* GlBindTextureFn)(GLenum, GLuint);
typedef void(WINAPI* GlCopyTexSubImage2DFn)(GLenum, GLint, GLint, GLint, GLint, GLint,
                                            GLsizei, GLsizei);
typedef void(WINAPI* GlPixelStoreiFn)(GLenum, GLint);

typedef HANDLE(WINAPI* WglDXOpenDeviceFn)(void*);
typedef BOOL(WINAPI* WglDXCloseDeviceFn)(HANDLE);
typedef HANDLE(WINAPI* WglDXRegisterObjectFn)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* WglDXUnregisterObjectFn)(HANDLE, HANDLE);
typedef BOOL(WINAPI* WglDXObjectAccessFn)(HANDLE, GLint, HANDLE*);

// Not in the GL 1.1 headers the Windows SDK ships.
inline constexpr GLenum kGlBgraExt = 0x80E1;
inline constexpr GLenum kWglAccessWriteDiscardNv = 0x00000002;

struct GlFunctions {
    HMODULE                module = nullptr;
    WglGetCurrentContextFn wglGetCurrentContext = nullptr;
    WglGetCurrentDcFn      wglGetCurrentDC = nullptr;
    WglGetProcAddressFn    wglGetProcAddress = nullptr;

    GlGetStringFn         getString = nullptr;
    GlGetErrorFn          getError = nullptr;
    GlFinishFn            finish = nullptr;
    GlReadBufferFn        readBuffer = nullptr;
    GlReadPixelsFn        readPixels = nullptr;
    GlGenTexturesFn       genTextures = nullptr;
    GlDeleteTexturesFn    deleteTextures = nullptr;
    GlBindTextureFn       bindTexture = nullptr;
    GlCopyTexSubImage2DFn copyTexSubImage2D = nullptr;
    GlPixelStoreiFn       pixelStorei = nullptr;

    WglDXOpenDeviceFn       dxOpenDevice = nullptr;
    WglDXCloseDeviceFn      dxCloseDevice = nullptr;
    WglDXRegisterObjectFn   dxRegisterObject = nullptr;
    WglDXUnregisterObjectFn dxUnregisterObject = nullptr;
    WglDXObjectAccessFn     dxLockObjects = nullptr;
    WglDXObjectAccessFn     dxUnlockObjects = nullptr;
};

// Resolves the core entry points. False means opengl32 is not in the process, i.e. the
// D3D7 rasterizer is selected and none of this applies.
bool glResolve(GlFunctions& gl);

// Must be called with the render thread's context current. Resolves the interop entry
// points and reports whether WGL_NV_DX_interop is advertised.
bool glResolveInterop(GlFunctions& gl, HDC dc);

// The client area of the window the current context draws to.
bool glDrawableSize(const GlFunctions& gl, int& width, int& height);

// One D3D11 texture registered as a GL texture.
struct GlSharedTexture {
    ID3D11Texture2D* texture = nullptr;
    HANDLE           shareHandle = nullptr;  // legacy, openable by the x64 presenter
    HANDLE           interopObject = nullptr;
    GLuint           name = 0;
};

// The D3D11 device must live on the adapter the OpenXR runtime named, or the share
// handle cannot be opened in the presenter at all.
HANDLE glInteropOpenDevice(const GlFunctions& gl, ID3D11Device* device);
void   glInteropCloseDevice(const GlFunctions& gl, HANDLE interopDevice);

bool glInteropRegister(const GlFunctions& gl, HANDLE interopDevice,
                       ID3D11Texture2D* texture, GlSharedTexture& out);
void glInteropUnregister(const GlFunctions& gl, HANDLE interopDevice,
                         GlSharedTexture& shared);

// Copy the current back buffer into the registered texture. Synchronous: it ends with
// glFinish, so when it returns the pixels really are there and the frame can be
// published. That costs the 0.21 ms measured above and removes any question about what
// the presenter might read half-written.
bool glInteropCopyBackBuffer(const GlFunctions& gl, HANDLE interopDevice,
                             const GlSharedTexture& shared, int width, int height);

// Pull the back buffer into system memory as BGRA, bottom-up, for the one-shot fov
// probe. This is the slow path the interop transport exists to avoid -- 1.36 ms and a
// full GPU sync -- which is why it is reserved for a keypress and never used per frame.
// `pixels` must hold width * height * 4 bytes.
bool glReadBackBufferBgra(const GlFunctions& gl, int width, int height, void* pixels);

}  // namespace gplvr
