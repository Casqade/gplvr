#include "presenter/Producer.h"

#include "RenderTarget.h"

#include "CursorOverlay.h"
#include "FrameBudget.h"
#include "GlInterop.h"
#include "Hooks.h"
#include "Log.h"
#include "Pribluda.h"
#include "Stereo.h"
#include "VrCamera.h"

#include <d3d11.h>
#include <dxgi.h>

#include <new>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <cstring>
#include <cwchar>

namespace gplvr {
namespace {

typedef HRESULT(WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                             UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*,
                                             ID3D11DeviceContext**);
typedef HRESULT(WINAPI* CreateDXGIFactory1Fn)(REFIID, void**);

enum class Stage {
    Off,
    WaitingForRequirements,
    Publishing,
    Failed,
};

struct State {
    Stage stage = Stage::Off;

    HANDLE        mapping = nullptr;
    ControlBlock* block = nullptr;
    HANDLE        presenterProcess = nullptr;

    GlFunctions gl;
    HANDLE      interopDevice = nullptr;
    HGLRC       glContext = nullptr;

    HMODULE              d3d11Module = nullptr;
    HMODULE              dxgiModule = nullptr;
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    ID3D11Texture2D* textures[kFrameSlots] = {};
    GlSharedTexture  shared[kFrameSlots] = {};

    // Pribluda's panel, two shared slots like the frame. Separate from the frame textures:
    // its size follows the panel rather than the render target. pribluda draws into
    // hudTexture, a plain GL texture of the same size that needs no interop lock, and the
    // image is copied into the next shared slot inside the frame transfer's lock.
    ID3D11Texture2D* hudTextures[kFrameSlots] = {};
    GlSharedTexture  hudShared[kFrameSlots] = {};
    int              hudWidth = 0;
    int              hudHeight = 0;
    GLuint           hudTexture = 0;
    GLuint           hudFramebuffer = 0;
    GLint            hudPreviousFramebuffer = 0;
    bool             hudBound = false;
    bool             hudPending = false;  // hudTexture holds an image not yet copied
    bool             hudReportedFailure = false;
    LONGLONG         hudPublishedQpc = 0;  // when the last panel image was published
    LONGLONG         hudDrawnQpc = 0;      // when pribluda last drew into hudTexture
    LONGLONG         qpcFrequency = 0;

    // The panel on the monitor. The views this frame's eyes were rendered from, kept for the monitor draw
    // that follows the transfer; whether that draw took over pribluda's monitor HUD this
    // frame; and the last state logged.
    SharedView  monitorViews[2] = {};
    bool        monitorViewsValid = false;
    bool        monitorHudTaken = false;
    const char* monitorHudReported = "";
    int         monitorHudReports = 32;
    bool        monitorHudGlError = false;

    int  width = 0;
    int  height = 0;
    int  frameSequence = 0;
    bool loggedFirstFrame = false;
    bool loggedFirstViews = false;

    // Pose diagnostics, logged every two seconds. viewLag counts, per published frame, how
    // many view sets the presenter had published after the one the eyes were rendered from
    // (0, 1, 2, 3+). slotOverwrites counts frames written into the slot holding the frame
    // the presenter drew last, before it had latched anything newer.
    int32_t  acquiredViewSequence = 0;
    int      viewLag[4] = {};
    int      slotOverwrites = 0;
    LONGLONG poseReportQpc = 0;
};

State g_state;

void fail(const char* why) {
    logf("vr: %s -- the presenter channel is off; the game continues as a flat stereo "
         "mod", why);
    g_state.stage = Stage::Failed;
}

// -- setup -----------------------------------------------------------------------

bool createControlBlock(State& state, wchar_t (&instance)[32]) {
    _snwprintf_s(instance, _TRUNCATE, L"%lu", GetCurrentProcessId());

    wchar_t name[128] = {};
    makeMappingName(name, instance);

    state.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       sizeof(ControlBlock), name);
    if (!state.mapping) {
        logf("vr: CreateFileMapping failed (error %lu)", GetLastError());
        return false;
    }

    state.block = static_cast<ControlBlock*>(
        MapViewOfFile(state.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock)));
    if (!state.block) {
        logf("vr: MapViewOfFile failed (error %lu)", GetLastError());
        return false;
    }

    // Placement-new so the member initialisers run; the mapping arrives zeroed but the
    // defaults in the struct are the documentation and should be what is actually there.
    new (state.block) ControlBlock();
    state.block->magic = kControlMagic;
    state.block->version = kControlVersion;
    state.block->byteSize = sizeof(ControlBlock);
    state.block->producerProcessId = GetCurrentProcessId();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state.block->producerState),
                        static_cast<LONG>(ProcessState::Starting));
    return true;
}

