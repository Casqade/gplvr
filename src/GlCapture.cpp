#include "GlCapture.h"

#include "Log.h"

#include <windows.h>
#include <GL/gl.h>
#include <d3d11.h>

#include <cstring>
#include <vector>

namespace gplvr {
namespace {

// -- what we borrow from opengl32 and from the NV interop extension ---------------
//
// All of it by GetProcAddress. Linking opengl32 would put an import in gplvr.dll and
// load GL into the game whether or not the probe is switched on -- and with the D3D7
// rasterizer selected there is no GL context at all, which should be a logged "not
// applicable", not a failure to start. D3D11CreateDevice goes the same way, so d3d11 is
// only pulled into the process when the interop route is actually tried; <d3d11.h> is
// still included, because guessing vtable layouts to avoid a header is a way to be
// subtly wrong for no gain.

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

// WGL_NV_DX_interop / _interop2
typedef HANDLE(WINAPI* WglDXOpenDeviceFn)(void*);
typedef BOOL(WINAPI* WglDXCloseDeviceFn)(HANDLE);
typedef HANDLE(WINAPI* WglDXRegisterObjectFn)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* WglDXUnregisterObjectFn)(HANDLE, HANDLE);
typedef BOOL(WINAPI* WglDXObjectAccessFn)(HANDLE, GLint, HANDLE*);
typedef const char*(WINAPI* WglGetExtensionsStringArbFn)(HDC);
typedef const char*(WINAPI* WglGetExtensionsStringExtFn)();

typedef HRESULT(WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                             UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*,
                                             ID3D11DeviceContext**);

// Not in the GL 1.1 headers the Windows SDK ships.
constexpr GLenum kGlBgraExt = 0x80E1;
constexpr GLenum kWglAccessWriteDiscardNv = 0x00000002;

struct Gl {
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

// Warm up before measuring: the first frames after a mode change carry driver work
// that is not representative, and glReadPixels in particular is far slower the first
// time it runs.
constexpr int kWarmupFrames = 20;
constexpr int kMeasureFrames = 60;

// The game changes render size at every menu-to-sim transition -- 640x480 for the menu,
// then whatever RasterV2:Width/Height says for the sim. Measuring once meant measuring
// the menu, which is 1.2 MB a frame against the sim's 14, and the two routes scale
// differently: a readback crosses PCIe and an interop copy does not. So the probe
// re-measures whenever the drawable changes size, and reports each size separately.
constexpr int kMaxRuns = 4;

struct State {
    bool contextReady = false;  // GL entry points resolved and a context found
    bool dead = false;          // no GL in this process; never retry
    bool reported = false;      // already reported for the current size
    int  runs = 0;

    Gl    gl;
    HGLRC context = nullptr;
    int   width = 0;
    int   height = 0;
    int   frame = 0;

    // route 2 -- glReadPixels
    std::vector<unsigned char> readback;
    long long                  readTicks = 0;
    int                        readSamples = 0;
    unsigned                   readPixel = 0;

