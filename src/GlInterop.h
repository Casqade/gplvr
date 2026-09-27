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
typedef void(WINAPI* GlGetTexLevelParameterivFn)(GLenum, GLint, GLenum, GLint*);
typedef void(WINAPI* GlPixelStoreiFn)(GLenum, GLint);
typedef void(WINAPI* GlTexImage2DFn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                     GLenum, GLenum, const void*);
typedef void(WINAPI* GlTexParameteriFn)(GLenum, GLenum, GLint);
typedef void(WINAPI* GlViewportFn)(GLint, GLint, GLsizei, GLsizei);
typedef void(WINAPI* GlMatrixModeFn)(GLenum);
typedef void(WINAPI* GlLoadIdentityFn)();
typedef void(WINAPI* GlOrthoFn)(double, double, double, double, double, double);
typedef void(WINAPI* GlTranslatefFn)(GLfloat, GLfloat, GLfloat);
typedef void(WINAPI* GlClearFn)(GLbitfield);
typedef void(WINAPI* GlClearColorFn)(GLfloat, GLfloat, GLfloat, GLfloat);

typedef void(WINAPI* GlEnableFn)(GLenum);
typedef GLboolean(WINAPI* GlIsEnabledFn)(GLenum);
typedef void(WINAPI* GlGetIntegervFn)(GLenum, GLint*);
typedef void(WINAPI* GlScissorFn)(GLint, GLint, GLsizei, GLsizei);

// Immediate mode, for the cursor overlay. GL 1.1 core, so all of these are plain
// opengl32 exports rather than extensions -- but they are still checked before use and
// still named if one is missing, because "the pointer did not appear" has to have a
// cause attached. glPushAttrib/glPopAttrib and the two matrix stacks are what make
// drawing inside the game's own context safe: every piece of state the overlay touches
// is saved and put back, the matrix MODE included, which rogl_displaySetDestination
// silently depends on.
typedef void(WINAPI* GlPushAttribFn)(GLbitfield);
typedef void(WINAPI* GlPopAttribFn)();
typedef void(WINAPI* GlPushMatrixFn)();
typedef void(WINAPI* GlPopMatrixFn)();
typedef void(WINAPI* GlBeginFn)(GLenum);
typedef void(WINAPI* GlEndFn)();
typedef void(WINAPI* GlVertex2fFn)(GLfloat, GLfloat);
typedef void(WINAPI* GlVertex4fFn)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(WINAPI* GlTexCoord2fFn)(GLfloat, GLfloat);
typedef void(WINAPI* GlColor4fFn)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(WINAPI* GlPolygonModeFn)(GLenum, GLenum);
typedef void(WINAPI* GlColorMaskFn)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void(WINAPI* GlBlendFuncFn)(GLenum, GLenum);
typedef void(WINAPI* GlTexEnviFn)(GLenum, GLenum, GLint);
typedef void(WINAPI* GlDepthMaskFn)(GLboolean);

// EXT_framebuffer_object / GL 3.0. Resolved through wglGetProcAddress, ARB names first
// and the EXT spelling as a fallback: roglv2 links a GLEW that only ever loads the EXT
// entry points, so the EXT names are certain to exist on any driver that runs the game.
typedef void(WINAPI* GlGenFramebuffersFn)(GLsizei, GLuint*);
typedef void(WINAPI* GlDeleteFramebuffersFn)(GLsizei, const GLuint*);
typedef void(WINAPI* GlBindFramebufferFn)(GLenum, GLuint);
typedef void(WINAPI* GlFramebufferTexture2DFn)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void(WINAPI* GlGenRenderbuffersFn)(GLsizei, GLuint*);
typedef void(WINAPI* GlDeleteRenderbuffersFn)(GLsizei, const GLuint*);
typedef void(WINAPI* GlBindRenderbufferFn)(GLenum, GLuint);
typedef void(WINAPI* GlRenderbufferStorageFn)(GLenum, GLenum, GLsizei, GLsizei);
typedef void(WINAPI* GlFramebufferRenderbufferFn)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum(WINAPI* GlCheckFramebufferStatusFn)(GLenum);
typedef void(WINAPI* GlBlitFramebufferFn)(GLint, GLint, GLint, GLint, GLint, GLint,
                                          GLint, GLint, GLbitfield, GLenum);