bool launchPresenter(State& state, const wchar_t* instance) {
    wchar_t directory[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, directory, MAX_PATH);
    if (wchar_t* slash = wcsrchr(directory, L'\\')) slash[1] = L'\0';

    wchar_t exePath[MAX_PATH] = {};
    _snwprintf_s(exePath, _TRUNCATE, L"%sgplvr-xr.exe", directory);
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        logf("vr: %S is not there -- build the x64 configuration and copy gplvr-xr.exe "
             "and its runtime folder next to the game", exePath);
        return false;
    }

    // MAX_PATH + 192, not + 64. A full-length exe path plus --present, --eye-scale and
    // all three optional flags overruns the old size, and _TRUNCATE would have cut the
    // LAST flag off silently -- so an option would simply not arrive, with nothing in
    // either log to say why. Widened once rather than counted every time a flag is added.
    wchar_t commandLine[MAX_PATH + 192] = {};
    _snwprintf_s(commandLine, _TRUNCATE,
                 L"\"%s\" --present %s --render-scale %.3f",
                 exePath, instance, g_options.vrRenderScale);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(exePath, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, directory, &startup, &process)) {
        logf("vr: could not start %S (error %lu)", exePath, GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    state.presenterProcess = process.hProcess;
    // The arguments, not just the pid. Options reach the presenter ONLY through this
    // line, so "I turned it on and nothing happened" is answered here or nowhere.
    logf("vr: presenter started, pid %lu, args%S -- it writes its own gplvr-xr.log",
         process.dwProcessId,
         wcsstr(commandLine, L" --present") ? wcsstr(commandLine, L" --present")
                                            : L" (command line truncated)");
    return true;
}

IDXGIAdapter* findAdapter(State& state, uint32_t luidLow, int32_t luidHigh) {
    state.dxgiModule = LoadLibraryA("dxgi.dll");
    if (!state.dxgiModule) {
        logf("vr: dxgi.dll would not load (error %lu)", GetLastError());
        return nullptr;
    }
    auto createFactory = reinterpret_cast<CreateDXGIFactory1Fn>(
        GetProcAddress(state.dxgiModule, "CreateDXGIFactory1"));
    if (!createFactory) {
        logf("vr: dxgi.dll does not export CreateDXGIFactory1");
        return nullptr;
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), (void**)&factory))) {
        logf("vr: CreateDXGIFactory1 failed");
        return nullptr;
    }

    IDXGIAdapter1* match = nullptr;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (!match && desc.AdapterLuid.LowPart == luidLow &&
            desc.AdapterLuid.HighPart == luidHigh) {
            logf("vr: using adapter %u %S, the one the OpenXR runtime asked for", index,
                 desc.Description);
            match = adapter;
        } else {
            adapter->Release();
        }
    }
    factory->Release();

    if (!match)
        logf("vr: no adapter matches the runtime's LUID %08lX:%08lX -- a texture shared "
             "from the wrong GPU cannot be opened at all",
             (unsigned long)luidHigh, (unsigned long)luidLow);
    return match;
}

bool createDevice(State& state) {
    if (state.device) return true;

    IDXGIAdapter* adapter =
        findAdapter(state, state.block->adapterLuidLow, state.block->adapterLuidHigh);
    if (!adapter) return false;

    state.d3d11Module = LoadLibraryA("d3d11.dll");
    auto create =
        state.d3d11Module
            ? reinterpret_cast<D3D11CreateDeviceFn>(
                  GetProcAddress(state.d3d11Module, "D3D11CreateDevice"))
            : nullptr;
    if (!create) {
        logf("vr: d3d11.dll or D3D11CreateDevice is unavailable (error %lu)",
             GetLastError());
        adapter->Release();
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL       level = D3D_FEATURE_LEVEL_11_0;
    // DRIVER_TYPE_UNKNOWN is required, not preferred, when an adapter is passed.
    const HRESULT created =
        create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
               D3D11_SDK_VERSION, &state.device, &level, &state.context);
    adapter->Release();
    if (FAILED(created) || !state.device) {
        logf("vr: D3D11CreateDevice failed (hr=0x%08lX)", (unsigned long)created);
        return false;
    }

    state.interopDevice = glInteropOpenDevice(state.gl, state.device);
    return state.interopDevice != nullptr;
}

void releaseHudTextures(State& state) {
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        glInteropUnregister(state.gl, state.interopDevice, state.hudShared[slot]);
        if (state.hudTextures[slot]) state.hudTextures[slot]->Release();
        state.hudTextures[slot] = nullptr;
    }
    if (state.hudFramebuffer && state.gl.deleteFramebuffers)
        state.gl.deleteFramebuffers(1, &state.hudFramebuffer);
    if (state.hudTexture && state.gl.deleteTextures)
        state.gl.deleteTextures(1, &state.hudTexture);
    state.hudFramebuffer = 0;
    state.hudTexture = 0;
    state.hudWidth = 0;
    state.hudHeight = 0;
    state.hudPending = false;
}

void releaseTextures(State& state) {
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        glInteropUnregister(state.gl, state.interopDevice, state.shared[slot]);
        if (state.textures[slot]) state.textures[slot]->Release();
        state.textures[slot] = nullptr;
    }
    releaseHudTextures(state);
}

bool createTextures(State& state, int width, int height) {
    releaseTextures(state);
    state.width = width;
    state.height = height;

    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        // Legacy SHARED, not SHARED_NTHANDLE: NV_DX_interop is documented against it,
        // and a legacy handle is valid in any process on the same adapter, which is what
        // carries it across the 32-to-64-bit boundary without duplication.
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        if (FAILED(state.device->CreateTexture2D(&desc, nullptr,
                                                 &state.textures[slot]))) {
            logf("vr: CreateTexture2D %dx%d failed for slot %u", width, height, slot);
            releaseTextures(state);
            return false;
        }
        if (!glInteropRegister(state.gl, state.interopDevice, state.textures[slot],
                               state.shared[slot])) {
            releaseTextures(state);
            return false;
        }
        storeSharedHandle(state.block->textures[slot], state.shared[slot].shareHandle);
    }

    state.block->textureWidth = static_cast<uint32_t>(width);
    state.block->textureHeight = static_cast<uint32_t>(height);
    state.block->textureFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    state.block->textureBottomUp = 1;

    // Publish the descriptions before the generation that advertises them, and the
    // generation before the state. The presenter reads them in the opposite order.
    InterlockedIncrement(
        reinterpret_cast<volatile LONG*>(&state.block->textureGeneration));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state.block->producerState),
                        static_cast<LONG>(ProcessState::TexturesReady));

    logf("vr: published %u shared textures at %dx%d, generation %d", kFrameSlots, width,
         height, state.block->textureGeneration);
    return true;
}

// Pribluda's panel is 2048 texels wide whatever the monitor is: at the default 0.6 m that
// is over three texels a millimetre, finer than a headset resolves at arm's length.
constexpr int kHudTextureWidth = 2048;

// How long the panel layer stays published after the last panel image. Long enough to ride
// over a frame that skips the draw, short enough that leaving a race clears it at once.
constexpr double kHudGraceSeconds = 0.25;