    // route 1 -- WGL_NV_DX_interop2
    bool                 interopAdvertised = false;
    bool                 interopWorking = false;
    HMODULE              d3d11Module = nullptr;
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* context3d = nullptr;
    ID3D11Texture2D*     texture = nullptr;
    ID3D11Texture2D*     staging = nullptr;
    HANDLE               shareHandle = nullptr;
    HANDLE               interopDevice = nullptr;
    HANDLE               interopObject = nullptr;
    GLuint               glTexture = 0;
    long long            interopTicks = 0;
    int                  interopSamples = 0;
    bool                 interopVerified = false;
    bool                 interopNeedsFlip = false;
    bool                 interopCompared = false;
};

State         g_state;
LARGE_INTEGER g_frequency{};

template <typename T>
void resolveGl(Gl& gl, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(gl.module, name));
}

template <typename T>
void resolveWglExtension(Gl& gl, const char* name, T& out) {
    out = reinterpret_cast<T>(gl.wglGetProcAddress(name));
}

bool openGl(Gl& gl) {
    // GetModuleHandle, not LoadLibrary: if opengl32 is not already in the process then
    // the D3D7 rasterizer is running and there is nothing here to probe. Loading it
    // ourselves would only manufacture a context-less failure.
    gl.module = GetModuleHandleA("opengl32.dll");
    if (!gl.module) return false;

    resolveGl(gl, "wglGetCurrentContext", gl.wglGetCurrentContext);
    resolveGl(gl, "wglGetCurrentDC", gl.wglGetCurrentDC);
    resolveGl(gl, "wglGetProcAddress", gl.wglGetProcAddress);
    resolveGl(gl, "glGetString", gl.getString);
    resolveGl(gl, "glGetError", gl.getError);
    resolveGl(gl, "glFinish", gl.finish);
    resolveGl(gl, "glReadBuffer", gl.readBuffer);
    resolveGl(gl, "glReadPixels", gl.readPixels);
    resolveGl(gl, "glGenTextures", gl.genTextures);
    resolveGl(gl, "glDeleteTextures", gl.deleteTextures);
    resolveGl(gl, "glBindTexture", gl.bindTexture);
    resolveGl(gl, "glCopyTexSubImage2D", gl.copyTexSubImage2D);
    resolveGl(gl, "glPixelStorei", gl.pixelStorei);

    return gl.wglGetCurrentContext && gl.wglGetCurrentDC && gl.wglGetProcAddress &&
           gl.getString && gl.getError && gl.finish && gl.readPixels &&
           gl.copyTexSubImage2D && gl.genTextures && gl.bindTexture && gl.readBuffer;
}

const char* wglExtensions(Gl& gl, HDC dc) {
    WglGetExtensionsStringArbFn arb = nullptr;
    resolveWglExtension(gl, "wglGetExtensionsStringARB", arb);
    if (arb) return arb(dc);

    WglGetExtensionsStringExtFn ext = nullptr;
    resolveWglExtension(gl, "wglGetExtensionsStringEXT", ext);
    if (ext) return ext();
    return nullptr;
}

// -- route 1 setup ----------------------------------------------------------------

bool createD3d11Device(State& state) {
    state.d3d11Module = LoadLibraryA("d3d11.dll");
    if (!state.d3d11Module) {
        logf("glprobe: d3d11.dll would not load (error %lu)", GetLastError());
        return false;
    }

    auto create = reinterpret_cast<D3D11CreateDeviceFn>(
        GetProcAddress(state.d3d11Module, "D3D11CreateDevice"));
    if (!create) {
        logf("glprobe: d3d11.dll does not export D3D11CreateDevice");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL       level = D3D_FEATURE_LEVEL_11_0;
    // Default adapter, which is right for a probe and wrong for the presenter: the real
    // one must use the adapter the OpenXR runtime names by LUID -- gplvr-xr prints it --
    // or a two-GPU laptop pays for a cross-adapter copy on every frame.
    const HRESULT created =
        create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
               D3D11_SDK_VERSION, &state.device, &level, &state.context3d);
    if (FAILED(created) || !state.device) {
        logf("glprobe: D3D11CreateDevice failed (hr=0x%08lX)", (unsigned long)created);
        return false;
    }
    logf("glprobe: D3D11 device created, feature level 0x%04X", (unsigned)level);
    return true;
}

bool createSharedTexture(State& state) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(state.width);
    desc.Height = static_cast<UINT>(state.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // The legacy SHARED flag rather than SHARED_NTHANDLE: NV_DX_interop is documented
    // against it, and BFVR's presentation protocol already carries a legacy handle as
    // one of its two transports, so this is a road with tyre marks on it.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT created = state.device->CreateTexture2D(&desc, nullptr, &state.texture);
    if (FAILED(created) || !state.texture) {
        logf("glprobe: CreateTexture2D %dx%d failed (hr=0x%08lX)", state.width,
             state.height, (unsigned long)created);
        return false;
    }

    IDXGIResource* resource = nullptr;
    if (SUCCEEDED(state.texture->QueryInterface(__uuidof(IDXGIResource),
                                                (void**)&resource))) {
        resource->GetSharedHandle(&state.shareHandle);
        resource->Release();
    }
    logf("glprobe: shared texture %dx%d BGRA, share handle %p", state.width,
         state.height, state.shareHandle);
    if (!state.shareHandle)
        logf("glprobe: WARNING -- no share handle, so this texture could not be opened "
             "by the x64 presenter even though the copy may work here");

    // A staging copy so the probe can look at the pixels rather than trust the return
    // codes. Every call in the interop path can succeed while the image is still blank.
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    created = state.device->CreateTexture2D(&desc, nullptr, &state.staging);
    if (FAILED(created)) {
        logf("glprobe: staging texture failed (hr=0x%08lX); the interop route can be "
             "timed but not verified", (unsigned long)created);
        state.staging = nullptr;
    }
    return true;
}

bool setUpInterop(State& state) {
    Gl& gl = state.gl;
    resolveWglExtension(gl, "wglDXOpenDeviceNV", gl.dxOpenDevice);
    resolveWglExtension(gl, "wglDXCloseDeviceNV", gl.dxCloseDevice);
    resolveWglExtension(gl, "wglDXRegisterObjectNV", gl.dxRegisterObject);
    resolveWglExtension(gl, "wglDXUnregisterObjectNV", gl.dxUnregisterObject);
    resolveWglExtension(gl, "wglDXLockObjectsNV", gl.dxLockObjects);
    resolveWglExtension(gl, "wglDXUnlockObjectsNV", gl.dxUnlockObjects);

    if (!gl.dxOpenDevice || !gl.dxRegisterObject || !gl.dxLockObjects ||
        !gl.dxUnlockObjects) {
        logf("glprobe: the WGL_NV_DX_interop entry points did not resolve");
        return false;
    }

    if (!createD3d11Device(state) || !createSharedTexture(state)) return false;

    state.interopDevice = gl.dxOpenDevice(state.device);
    if (!state.interopDevice) {
        logf("glprobe: wglDXOpenDeviceNV failed (GL error 0x%04X)", gl.getError());
        return false;
    }

    gl.genTextures(1, &state.glTexture);
    state.interopObject = gl.dxRegisterObject(state.interopDevice, state.texture,
                                              state.glTexture, GL_TEXTURE_2D,
                                              kWglAccessWriteDiscardNv);
    if (!state.interopObject) {
        logf("glprobe: wglDXRegisterObjectNV failed (GL error 0x%04X)", gl.getError());
        return false;
    }

    logf("glprobe: interop ready -- the D3D11 texture is GL texture %u",
         state.glTexture);
    return true;
}

// -- the two timed routes ---------------------------------------------------------

// One pixel, in the middle of the left eye's half of the buffer horizontally and a
// quarter of the way up it vertically.
//
// A quarter and not a half, deliberately. The flip check below compares row y against
// row height-1-y, and at the midpoint those are adjacent rows showing the same thing --
// the test would agree with itself whichever way up the image was. A quarter of the way
// up puts the two candidates in the lower and upper quarters of the frame, which in a
// cockpit view is road against sky.
int sampleX(const State& state) { return state.width / 4; }
int sampleY(const State& state) { return state.height / 4; }

size_t sampleOffset(const State& state) {
    return (static_cast<size_t>(sampleY(state)) * state.width + sampleX(state)) * 4;
}

long long timedReadPixels(State& state) {
    Gl& gl = state.gl;
    LARGE_INTEGER begin, end;

    QueryPerformanceCounter(&begin);
    gl.readBuffer(GL_BACK);
    if (gl.pixelStorei) gl.pixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.readPixels(0, 0, state.width, state.height, kGlBgraExt, GL_UNSIGNED_BYTE,
                  state.readback.data());
    QueryPerformanceCounter(&end);

    std::memcpy(&state.readPixel, state.readback.data() + sampleOffset(state), 4);
    return end.QuadPart - begin.QuadPart;
}

long long timedInteropCopy(State& state) {
    Gl& gl = state.gl;
    LARGE_INTEGER begin, end;

    QueryPerformanceCounter(&begin);
    if (!gl.dxLockObjects(state.interopDevice, 1, &state.interopObject)) return -1;

    gl.readBuffer(GL_BACK);
    gl.bindTexture(GL_TEXTURE_2D, state.glTexture);
    gl.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, state.width, state.height);
    gl.bindTexture(GL_TEXTURE_2D, 0);
    gl.dxUnlockObjects(state.interopDevice, 1, &state.interopObject);
    // glCopyTexSubImage2D only queues work. Without this the measurement would be the
    // cost of *asking*, which is near zero and would make the comparison against
    // glReadPixels -- which synchronises by definition -- meaningless.
    gl.finish();
    QueryPerformanceCounter(&end);