typedef HANDLE(WINAPI* WglDXOpenDeviceFn)(void*);
typedef BOOL(WINAPI* WglDXCloseDeviceFn)(HANDLE);
typedef HANDLE(WINAPI* WglDXRegisterObjectFn)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* WglDXUnregisterObjectFn)(HANDLE, HANDLE);
typedef BOOL(WINAPI* WglDXObjectAccessFn)(HANDLE, GLint, HANDLE*);

// Not in the GL 1.1 headers the Windows SDK ships.
inline constexpr GLenum kGlBgraExt = 0x80E1;
inline constexpr GLenum kWglAccessWriteDiscardNv = 0x00000002;

inline constexpr GLenum kGlFramebuffer = 0x8D40;
inline constexpr GLenum kGlReadFramebuffer = 0x8CA8;
inline constexpr GLenum kGlDrawFramebuffer = 0x8CA9;
inline constexpr GLenum kGlRenderbuffer = 0x8D41;
inline constexpr GLenum kGlColorAttachment0 = 0x8CE0;
inline constexpr GLenum kGlDepthAttachment = 0x8D00;
inline constexpr GLenum kGlDepthComponent24 = 0x81A6;
inline constexpr GLenum kGlFramebufferComplete = 0x8CD5;
inline constexpr GLenum kGlClampToEdge = 0x812F;
inline constexpr GLenum kGlFramebufferBinding = 0x8CA6;
inline constexpr GLenum kGlReadFramebufferBinding = 0x8CAA;

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

    // Asks a texture GL already owns how big it is and in what format. The mirror cache
    // uses it to check that a texture name read out of roglv2's own bitmap really is the
    // mirror's before anything is written through it.
    GlGetTexLevelParameterivFn getTexLevelParameteriv = nullptr;

    GlPixelStoreiFn       pixelStorei = nullptr;
    GlTexImage2DFn        texImage2D = nullptr;
    GlTexParameteriFn     texParameteri = nullptr;
    GlViewportFn          viewport = nullptr;
    GlMatrixModeFn        matrixMode = nullptr;
    GlLoadIdentityFn      loadIdentity = nullptr;
    GlOrthoFn             ortho = nullptr;
    GlTranslatefFn        translatef = nullptr;
    GlClearFn             clear = nullptr;
    GlClearColorFn        clearColor = nullptr;

    GlEnableFn            enable = nullptr;
    GlEnableFn            disable = nullptr;
    GlIsEnabledFn         isEnabled = nullptr;
    GlGetIntegervFn       getIntegerv = nullptr;
    GlScissorFn           scissor = nullptr;

    GlPushAttribFn  pushAttrib = nullptr;
    GlPopAttribFn   popAttrib = nullptr;
    GlPushMatrixFn  pushMatrix = nullptr;
    GlPopMatrixFn   popMatrix = nullptr;
    GlBeginFn       begin = nullptr;
    GlEndFn         end = nullptr;
    GlVertex2fFn    vertex2f = nullptr;
    GlVertex4fFn    vertex4f = nullptr;
    GlTexCoord2fFn  texCoord2f = nullptr;
    GlColor4fFn     color4f = nullptr;
    GlPolygonModeFn polygonMode = nullptr;
    GlColorMaskFn   colorMask = nullptr;
    GlBlendFuncFn   blendFunc = nullptr;
    GlTexEnviFn     texEnvi = nullptr;
    GlDepthMaskFn   depthMask = nullptr;

    // Which spelling each framebuffer entry point resolved to. Not cosmetic: the EXT
    // entry points predate GL 3.0's split of the framebuffer binding into a READ and a
    // DRAW target, and a call that passes GL_READ_FRAMEBUFFER to an entry point that
    // does not know it fails with INVALID_ENUM, leaves the binding alone, and makes
    // every later call read from the wrong place.
    bool framebufferIsExt = false;

    GlGenFramebuffersFn         genFramebuffers = nullptr;
    GlDeleteFramebuffersFn      deleteFramebuffers = nullptr;
    GlBindFramebufferFn         bindFramebuffer = nullptr;
    GlFramebufferTexture2DFn    framebufferTexture2D = nullptr;
    GlGenRenderbuffersFn        genRenderbuffers = nullptr;
    GlDeleteRenderbuffersFn     deleteRenderbuffers = nullptr;
    GlBindRenderbufferFn        bindRenderbuffer = nullptr;
    GlRenderbufferStorageFn     renderbufferStorage = nullptr;
    GlFramebufferRenderbufferFn framebufferRenderbuffer = nullptr;
    GlCheckFramebufferStatusFn  checkFramebufferStatus = nullptr;
    GlBlitFramebufferFn         blitFramebuffer = nullptr;

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