bool createHudTextures(State& state, int width, int height) {
    releaseHudTextures(state);

    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        if (FAILED(state.device->CreateTexture2D(&desc, nullptr,
                                                 &state.hudTextures[slot]))) {
            logf("vr: CreateTexture2D %dx%d failed for HUD panel slot %u", width, height,
                 slot);
            state.hudTextures[slot] = nullptr;
            releaseHudTextures(state);
            return false;
        }
        if (!glInteropRegister(state.gl, state.interopDevice, state.hudTextures[slot],
                               state.hudShared[slot])) {
            releaseHudTextures(state);
            return false;
        }
        storeSharedHandle(state.block->hudTextures[slot], state.hudShared[slot].shareHandle);
    }

    // The texture pribluda draws into. Plain GL, so the draw takes no interop lock; its image
    // reaches a shared slot in the frame transfer's lock.
    GlFunctions& gl = state.gl;
    GLint previousTexture = 0;
    GLint previousFramebuffer = 0;
    gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    gl.getIntegerv(kGlFramebufferBinding, &previousFramebuffer);
    gl.genTextures(1, &state.hudTexture);
    gl.bindTexture(GL_TEXTURE_2D, state.hudTexture);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                  nullptr);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    gl.genFramebuffers(1, &state.hudFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, state.hudFramebuffer);
    gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                            state.hudTexture, 0);
    GLenum status = gl.checkFramebufferStatus(kGlFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previousFramebuffer));
    if (status != kGlFramebufferComplete) {
        logf("vr: the HUD panel's own %dx%d framebuffer is incomplete (0x%04X) -- no panel",
             width, height, status);
        releaseHudTextures(state);
        return false;
    }

    state.hudWidth = width;
    state.hudHeight = height;
    state.block->hudTextureWidth = static_cast<uint32_t>(width);
    state.block->hudTextureHeight = static_cast<uint32_t>(height);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state.block->hudGeneration));
    logf("vr: HUD panel textures %dx%d shared, generation %d; pribluda draws into GL texture "
         "%u, copied into the shared slot under the frame transfer's lock",
         width, height, state.block->hudGeneration, state.hudTexture);
    return true;
}

// --- the one-shot field-of-view probe -------------------------------------------

// 32-bit BGRA, bottom-up -- which is the order glReadPixels already returns, so the
// rows go out untouched. `xOffset` picks one half out of the side-by-side buffer.
bool writeBmp32(const char* path, const unsigned char* pixels, int sourceWidth,
                int width, int height, int xOffset) {
    FILE* file = fopen(path, "wb");
    if (!file) return false;

    const DWORD imageBytes = static_cast<DWORD>(width) * height * 4;
    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42;  // 'BM'
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;

    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;  // positive: bottom-up, matching the readback
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = imageBytes;

    bool ok = fwrite(&fileHeader, sizeof(fileHeader), 1, file) == 1 &&
              fwrite(&info, sizeof(info), 1, file) == 1;
    for (int y = 0; ok && y < height; ++y) {
        const unsigned char* row =
            pixels + (static_cast<size_t>(y) * sourceWidth + xOffset) * 4;
        ok = fwrite(row, 4, width, file) == static_cast<size_t>(width);
    }
    fclose(file);
    return ok;
}

// -- the headset panel over the monitor picture ----------------------------------------