    const GLenum error = gl.getError();
    if (error) {
        logf("glprobe: the interop copy left GL error 0x%04X", error);
        return -1;
    }
    return end.QuadPart - begin.QuadPart;
}

// Did the pixels actually land in the D3D11 texture? Copy it to staging, map it, and
// compare one pixel with what glReadPixels saw in the same frame.
void verifyInterop(State& state) {
    if (!state.staging || !state.context3d || state.interopCompared) return;
    state.interopCompared = true;

    state.context3d->CopyResource(state.staging, state.texture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(state.context3d->Map(state.staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        logf("glprobe: could not map the staging texture; the interop route is timed "
             "but unverified");
        return;
    }

    // Read the same index and the mirrored one, then work out which convention the
    // copy used. Getting the *conclusion* right matters more than getting the two
    // values, and it is easy to state backwards -- so, carefully:
    //
    //   * glReadPixels fills its buffer bottom-up. readback[y] is the window row y
    //     counted from the BOTTOM.
    //   * Mapping a D3D11 texture gives rows top-down. rows[y] is the texture row y
    //     counted from the TOP.
    //
    // So a match at the *same* index means texture row 0 holds window row 0 counted
    // from the bottom -- the texture is bottom-up, and it needs flipping before it
    // reaches an OpenXR swapchain image, which is top-down like D3D. A match at the
    // mirrored index is the one that means no flip is needed.
    const int   x = sampleX(state);
    const int   y = sampleY(state);
    const auto* rows = static_cast<const unsigned char*>(mapped.pData);
    unsigned    d3dAtSameIndex = 0;
    unsigned    d3dAtMirroredIndex = 0;
    std::memcpy(&d3dAtSameIndex, rows + static_cast<size_t>(y) * mapped.RowPitch + x * 4,
                4);
    std::memcpy(&d3dAtMirroredIndex,
                rows + static_cast<size_t>(state.height - 1 - y) * mapped.RowPitch +
                    x * 4,
                4);
    state.context3d->Unmap(state.staging, 0);

    const bool bottomUp = d3dAtSameIndex == state.readPixel;
    const bool topDown = d3dAtMirroredIndex == state.readPixel;
    state.interopVerified = bottomUp || topDown;
    state.interopNeedsFlip = bottomUp;

    logf("glprobe: verification -- glReadPixels(bottom-up) 0x%08X, D3D row[y] 0x%08X, "
         "D3D row[h-1-y] 0x%08X",
         state.readPixel, d3dAtSameIndex, d3dAtMirroredIndex);
    if (!state.interopVerified)
        logf("glprobe: the pixels do NOT match, so the interop copy is not delivering "
             "this frame's image whatever its return codes say");
    else if (bottomUp)
        logf("glprobe: the image arrived, stored bottom-up -- the presenter MUST flip "
             "it, or sample it with v inverted, or OpenXR gets it upside down");
    else
        logf("glprobe: the image arrived, stored top-down -- no flip needed");
}

// -- one-time discovery -----------------------------------------------------------

// Once per process: resolve GL, find the context, print what we are talking to.
bool openContext(State& state) {
    QueryPerformanceFrequency(&g_frequency);

    if (!openGl(state.gl)) {
        state.dead = true;
        logf("glprobe: opengl32 is not loaded in this process -- the D3D7 rasterizer is "
             "selected, so there is no GL back buffer to capture. Set the rasterizer to "
             "roglv2 and try again.");
        return false;
    }

    Gl& gl = state.gl;
    state.context = gl.wglGetCurrentContext();
    const HDC dc = gl.wglGetCurrentDC();
    if (!state.context || !dc) {
        state.dead = true;
        logf("glprobe: no current GL context on the render thread (context %p, dc %p)",
             (void*)state.context, (void*)dc);
        return false;
    }

    logf("glprobe: GL vendor   %s", (const char*)gl.getString(GL_VENDOR));
    logf("glprobe: GL renderer %s", (const char*)gl.getString(GL_RENDERER));
    logf("glprobe: GL version  %s", (const char*)gl.getString(GL_VERSION));

    const char* extensions = wglExtensions(gl, dc);
    state.interopAdvertised =
        extensions && (std::strstr(extensions, "WGL_NV_DX_interop2") ||
                       std::strstr(extensions, "WGL_NV_DX_interop"));
    logf("glprobe: WGL_NV_DX_interop2 %s",
         !extensions               ? "unknown -- no WGL extension string"
         : state.interopAdvertised ? "advertised"
                                   : "NOT advertised; only the readback route is open");

    state.contextReady = true;
    return true;
}

bool drawableSize(State& state, int& width, int& height) {
    const HDC dc = state.gl.wglGetCurrentDC();
    if (!dc) return false;
    RECT client = {};
    if (!GetClientRect(WindowFromDC(dc), &client)) return false;
    width = client.right - client.left;
    height = client.bottom - client.top;
    return width > 0 && height > 0;
}

// Per size: allocate the readback buffer and rebuild the interop objects.
void beginSize(State& state, int width, int height) {
    state.width = width;
    state.height = height;
    state.frame = 0;
    state.reported = false;
    state.readTicks = 0;
    state.readSamples = 0;
    state.readPixel = 0;
    state.interopTicks = 0;
    state.interopSamples = 0;
    state.interopWorking = false;
    state.interopVerified = false;
    state.interopNeedsFlip = false;
    state.interopCompared = false;

    logf("glprobe: measuring at %dx%d", width, height);
    state.readback.resize(static_cast<size_t>(width) * height * 4);
    if (state.interopAdvertised) state.interopWorking = setUpInterop(state);
}

void report(State& state) {
    const double toMs = 1000.0 / g_frequency.QuadPart;
    const double readMs =
        state.readSamples ? state.readTicks * toMs / state.readSamples : 0.0;
    const double interopMs =
        state.interopSamples ? state.interopTicks * toMs / state.interopSamples : 0.0;
    const double megabytes = state.readback.size() / (1024.0 * 1024.0);

    logf("glprobe: -- %d frames at %dx%d, %.1f MB per frame --", state.readSamples,
         state.width, state.height, megabytes);
    logf("glprobe: glReadPixels   %6.2f ms  (sampled pixel 0x%08X)", readMs,
         state.readPixel);
    if (state.interopSamples)
        logf("glprobe: NV_DX_interop %6.2f ms  (with glFinish, so both figures include "
             "the GPU work)  verified=%s flip=%s",
             interopMs, state.interopVerified ? "yes" : "NO",
             state.interopNeedsFlip ? "needed" : "not needed");
    else
        logf("glprobe: NV_DX_interop did not run");

    // Phase 1 measured two eye renders at about 2.4 ms inside a 27.8 ms budget. State
    // what is left rather than leaving it to be worked out.
    const double chosen = state.interopSamples ? interopMs : readMs;
    logf("glprobe: 27.8 ms budget at 36 Hz, ~2.4 ms of it already spent on two eye "
         "renders, so a %.2f ms transport uses %.0f%% of the remaining %.1f ms",
         chosen, 100.0 * chosen / 25.4, 25.4);

    if (state.readPixel == 0)
        logf("glprobe: WARNING -- the sampled pixel is black, so these timings may be "
             "measuring an empty buffer. Run the probe while actually driving.");

    state.reported = true;
    ++state.runs;
}

// Everything that is sized, so a new drawable size can rebuild it. The D3D11 device
// itself is sized too here -- setUpInterop creates it -- which is wasteful but keeps
// teardown a single path, and this runs at most kMaxRuns times.
void releaseSized(State& state) {
    Gl& gl = state.gl;

    if (state.interopObject && gl.dxUnregisterObject)
        gl.dxUnregisterObject(state.interopDevice, state.interopObject);
    state.interopObject = nullptr;

    if (state.glTexture && gl.deleteTextures) gl.deleteTextures(1, &state.glTexture);
    state.glTexture = 0;

    if (state.interopDevice && gl.dxCloseDevice) gl.dxCloseDevice(state.interopDevice);
    state.interopDevice = nullptr;

    if (state.staging) state.staging->Release();
    state.staging = nullptr;
    if (state.texture) state.texture->Release();
    state.texture = nullptr;
    if (state.context3d) state.context3d->Release();
    state.context3d = nullptr;
    if (state.device) state.device->Release();
    state.device = nullptr;
    state.shareHandle = nullptr;
    state.interopWorking = false;

    state.readback.clear();
    state.readback.shrink_to_fit();
}

}  // namespace

void glProbeFrame() {
    State& state = g_state;
    if (state.dead || state.runs >= kMaxRuns) return;
    if (!state.contextReady && !openContext(state)) return;

    int width = 0;
    int height = 0;
    if (!drawableSize(state, width, height)) return;

    if (width != state.width || height != state.height) {
        if (state.width)
            logf("glprobe: drawable changed %dx%d -> %dx%d, measuring again",
                 state.width, state.height, width, height);
        releaseSized(state);
        beginSize(state, width, height);
    }

    if (state.reported) return;

    ++state.frame;
    if (state.frame <= kWarmupFrames) return;

    state.readTicks += timedReadPixels(state);
    ++state.readSamples;

    if (state.interopWorking) {
        const long long ticks = timedInteropCopy(state);
        if (ticks < 0) {
            state.interopWorking = false;
            logf("glprobe: the interop route failed mid-run and has been switched off");
        } else {
            state.interopTicks += ticks;
            ++state.interopSamples;
            // Verify on the first measured frame, while readPixel is this same frame's.
            verifyInterop(state);
        }
    }

    if (state.readSamples >= kMeasureFrames) {
        report(state);
        // Hand the GPU resources back but keep the size, so the probe stays quiet until
        // the drawable actually changes rather than immediately measuring again.
        releaseSized(state);
        if (state.runs >= kMaxRuns)
            logf("glprobe: %d measurements taken; the probe is done", state.runs);
    }
}

void glProbeRelease() { releaseSized(g_state); }

}  // namespace gplvr