// Framebuffer objects, for rendering off-screen at a size the window does not dictate.
// Must be called with a context current. False means the off-screen path is unavailable
// and everything falls back to rendering into the window's own back buffer.
bool glResolveFramebuffer(GlFunctions& gl);

// The client area of the window the current context draws to.
bool glDrawableSize(const GlFunctions& gl, int& width, int& height);

// One D3D11 texture registered as a GL texture.
struct GlSharedTexture {
    ID3D11Texture2D* texture = nullptr;
    HANDLE           shareHandle = nullptr;  // legacy, openable by the x64 presenter
    HANDLE           interopObject = nullptr;
    GLuint           name = 0;
    // A framebuffer with `name` as colour attachment 0, so the frame can be moved with
    // glBlitFramebuffer instead of glCopyTexSubImage2D. See the copy function.
    GLuint framebuffer = 0;
};

// The D3D11 device must live on the adapter the OpenXR runtime named, or the share
// handle cannot be opened in the presenter at all.
HANDLE glInteropOpenDevice(const GlFunctions& gl, ID3D11Device* device);
void   glInteropCloseDevice(const GlFunctions& gl, HANDLE interopDevice);

bool glInteropRegister(const GlFunctions& gl, HANDLE interopDevice,
                       ID3D11Texture2D* texture, GlSharedTexture& out);
void glInteropUnregister(const GlFunctions& gl, HANDLE interopDevice,
                         GlSharedTexture& shared);

// Copy the current render result into the registered texture. Synchronous: it ends with
// glFinish, so when it returns the pixels really are there and the frame can be
// published. That costs the 0.21 ms measured above and removes any question about what
// the presenter might read half-written.
//
// `sourceFramebuffer` is 0 for the window's own back buffer, or the name of an
// off-screen framebuffer whose colour attachment 0 holds the frame. The cost does not
// change with the source -- it is the same glCopyTexSubImage2D either way, which is why
// rendering off-screen adds no copy.
//
// From the window's back buffer it uses glCopyTexSubImage2D, which is proven. From an
// off-screen framebuffer it uses glBlitFramebuffer into a framebuffer wrapping the shared
// texture instead, because glCopyTexSubImage2D with an FBO as the read source returns
// INVALID_OPERATION here -- measured, twice, with errors drained beforehand so it could
// only have been its own. Which call in the sequence failed is reported for the first few
// frames after every texture recreation, so a failure names itself instead of arriving as
// a black headset.
// Write one rectangle of the framebuffer bound for READING to an uncompressed 24-bit TGA
// next to the game -- the caller binds the source; see dumpEyeRect in Hooks.cpp. This is
// a diagnostic: it exists so that "the pixels are in the frame" can be
// looked at rather than argued about. TGA because its default origin is bottom-left,
// which is GL's, so the rows go out in the order they come back with no flip to get wrong.
//
// Called at publish time with each eye's own rect, it shows exactly what the headset was
// handed -- not the combined side-by-side the monitor shows, which is the picture that
// has been misleading this investigation.
bool glDumpRectToTga(const GlFunctions& gl, int x, int y, int width, int height,
                     const char* path);