void quatRotate(float qx, float qy, float qz, float qw, const float v[3], float out[3]) {
    float tx = 2.0f * (qy * v[2] - qz * v[1]);
    float ty = 2.0f * (qz * v[0] - qx * v[2]);
    float tz = 2.0f * (qx * v[1] - qy * v[0]);
    out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

// The panel's corners in its layer's space -- VIEW for the head, the seat space for the
// seat -- placed exactly as gplvr-xr.exe places the quad: centre at (right, up, -ahead),
// turned toward the origin by a yaw about +y after a pitch about +x. Bottom-left,
// bottom-right, top-right, top-left, which are texture (0,0) (1,0) (1,1) (0,1): GL drew the
// panel bottom-up.
void panelCorners(const HudPlacement& placement, float aspect, float corners[4][3]) {
    float right = placement.right;
    float up = -placement.down;
    float ahead = placement.ahead;
    float level = std::sqrt(right * right + ahead * ahead);
    float yaw = std::atan2(-right, ahead);
    float pitch = std::atan2(up, level);
    float cy = std::cos(yaw * 0.5f);
    float sy = std::sin(yaw * 0.5f);
    float cp = std::cos(pitch * 0.5f);
    float sp = std::sin(pitch * 0.5f);
    float halfW = placement.width * 0.5f;
    float halfH = halfW * aspect;
    static const float kSigns[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    for (int i = 0; i < 4; ++i) {
        float local[3] = {kSigns[i][0] * halfW, kSigns[i][1] * halfH, 0.0f};
        float turned[3];
        quatRotate(cy * sp, sy * cp, -sy * sp, cy * cp, local, turned);
        corners[i][0] = right + turned[0];
        corners[i][1] = up + turned[1];
        corners[i][2] = -ahead + turned[2];
    }
}

constexpr float kMonitorHudNear = 0.02f;  // metres; a corner closer than this is not drawn

// One eye's picture: its corners projected through the pose and fov that eye was rendered
// with, then drawn as a textured quad with a homogeneous w per vertex, so the texture is
// interpolated perspective-correctly exactly as the eye camera would have drawn it. False
// when the quad is behind the eye, off the picture, or the eye is not in the picture.
bool drawPanelInEye(State& state, const WindowPicture& picture, int eye, bool seat,
                    const float corners[4][3]) {
    GlFunctions&      gl = state.gl;
    const SharedView& view = state.monitorViews[eye];
    int rx = static_cast<int>(state.block->eyeRect[eye][0]);
    int ry = static_cast<int>(state.block->eyeRect[eye][1]);
    int rw = static_cast<int>(state.block->eyeRect[eye][2]);
    int rh = static_cast<int>(state.block->eyeRect[eye][3]);
    if (rw <= 0 || rh <= 0 || picture.srcWidth <= 0) return false;
    if (rx < picture.srcX || rx + rw > picture.srcX + picture.srcWidth) return false;

    // The eye's rect in window pixels. eyeRect counts rows from the top of the frame; GL and
    // the blit count them from the bottom.
    float scale = static_cast<float>(picture.dstWidth) / static_cast<float>(picture.srcWidth);
    float wx = picture.dstX + (rx - picture.srcX) * scale;
    float wy = picture.dstY + (picture.srcHeight - ry - rh) * scale;
    float ww = rw * scale;
    float wh = rh * scale;

    float tanL = std::tan(view.fov.angleLeft);
    float tanR = std::tan(view.fov.angleRight);
    float tanU = std::tan(view.fov.angleUp);
    float tanD = std::tan(view.fov.angleDown);
    if (tanR - tanL <= 0.0f || tanU - tanD <= 0.0f) return false;

    const SharedPose& pose = view.pose;
    const SharedPose& seatPose = state.block->seatPose;
    // VIEW space is the head: between the two eyes, turned as they are.
    float head[3] = {
        (state.monitorViews[0].pose.positionX + state.monitorViews[1].pose.positionX) * 0.5f,
        (state.monitorViews[0].pose.positionY + state.monitorViews[1].pose.positionY) * 0.5f,
        (state.monitorViews[0].pose.positionZ + state.monitorViews[1].pose.positionZ) * 0.5f};

    float window[4][3];
    int   left = 0, rightOut = 0, below = 0, above = 0;
    for (int i = 0; i < 4; ++i) {
        float local[3];
        if (seat) {
            quatRotate(seatPose.orientationX, seatPose.orientationY, seatPose.orientationZ,
                       seatPose.orientationW, corners[i], local);
            local[0] += seatPose.positionX;
            local[1] += seatPose.positionY;
            local[2] += seatPose.positionZ;
        } else {
            quatRotate(pose.orientationX, pose.orientationY, pose.orientationZ,
                       pose.orientationW, corners[i], local);
            local[0] += head[0];
            local[1] += head[1];
            local[2] += head[2];
        }
        float relative[3] = {local[0] - pose.positionX, local[1] - pose.positionY,
                             local[2] - pose.positionZ};
        float inEye[3];
        quatRotate(-pose.orientationX, -pose.orientationY, -pose.orientationZ,
                   pose.orientationW, relative, inEye);
        float depth = -inEye[2];
        if (depth < kMonitorHudNear) return false;
        float u = (inEye[0] / depth - tanL) / (tanR - tanL);
        float v = (inEye[1] / depth - tanD) / (tanU - tanD);
        if (u < 0.0f) ++left;
        if (u > 1.0f) ++rightOut;
        if (v < 0.0f) ++below;
        if (v > 1.0f) ++above;
        window[i][0] = wx + u * ww;
        window[i][1] = wy + v * wh;
        window[i][2] = depth;
    }
    if (left == 4 || rightOut == 4 || below == 4 || above == 4) return false;

    int sx = static_cast<int>(std::floor(wx));
    int sy = static_cast<int>(std::floor(wy));
    gl.scissor(sx, sy, static_cast<int>(std::ceil(wx + ww)) - sx,
               static_cast<int>(std::ceil(wy + wh)) - sy);

    static const GLfloat kTexture[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    gl.begin(GL_QUADS);
    for (int i = 0; i < 4; ++i) {
        float w = window[i][2];
        gl.texCoord2f(kTexture[i][0], kTexture[i][1]);
        gl.vertex4f(window[i][0] * w, window[i][1] * w, 0.0f, w);
    }
    gl.end();
    return true;
}

// Logged when the answer changes, capped: entering and leaving races flips it.
void noteMonitorHud(State& state, const char* reason, int monitorEye) {
    if (reason == state.monitorHudReported || state.monitorHudReports <= 0) return;
    --state.monitorHudReports;
    state.monitorHudReported = reason;
    if (!reason) {
        logf("vr: monitor HUD engaged -- the headset panel (%s) is drawn over %s of the "
             "monitor picture and pribluda's own monitor HUD is skipped",
             g_options.vrHudLayer == 1 ? "following the head" : "fixed to the seat",
             monitorEye == 1 ? "each eye" : (monitorEye == 2 ? "the left eye" : "the right eye"));
    } else {
        logf("vr: monitor HUD not drawn -- %s; pribluda draws its own monitor HUD", reason);
    }
}

// After the eye blit, into the window. Every piece of state touched goes back, the matrix
// mode included: roglv2 relies on GL_PROJECTION being current.
void drawMonitorHud(State& state, int monitorEye) {
    GlFunctions& gl = state.gl;

    WindowPicture picture;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    bool live = state.hudTexture && state.hudDrawnQpc && state.qpcFrequency &&
                now.QuadPart - state.hudDrawnQpc <
                    static_cast<LONGLONG>(state.qpcFrequency * kHudGraceSeconds);
    int  layer = g_options.vrHudLayer;

    const char* reason = nullptr;
    if (monitorEye == 0)
        reason = "vrMonitorEye = 0 puts no picture in the window";
    else if (!pribludaMonitorDrawRedirected())
        reason = "pribluda's monitor draw is not redirected (see the pribluda: lines)";
    else if (!live)
        reason = "no HUD panel drawn in the last 0.25 s (not in a race, or vrHudLayer 0)";
    else if (!renderTargetWindowPicture(picture))
        reason = "the window shows no off-screen picture this frame";
    else if (!state.monitorViewsValid)
        reason = "this frame has no tracked stereo views to place the panel with";
    else if (layer != 1 && layer != 2)
        reason = "vrHudLayer is not 1 or 2";
    else if (layer == 2 && !state.block->seatGeneration)
        reason = "no seat has been published yet";
    else if (!gl.pushAttrib || !gl.popAttrib || !gl.pushMatrix || !gl.popMatrix ||
             !gl.matrixMode || !gl.loadIdentity || !gl.ortho || !gl.viewport || !gl.enable ||
             !gl.disable || !gl.scissor || !gl.bindTexture || !gl.texEnvi || !gl.blendFunc ||
             !gl.color4f || !gl.begin || !gl.end || !gl.texCoord2f || !gl.vertex4f ||
             !gl.polygonMode || !gl.colorMask || !gl.getIntegerv || !gl.bindFramebuffer)
        reason = "GL entry points for a textured quad are missing";
    noteMonitorHud(state, reason, monitorEye);
    if (reason) return;

    float corners[4][3];
    panelCorners(layer == 1 ? g_options.vrHudHead : g_options.vrHudSeat,
                 static_cast<float>(state.hudHeight) / static_cast<float>(state.hudWidth),
                 corners);

    GLint previousDraw = 0;
    GLint savedMode = GL_PROJECTION;
    gl.getIntegerv(kGlFramebufferBinding, &previousDraw);
    gl.getIntegerv(GL_MATRIX_MODE, &savedMode);
    if (previousDraw != 0) gl.bindFramebuffer(kGlDrawFramebuffer, 0);

    gl.pushAttrib(static_cast<GLbitfield>(
        GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
        GL_POLYGON_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT | GL_VIEWPORT_BIT | GL_TRANSFORM_BIT));
    GLint savedTexture = 0;
    gl.getIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture);

    gl.viewport(0, 0, picture.windowWidth, picture.windowHeight);
    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    gl.ortho(0.0, static_cast<double>(picture.windowWidth), 0.0,
             static_cast<double>(picture.windowHeight), -1.0, 1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_STENCIL_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_COLOR_LOGIC_OP);
    gl.disable(GL_TEXTURE_1D);
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.enable(GL_SCISSOR_TEST);
    gl.enable(GL_TEXTURE_2D);
    gl.bindTexture(GL_TEXTURE_2D, state.hudTexture);
    gl.texEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    gl.color4f(1.0f, 1.0f, 1.0f, 1.0f);
    // The panel is premultiplied: text opaque, pribluda's backdrops black at their coverage.
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    bool seat = layer == 2;
    if (monitorEye == 1 || monitorEye == 2) drawPanelInEye(state, picture, 0, seat, corners);
    if (monitorEye == 1 || monitorEye == 3) drawPanelInEye(state, picture, 1, seat, corners);

    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedTexture));
    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMode));
    if (previousDraw != 0)
        gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(previousDraw));

    // A quad entirely off the picture still counts: pribluda's monitor HUD must not pop in
    // whenever the panel is out of view.
    state.monitorHudTaken = true;

    if (gl.getError) {
        GLenum error = gl.getError();
        if (error != GL_NO_ERROR && !state.monitorHudGlError) {
            state.monitorHudGlError = true;
            logf("vr: monitor HUD -- GL error 0x%04X after drawing the panel over the monitor "
                 "(logged once)",
                 error);
        }
    }
}

}  // namespace

bool presenterStart() {
    State& state = g_state;
    if (state.stage != Stage::Off) return state.stage == Stage::Publishing ||
                                          state.stage == Stage::WaitingForRequirements;

    if (!glResolve(state.gl)) {
        fail("opengl32 is not in this process, so there is no back buffer to share");
        return false;
    }
    const HDC dc = state.gl.wglGetCurrentDC();
    if (!dc || !state.gl.wglGetCurrentContext()) {
        fail("no current GL context on the render thread");
        return false;
    }
    if (!glResolveInterop(state.gl, dc)) {
        fail("WGL_NV_DX_interop is unavailable, and it is the only transport built");
        return false;
    }
    // Not a failure if it does not resolve: the frame then goes into the window's own
    // back buffer, exactly as it did before there was an off-screen path at all.
    renderTargetResolve(state.gl);

    wchar_t instance[32] = {};
    if (!createControlBlock(state, instance)) {
        fail("the shared control block could not be created");
        return false;
    }
    if (!launchPresenter(state, instance)) {
        fail("the presenter would not start");
        return false;
    }

    state.stage = Stage::WaitingForRequirements;
    logf("vr: waiting for the presenter to bring the runtime up and name its adapter");
    return true;
}

void presenterFrame(bool sideBySide, bool stretchToFill) {
    State& state = g_state;
    if (state.stage == Stage::Off || state.stage == Stage::Failed) return;

    ControlBlock* block = state.block;
    if (!block) return;
    state.monitorViewsValid = false;

    // The presenter dying is a normal outcome -- no headset, SteamVR refused, the user
    // closed it -- and must not take the game with it.
    if (state.presenterProcess &&
        WaitForSingleObject(state.presenterProcess, 0) == WAIT_OBJECT_0) {
        logf("vr: the presenter exited (error code %d in the block); going quiet",
             block->presenterError);
        presenterStop();
        return;
    }

    const ProcessState presenter =
        static_cast<ProcessState>(block->presenterState);

    if (state.stage == Stage::WaitingForRequirements) {
        if (presenter == ProcessState::Failed) {
            logf("vr: the presenter reported failure (%d) -- see gplvr-xr.log",
                 block->presenterError);
            presenterStop();
            return;
        }
        if (presenter < ProcessState::RequirementsReady) return;

        logf("vr: presenter is ready -- adapter %08lX:%08lX, wants %ux%u per eye",
             (unsigned long)block->adapterLuidHigh, (unsigned long)block->adapterLuidLow,
             block->targetEyeWidth, block->targetEyeHeight);
        if (!createDevice(state)) {
            fail("could not create a D3D11 device on the runtime's adapter");
            return;
        }
        state.stage = Stage::Publishing;
    }

    // roglv2 recreates its GL context across a mode change, and our texture names and
    // interop registrations belong to the old one. Seen once as a lone GL_INVALID_VALUE
    // at the menu-to-sim transition, which the resize that followed happened to paper
    // over -- by luck, and not if a context is ever remade at the same size.
    const HGLRC currentContext = state.gl.wglGetCurrentContext();

    // No context at all means the game is on its way out -- roglv2 unbinds before it
    // tears the window down. Drop the textures and wait rather than calling that a
    // failure: the previous version logged "the presenter channel is off" during every
    // clean shutdown, which reads as a fault and is not one.
    if (!currentContext) {
        if (state.glContext) {
            logf("vr: the GL context has gone away; releasing until one comes back");
            releaseTextures(state);
            // The cursor texture belongs to that context too, and it is not in
            // releaseTextures because it is not one of the shared frame textures.
            cursorOverlayReleaseGl();
            state.glContext = nullptr;
            state.width = 0;
            state.height = 0;
        }
        return;
    }

    if (currentContext != state.glContext) {
        if (state.glContext)
            logf("vr: the GL context changed (%p -> %p), rebuilding the shared textures",
                 (void*)state.glContext, (void*)currentContext);
        state.glContext = currentContext;
        releaseTextures(state);
        cursorOverlayReleaseGl();
        glInteropCloseDevice(state.gl, state.interopDevice);
        state.interopDevice = glInteropOpenDevice(state.gl, state.device);
        if (!state.interopDevice) {
            fail("the interop device could not be reopened for the new GL context");
            return;
        }
        state.width = 0;
        state.height = 0;
    }

    // What the game should render into, in pixels, both eyes side by side. The
    // presenter publishes the size it actually created its swapchains at -- scaled by
    // vrRenderScale and clamped to the runtime's maximum -- so there is one number and
    // not two that have to agree.
    if (g_options.vrOffscreen && block->targetEyeWidth && block->targetEyeHeight) {
        renderTargetRequestSize(static_cast<int>(block->targetEyeWidth) * 2,
                                static_cast<int>(block->targetEyeHeight));
    } else {
        renderTargetRequestSize(0, 0);
    }

    int width = 0;
    int height = 0;
    // The render target when there is one, the window when there is not. Asked in that
    // order rather than switched on the option, so a target that failed to build is
    // indistinguishable from the option being off.
    if (!renderTargetSize(width, height) && !glDrawableSize(state.gl, width, height))
        return;

    if (width != state.width || height != state.height) {
        if (state.width)
            logf("vr: render size changed %dx%d -> %dx%d, recreating shared textures",
                 state.width, state.height, width, height);
        if (!createTextures(state, width, height)) {
            fail("the shared textures could not be created");
            return;
        }
    }

    block->sideBySide = sideBySide ? 1 : 0;
    block->monoScale = g_options.vrMenuScale;
    block->gamma = g_options.vrGamma;
    const HudPlacement& placement =
        g_options.vrHudLayer == 1 ? g_options.vrHudHead : g_options.vrHudSeat;
    block->hudRight = placement.right;
    block->hudUp = -placement.down;
    block->hudAhead = placement.ahead;
    block->hudWidth = placement.width;
    block->eyeHalfWidth = static_cast<uint32_t>(stereoEyeWidth());

    // Where each eye actually landed. Published every frame rather than at setup,
    // because the eye targets rebuild themselves whenever the game changes render size
    // and a stale rect would sample the wrong pixels rather than fail visibly.
    for (int eye = 0; eye < 2; ++eye) {
        int rect[4] = {};
        const bool have = sideBySide && stereoEyeRect(eye, rect);
        for (int i = 0; i < 4; ++i)
            block->eyeRect[eye][i] = have ? static_cast<uint32_t>(rect[i]) : 0u;
    }
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->stretchToFill),
                        stretchToFill ? 1 : 0);

    // The poses this frame was drawn from, echoed back untouched. Written before the
    // frame sequence that advertises the pixels, so the presenter never pairs a new
    // image with the previous frame's pose.
    // The slot must be the one the published sequence names, not the one before it:
    // the presenter reads slot (frameSequence % kFrameSlots), so the frame we are about
    // to write is numbered frameSequence + 1 and belongs in that slot.
    const uint32_t slot = static_cast<uint32_t>(state.frameSequence + 1) % kFrameSlots;

    // The poses go in the SAME slot as the pixels they belong to. Version 1 kept one
    // shared copy, written before frameSequence moved, so a consumer could latch
    // sequence N, have these overwritten with N+1's poses underneath it, and still pass
    // its consistency check -- because that check can only see the counter move. It
    // then drew frame N's pixels with frame N+1's poses: one frame submitted with the
    // wrong pose, then correct again. That is the blink.
    SharedView rendered[2] = {};
    const bool haveRendered = vrLastRenderedViews(rendered);
    if (haveRendered) {
        block->renderedViews[slot][0] = rendered[0];
        block->renderedViews[slot][1] = rendered[1];
    }
    block->renderedViewsValid[slot] = haveRendered ? 1 : 0;

    // The monitor draw after the transfer places the panel in the picture these views
    // rendered. Only a tracked side-by-side frame drawn at the runtime's frustum maps an eye's
    // rect exactly onto its fov.
    if (haveRendered && sideBySide && stretchToFill) {
        state.monitorViews[0] = rendered[0];
        state.monitorViews[1] = rendered[1];
        state.monitorViewsValid = true;
    }

    // Timed, because it is the one per-frame cost with no instrument on it and it runs
    // once per PUBLISHED frame -- so three to six times a tick, each ending in a glFinish
    // that stalls until the GPU drains. At 76.8 MB that is not obviously cheap, and the
    // frame budget cannot be reasoned about while one of its terms is unknown.
    if (state.frameSequence > 0 && block->consumedSequence == state.frameSequence - 1)
        ++state.slotOverwrites;

    // The span switch comes before the copy function, so its GPU mark is never issued between
    // that function's error checks.
    // A panel pribluda drew since the last copy rides in the same lock as the frame, into the
    // slot after the one last published; moving hudSequence on by one below names that slot.
    uint32_t    hudSlot = static_cast<uint32_t>(block->hudSequence + 1) % kFrameSlots;
    bool        copyPanel = state.hudPending && state.hudFramebuffer &&
                     state.hudShared[hudSlot].interopObject;
    GlPanelCopy panelCopy;
    if (copyPanel) {
        panelCopy.shared = &state.hudShared[hudSlot];
        panelCopy.sourceFramebuffer = state.hudFramebuffer;
        panelCopy.width = state.hudWidth;
        panelCopy.height = state.hudHeight;
    }

    int           transferring = frameBudgetEnter(kSpanTransfer);
    LARGE_INTEGER transferBegin, transferEnd;
    QueryPerformanceCounter(&transferBegin);
    const bool transferred = glInteropCopyBackBuffer(
        state.gl, state.interopDevice, state.shared[slot], width, height,
        renderTargetFramebuffer(), g_options.vrTransferFinish, copyPanel ? &panelCopy : nullptr);
    QueryPerformanceCounter(&transferEnd);
    frameBudgetLeave(transferring);
    presenterNoteTransfer(transferEnd.QuadPart - transferBegin.QuadPart);
    // A panel image not copied because the frame failed stays pending for the next frame.
    if (!transferred) return;

    // The copy above ends in wglDXUnlockObjectsNV, which completes the GL work on the shared
    // textures, and by default in glFinish as well (vrTransferFinish), so the pixels are
    // complete before the sequence that advertises them is published. That ordering is the
    // whole reason no lock is needed here.
    ++state.frameSequence;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->frameSequence),
                        state.frameSequence);
    vrNoteFramePublished();

    // The HUD panel copied under the same lock goes out now, after the same synchronisation,
    // so the presenter can never copy one that is still being written.
    if (copyPanel) {
        state.hudPending = false;
        if (panelCopy.copied) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->hudSequence),
                                block->hudSequence + 1);
            state.hudPublishedQpc = transferEnd.QuadPart;
        }
    }

    // The layer is published only while the game keeps drawing the panel. pribluda's draw
    // stops when the race ends, and a layer left on would go on showing its last image over
    // the lobby. Written after the sequence, so a panel coming back is never shown for a
    // display frame with the image from before.
    if (!state.qpcFrequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        state.qpcFrequency = frequency.QuadPart;
    }
    bool hudLive = state.hudPublishedQpc &&
                   transferEnd.QuadPart - state.hudPublishedQpc <
                       static_cast<LONGLONG>(state.qpcFrequency * kHudGraceSeconds);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->hudLayer),
                        hudLive ? g_options.vrHudLayer : 0);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block->producedFrames));

    if (haveRendered && state.acquiredViewSequence) {
        int lag = block->viewSequence - state.acquiredViewSequence;
        ++state.viewLag[lag <= 0 ? 0 : (lag >= 3 ? 3 : lag)];
    }
    if (!state.poseReportQpc) state.poseReportQpc = transferEnd.QuadPart;
    if (transferEnd.QuadPart - state.poseReportQpc >= 2 * state.qpcFrequency) {
        logf("vr: pose age at publish -- view sets the presenter published after the rendered "
             "one 0:%d 1:%d 2:%d 3+:%d | written over the slot of the presenter's last drawn "
             "frame %d times",
             state.viewLag[0], state.viewLag[1], state.viewLag[2], state.viewLag[3],
             state.slotOverwrites);
        for (int& count : state.viewLag) count = 0;
        state.slotOverwrites = 0;
        state.poseReportQpc = transferEnd.QuadPart;
    }

    if (!state.loggedFirstFrame) {
        state.loggedFirstFrame = true;
        logf("vr: first frame published -- %dx%d, %s", width, height,
             sideBySide ? "side by side" : "one image for both eyes");
    }

    // The return path, proven once rather than continuously. Nothing consumes these yet;
    // Phase 3 feeds them into the scene parameter block.
    if (!state.loggedFirstViews && block->viewsValid && block->viewSequence) {
        state.loggedFirstViews = true;
        logf("vr: first views received -- head (%.3f, %.3f, %.3f), left eye fov "
             "L%.4f R%.4f U%.4f D%.4f",
             block->headPose.positionX, block->headPose.positionY,
             block->headPose.positionZ, block->views[0].fov.angleLeft,
             block->views[0].fov.angleRight, block->views[0].fov.angleUp,
             block->views[0].fov.angleDown);
    }
}