// The HUD panel's image, copied into its shared texture inside the frame transfer: both
// textures are locked in one wglDXLockObjectsNV and unlocked in one wglDXUnlockObjectsNV, so
// the frame costs one interop synchronisation rather than two. `sourceFramebuffer` is a
// framebuffer whose colour attachment 0 holds the panel, `width` x `height` like the shared
// texture. `copied` comes back true only when the panel reached its texture; a failed panel
// copy never fails the frame.
struct GlPanelCopy {
    const GlSharedTexture* shared = nullptr;
    GLuint                 sourceFramebuffer = 0;
    int                    width = 0;
    int                    height = 0;
    bool                   copied = false;
};

// `finish` issues the glFinish after the unlock (vrTransferFinish). `panel` may be null.
bool glInteropCopyBackBuffer(const GlFunctions& gl, HANDLE interopDevice,
                             const GlSharedTexture& shared, int width, int height,
                             GLuint sourceFramebuffer, bool finish, GlPanelCopy* panel);

// The direct path: the frame is drawn INTO the shared texture and the copy above never
// runs. See report 80 and vrDirectToShared.
//
// The lock is taken before the frame's first draw call and held until it is published, so
// a slot is unavailable to Direct3D for the whole frame rather than for the moment of a
// copy. The texture is registered WRITE_DISCARD, which is what rendering into it wants and
// also means a frame never starts from the previous one's pixels.
bool glInteropLockDirect(const GlFunctions& gl, HANDLE interopDevice,
                         const GlSharedTexture& shared);

// Finish a frame rendered straight into `shared`: copy the HUD panel if there is one, then
// unlock, and glFinish when `finish`. The unlock is the synchronisation, exactly as it is
// for the copy.
//
// THE CALLER MUST HAVE DETACHED the texture from every framebuffer first. GL may not still
// be holding a shared object at the unlock -- the bug that made the race black, recorded at
// the detach inside the copy.
bool glInteropPublishDirect(const GlFunctions& gl, HANDLE interopDevice,
                            const GlSharedTexture& shared, bool finish, GlPanelCopy* panel);

// Copy ONE panel into its shared texture under a lock of its own, outside the frame
// transfer entirely.
//
// The frame transfer batches the frame and exactly one panel into a single
// wglDXLockObjectsNV/wglDXUnlockObjectsNV pair (`HANDLE objects[2]` in both copy paths),
// and that batch is the most delicate code in this project -- the detach-before-unlock bug
// recorded inside it made every race black while reporting success. A SECOND panel, the VR
// menu's, takes its own pair rather than widening that one: it costs one extra interop
// synchronisation on the frames the menu is open, which is a menu and not a race, and the
// frame path keeps the single synchronisation it has always had.
//
// The blit itself is the same debugged routine both frame paths use, detach and all.
// `panel.copied` says whether the image reached the shared texture.
bool glInteropPublishPanel(const GlFunctions& gl, HANDLE interopDevice, GlPanelCopy& panel);

// Pull the back buffer into system memory as BGRA, bottom-up, for the one-shot fov
// probe. This is the slow path the interop transport exists to avoid -- 1.36 ms and a
// full GPU sync -- which is why it is reserved for a keypress and never used per frame.
// `pixels` must hold width * height * 4 bytes.
bool glReadBackBufferBgra(const GlFunctions& gl, int width, int height, void* pixels);

}  // namespace gplvr