void presenterEndFrame() {
    State& state = g_state;
    if (state.stage == Stage::Off) return;
    renderTargetEndFrame(state.gl);
}

void presenterBlitToWindow(int eye) {
    State& state = g_state;
    state.monitorHudTaken = false;
    if (state.stage == Stage::Off) return;
    renderTargetBlitToWindow(state.gl, eye);
    if (state.stage != Stage::Publishing || !state.block) return;
    frameBudgetSwitch(kSpanWindowHud);
    drawMonitorHud(state, eye);
}

void presenterDrawCursor(bool sideBySide) {
    State& state = g_state;
    // Off or Failed means there is no headset to be blind in, and the flat window
    // already has the real Windows pointer over it. Failed is included deliberately: the
    // GL entry points may well have resolved before whatever failed, and a second cursor
    // on the monitor would be a puzzle rather than a feature.
    if (state.stage == Stage::Off || state.stage == Stage::Failed) return;
    if (!state.gl.module) return;
    cursorOverlayDraw(state.gl, sideBySide);
}

void presenterRestoreUnderCursor() {
    State& state = g_state;
    // NOT gated on the stage, unlike the draw above. presenterFrame stops the channel on
    // the spot if the presenter process has exited, and that can land between the two
    // calls -- after which the saved rect would sit in the game's back buffer for the
    // rest of the session, which is the trail this exists to prevent.
    // cursorOverlayRestore is inert when nothing is saved, so this costs a branch.
    if (!state.gl.module) return;
    cursorOverlayRestore(state.gl);
}

void presenterBeginFrame() {
    State& state = g_state;
    if (state.stage == Stage::Off) return;
    if (!g_options.vr || !g_options.vrOffscreen) {
        // Not merely "do not bind": the engine's back buffer size has to go back to what
        // roglv2 believes, or the next frame is laid out for a surface that is no longer
        // the render target.
        renderTargetRelease(state.gl);
        return;
    }
    renderTargetBeginFrame(state.gl);
}

namespace {
long long g_transferTicks = 0;
int       g_transferCount = 0;
}  // namespace

void presenterNoteTransfer(long long ticks) {
    g_transferTicks += ticks;
    ++g_transferCount;
}

bool presenterTakeTransferCost(double& totalMs, int& count) {
    if (!g_transferCount) return false;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    totalMs = 1000.0 * static_cast<double>(g_transferTicks) /
              static_cast<double>(frequency.QuadPart);
    count = g_transferCount;
    g_transferTicks = 0;
    g_transferCount = 0;
    return true;
}

bool presenterFovMutable() {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return true;
    return state.block->fovMutable != 0;
}

int32_t presenterViewSequence() {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return 0;
    return state.block->viewsValid ? state.block->viewSequence : 0;
}

bool presenterLatestViews(SharedView views[2], SharedPose& head, SharedMotion* motion) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return false;

    ControlBlock* block = state.block;

    // Retry rather than give up. Returning false here drops the whole frame out of head
    // tracking and back to the flat IPD path -- one frame rendered from the car's own
    // camera, which reads as a jolt. The presenter publishes at 90 Hz and we read at 36,
    // so catching it mid-write is rare but not rare enough to shrug at, and three tries
    // makes it vanishingly unlikely.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int32_t sequence = block->viewSequence;
        if (!sequence || !block->viewsValid) return false;

        views[0] = block->views[0];
        views[1] = block->views[1];
        head = block->headPose;
        if (motion) *motion = block->motion;

        // If the sequence has not moved, nothing was published underneath the copy.
        if (block->viewSequence == sequence) {
            // The eye cameras ask for the motion; that call is the one a frame renders from.
            if (motion) state.acquiredViewSequence = sequence;
            return true;
        }
    }
    return false;
}

void presenterPublishSeat(const SharedPose& seat) {
    State& state = g_state;
    if (!state.block) return;

    ControlBlock* block = state.block;
    block->seatPose = seat;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block->seatGeneration));
}

bool presenterSeatPublished() {
    State& state = g_state;
    return state.block && state.block->seatGeneration != 0;
}

bool presenterHudBegin(int aspectWidth, int aspectHeight, int& width, int& height) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block || !state.device ||
        !state.interopDevice || !state.glContext || state.hudBound)
        return false;
    if (aspectWidth <= 0 || aspectHeight <= 0) return false;

    GlFunctions& gl = state.gl;
    if (!gl.bindFramebuffer) glResolveFramebuffer(gl);
    if (!gl.bindFramebuffer || !gl.framebufferTexture2D || !gl.checkFramebufferStatus ||
        !gl.genFramebuffers || !gl.deleteFramebuffers || !gl.blitFramebuffer || !gl.clear ||
        !gl.clearColor || !gl.viewport || !gl.getIntegerv || !gl.texImage2D ||
        !gl.texParameteri || !gl.dxLockObjects || !gl.dxUnlockObjects) {
        if (!state.hudReportedFailure) {
            state.hudReportedFailure = true;
            logf("vr: the HUD panel needs framebuffer objects, framebuffer blits and interop "
                 "locking, and GL did not provide them -- no panel");
        }
        return false;
    }

    int wantedHeight = static_cast<int>(static_cast<long long>(kHudTextureWidth) *
                                        aspectHeight / aspectWidth);
    if (wantedHeight < 1) wantedHeight = 1;
    if ((state.hudWidth != kHudTextureWidth || state.hudHeight != wantedHeight ||
         !state.hudFramebuffer) &&
        !createHudTextures(state, kHudTextureWidth, wantedHeight))
        return false;

    // The panel's own texture: no interop lock here. Drawn twice without a copy in between
    // -- a frame that never reached the transfer, or a skipped one -- it is simply drawn again.
    frameBudgetSwitch(kSpanPanelBind);
    gl.getIntegerv(kGlFramebufferBinding, &state.hudPreviousFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, state.hudFramebuffer);
    gl.viewport(0, 0, state.hudWidth, state.hudHeight);
    gl.clearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl.clear(GL_COLOR_BUFFER_BIT);

    state.hudBound = true;
    width = state.hudWidth;
    height = state.hudHeight;
    return true;
}

void presenterHudEnd() {
    State& state = g_state;
    if (!state.hudBound) return;
    state.hudBound = false;
    frameBudgetSwitch(kSpanPanelUnbind);

    state.gl.bindFramebuffer(kGlFramebuffer,
                             static_cast<GLuint>(state.hudPreviousFramebuffer));

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    state.hudDrawnQpc = now.QuadPart;
    state.hudPending = true;
}

bool presenterMonitorHudTaken() { return g_state.monitorHudTaken; }

void presenterStop() {
    State& state = g_state;
    if (state.stage == Stage::Off) return;

    if (state.block) {
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&state.block->shutdownRequested), 1);
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&state.block->producerState),
            static_cast<LONG>(ProcessState::Stopped));
    }

    releaseTextures(state);
    glInteropCloseDevice(state.gl, state.interopDevice);
    state.interopDevice = nullptr;

    if (state.context) state.context->Release();
    state.context = nullptr;
    if (state.device) state.device->Release();
    state.device = nullptr;

    if (state.presenterProcess) {
        // Give it a moment to notice shutdownRequested and end its session cleanly;
        // killing an OpenXR session leaves SteamVR wedged for the next run.
        WaitForSingleObject(state.presenterProcess, 3000);
        CloseHandle(state.presenterProcess);
        state.presenterProcess = nullptr;
    }

    if (state.block) UnmapViewOfFile(state.block);
    state.block = nullptr;
    if (state.mapping) CloseHandle(state.mapping);
    state.mapping = nullptr;

    state.stage = Stage::Off;
    state.width = 0;
    state.height = 0;
}

}  // namespace gplvr
