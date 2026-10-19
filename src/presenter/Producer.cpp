#include "presenter/Producer.h"

#include "RenderTarget.h"

#include "CursorOverlay.h"
#include "FrameBudget.h"
#include "FrameTrace.h"
#include "GlInterop.h"
#include "Hooks.h"
#include "Log.h"
#include "Paths.h"
#include "Pribluda.h"
#include "RenderPacing.h"
#include "Stereo.h"
#include "VrCamera.h"
#include "VrMenu.h"

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

    // The VR menu's panel: the same shape as the HUD's above, with one difference that
    // matters. Its image is published under a LOCK OF ITS OWN
    // (glInteropPublishPanel) rather than riding in the frame transfer's batched lock,
    // because that batch carries exactly one panel and widening it means editing the two
    // most delicate functions in the project. The menu is open for seconds at a time, so
    // one extra interop synchronisation on those frames is the cheap side of the trade.
    ID3D11Texture2D* menuTextures[kFrameSlots] = {};
    GlSharedTexture  menuShared[kFrameSlots] = {};
    int              menuWidth = 0;
    int              menuHeight = 0;
    GLuint           menuTexture = 0;
    GLuint           menuFramebuffer = 0;
    GLint            menuPreviousFramebuffer = 0;
    bool             menuBound = false;
    bool             menuReportedFailure = false;

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
    // Side-by-side frames held because they were rendered at a size other than the one the
    // presenter asked for: in the current run, and reports left.
    int  wrongSizeHeld = 0;
    int  wrongSizeReports = 8;
    bool loggedFirstFrame = false;
    bool loggedFirstViews = false;

    // vrDirectToShared, report 80. The slot the frame is being drawn straight into --
    // locked from before its first draw call until it is published -- or -1 when the frame
    // goes into our own target and is copied across at publish. `directGaveUp` is one-way:
    // a lock or an attachment that fails takes the session back to the copy for good,
    // because a path that alternates between the two would be neither measurable nor
    // debuggable.
    int  directSlot = -1;
    bool directGaveUp = false;
    bool directReported = false;
    bool directNotYetReported = false;
    // The window's picture has to be taken while the shared texture is still locked, which
    // is before the publish rather than after it. Set there, cleared by the blit that would
    // otherwise repeat it.
    bool windowBlitDone = false;

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
    // Beside gplvr.dll, which is also where the presenter finds its own runtime folder: it opens
    // runtime\openxr\win64\openxr_loader.dll relative to itself.
    const wchar_t* directory = pathsOwnDirectoryW();

    wchar_t exePath[MAX_PATH] = {};
    _snwprintf_s(exePath, _TRUNCATE, L"%sgplvr-xr.exe", directory);
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        logf("vr: %S is not there -- build the x64 configuration and copy gplvr-xr.exe "
             "and its runtime folder in beside gplvr.dll", exePath);
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

void releaseMenuTextures(State& state) {
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        glInteropUnregister(state.gl, state.interopDevice, state.menuShared[slot]);
        if (state.menuTextures[slot]) state.menuTextures[slot]->Release();
        state.menuTextures[slot] = nullptr;
    }
    if (state.menuFramebuffer && state.gl.deleteFramebuffers)
        state.gl.deleteFramebuffers(1, &state.menuFramebuffer);
    if (state.menuTexture && state.gl.deleteTextures)
        state.gl.deleteTextures(1, &state.menuTexture);
    state.menuFramebuffer = 0;
    state.menuTexture = 0;
    state.menuWidth = 0;
    state.menuHeight = 0;
    state.menuBound = false;
    if (state.block) InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&state.block->menuVisible), 0);
}

// Hand back a slot that is being rendered into without publishing it. The frame in it is
// lost, which is the right outcome everywhere this is called from: the textures are about
// to be unregistered, and unregistering one that GL still holds is the same fault as
// unlocking one that is still attached.
void dropDirect(State& state) {
    if (state.directSlot < 0) return;
    renderTargetAttachColour(state.gl, 0);
    glInteropPublishDirect(state.gl, state.interopDevice,
                           state.shared[state.directSlot], false, nullptr);
    state.directSlot = -1;
}

void releaseTextures(State& state) {
    dropDirect(state);
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        glInteropUnregister(state.gl, state.interopDevice, state.shared[slot]);
        if (state.textures[slot]) state.textures[slot]->Release();
        state.textures[slot] = nullptr;
    }
    releaseHudTextures(state);
    releaseMenuTextures(state);
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

void directGiveUp(State& state, const char* why) {
    if (state.directGaveUp) return;
    state.directGaveUp = true;
    renderTargetAttachColour(state.gl, 0);
    logf("vr: rendering straight into the shared texture is off for this session -- %s. The "
         "frame is copied into it at publish instead, as it was before vrDirectToShared.",
         why);
}

// Take the slot the next frame will be drawn into, lock it, and make it the off-screen
// target's colour attachment.
//
// Called after every publish rather than once a frame, because an extra render is a whole
// frame of its own and publishes on its own: between two of them there is no other moment
// at which a new slot could be taken.
//
// Everything that is not ready yet is a silent no-op, not a failure -- the textures are
// created at publish time and the framebuffer at bind time, so the first frames of a
// session, of a mode change and of a resolution change legitimately have neither. Those
// frames take the copy path, which is also the fallback if this is ever refused.
// Say once that the option is on and has not taken. Every reason below is a silent `return`,
// and a silently inert option reads exactly like one that is working.
void directNotYet(State& state, const char* why) {
    if (state.directReported || state.directNotYetReported) return;
    state.directNotYetReported = true;
    logf("vr: vrDirectToShared = 1 but the frame is still being copied -- %s. If this is the "
         "last word on it, the option did nothing this session.",
         why);
}

void acquireDirect(State& state) {
    if (!g_options.vrDirectToShared || state.directGaveUp) return;
    if (state.stage != Stage::Publishing || state.directSlot >= 0) return;
    if (!state.gl.dxLockObjects || !state.gl.dxUnlockObjects || !state.interopDevice) {
        directNotYet(state, "WGL_NV_DX_interop's lock entry points or the interop device are "
                            "not there");
        return;
    }

    int width = 0;
    int height = 0;
    if (!renderTargetSize(width, height)) {
        directNotYet(state,
                     "there is no off-screen render target to attach it to -- it did not "
                     "build, and the target: lines say why");
        return;
    }
    if (width != state.width || height != state.height) return;

    // The same arithmetic the publish uses: the presenter reads slot
    // (frameSequence % kFrameSlots), so the frame about to be drawn is numbered
    // frameSequence + 1 and belongs in the other one.
    uint32_t               slot = static_cast<uint32_t>(state.frameSequence + 1) % kFrameSlots;
    const GlSharedTexture& shared = state.shared[slot];
    if (!shared.interopObject || !shared.name) {
        directNotYet(state, "the shared textures are not registered with GL");
        return;
    }

    if (!glInteropLockDirect(state.gl, state.interopDevice, shared)) {
        directGiveUp(state, "wglDXLockObjectsNV would not give GL the slot");
        return;
    }
    if (!renderTargetAttachColour(state.gl, shared.name)) {
        glInteropPublishDirect(state.gl, state.interopDevice, shared, false, nullptr);
        directGiveUp(state, "the shared texture would not serve as the off-screen "
                            "framebuffer's colour attachment");
        return;
    }

    state.directSlot = static_cast<int>(slot);
    if (!state.directReported) {
        state.directReported = true;
        logf("vr: the frame is rendered straight into the shared texture -- slot %u, GL "
             "texture %u, %dx%d. No copy at publish; the interop lock is held for the whole "
             "frame.",
             slot, shared.name, width, height);
    }
}

// Pribluda's panel is 2048 texels wide whatever the monitor is: at the default 0.6 m that
// is over three texels a millimetre, finer than a headset resolves at arm's length.
constexpr int kHudTextureWidth = 2048;

// How long the panel layer stays published after the last panel image. Long enough to ride
// over a frame that skips the draw, short enough that leaving a race clears it at once.
constexpr double kHudGraceSeconds = 0.25;

// How many side-by-side frames rendered at a size other than the presenter's are held before
// they go out anyway. Race entry needs one to three; 30 is a third of a second at 90 Hz.
constexpr int kWrongSizeHoldFrames = 30;

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

// The VR menu's panel, built the same way pribluda's is: two shared D3D11 slots for the
// presenter to read, one plain GL texture the menu draws into with no interop lock, and a
// framebuffer over it. The only difference is where the image is published from.
bool createMenuTextures(State& state, int width, int height) {
    releaseMenuTextures(state);

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
                                                 &state.menuTextures[slot]))) {
            logf("vr: CreateTexture2D %dx%d failed for VR menu slot %u", width, height,
                 slot);
            state.menuTextures[slot] = nullptr;
            releaseMenuTextures(state);
            return false;
        }
        if (!glInteropRegister(state.gl, state.interopDevice, state.menuTextures[slot],
                               state.menuShared[slot])) {
            releaseMenuTextures(state);
            return false;
        }
        storeSharedHandle(state.block->menuTextures[slot],
                          state.menuShared[slot].shareHandle);
    }

    GlFunctions& gl = state.gl;
    GLint        previousTexture = 0;
    GLint        previousFramebuffer = 0;
    gl.getIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    gl.getIntegerv(kGlFramebufferBinding, &previousFramebuffer);
    gl.genTextures(1, &state.menuTexture);
    gl.bindTexture(GL_TEXTURE_2D, state.menuTexture);
    gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                  nullptr);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    gl.genFramebuffers(1, &state.menuFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, state.menuFramebuffer);
    gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
                            state.menuTexture, 0);
    GLenum status = gl.checkFramebufferStatus(kGlFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previousFramebuffer));
    if (status != kGlFramebufferComplete) {
        logf("vr: the VR menu's %dx%d framebuffer is incomplete (0x%04X) -- no menu layer",
             width, height, status);
        releaseMenuTextures(state);
        return false;
    }

    state.menuWidth = width;
    state.menuHeight = height;
    state.block->menuTextureWidth = static_cast<uint32_t>(width);
    state.block->menuTextureHeight = static_cast<uint32_t>(height);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state.block->menuGeneration));
    logf("vr: VR menu textures %dx%d shared, generation %d; the menu draws into GL "
         "texture %u and publishes under a lock of its own",
         width, height, state.block->menuGeneration, state.menuTexture);
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

// The VR menu's four corners, from the pose and extent the game published rather than
// from a placement's three scalars. Same output, same space (the seat's), so
// drawPanelInEye takes it unchanged -- which is the point: the monitor's copy of the menu
// uses the projection that already works for the HUD panel rather than a second one.
void panelPoseCorners(const SharedPose& pose, float widthMetres, float heightMetres,
                      float corners[4][3]) {
    const float halfW = widthMetres * 0.5f;
    const float halfH = heightMetres * 0.5f;
    // THE SAME ORDER panelCorners uses, because drawPanelInEye pairs a fixed texture
    // coordinate with each index. A different winding here would draw the menu upside
    // down on the monitor and nowhere else, which is the kind of fault that gets blamed
    // on the headset.
    static const float kSigns[4][2] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    for (int i = 0; i < 4; ++i) {
        const float local[3] = {kSigns[i][0] * halfW, kSigns[i][1] * halfH, 0.0f};
        float       turned[3];
        quatRotate(pose.orientationX, pose.orientationY, pose.orientationZ,
                   pose.orientationW, local, turned);
        corners[i][0] = pose.positionX + turned[0];
        corners[i][1] = pose.positionY + turned[1];
        corners[i][2] = pose.positionZ + turned[2];
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

    // WHY THIS FUNCTION EXISTS AT ALL, and why the VR menu now rides it too.
    //
    // A quad composition layer is composited by the RUNTIME, into the headset, and never
    // reaches GPL's own window. The HUD panel appears on the monitor only because this
    // draws it a second time, projecting its corners through each eye's rendered pose and
    // fov -- we are the compositor in the window. The menu became a quad layer for the
    // headset and would otherwise have vanished from the monitor the same day.
    //
    // The two have DIFFERENT gates and one shared setup. The HUD's gates are about
    // pribluda: its own monitor overlay is suppressed on the frames this one draws, so
    // this must not draw unless that redirect is in place. None of that is true of the
    // menu, which replaces nothing and is drawn whenever it is up.
    const char* common = nullptr;
    if (monitorEye == 0)
        common = "vrMonitorEye = 0 puts no picture in the window";
    else if (!renderTargetWindowPicture(picture))
        common = "the window shows no off-screen picture this frame";
    else if (!state.monitorViewsValid)
        common = "this frame has no tracked stereo views to place the panel with";
    else if (!gl.pushAttrib || !gl.popAttrib || !gl.pushMatrix || !gl.popMatrix ||
             !gl.matrixMode || !gl.loadIdentity || !gl.ortho || !gl.viewport || !gl.enable ||
             !gl.disable || !gl.scissor || !gl.bindTexture || !gl.texEnvi || !gl.blendFunc ||
             !gl.color4f || !gl.begin || !gl.end || !gl.texCoord2f || !gl.vertex4f ||
             !gl.polygonMode || !gl.colorMask || !gl.getIntegerv || !gl.bindFramebuffer)
        common = "GL entry points for a textured quad are missing";

    const char* reason = common;
    if (!reason) {
        if (!pribludaMonitorDrawRedirected())
            reason = "pribluda's monitor draw is not redirected (see the pribluda: lines)";
        else if (!live)
            reason = "no HUD panel drawn in the last 0.25 s (not in a race, or vrHudLayer 0)";
        else if (layer != 1 && layer != 2)
            reason = "vrHudLayer is not 1 or 2";
        else if (layer == 2 && !state.block->seatGeneration)
            reason = "no seat has been published yet";
    }
    // The line keeps saying what it always said: why the HUD PANEL is not over the
    // monitor. The menu's own absence is the VR menu's counters, not this one's.
    noteMonitorHud(state, reason, monitorEye);

    const bool drawHudPanel = reason == nullptr;
    const bool drawMenuPanel = common == nullptr && state.menuTexture &&
                               state.block->menuVisible && state.block->menuWidthMetres > 0.0f;
    if (!drawHudPanel && !drawMenuPanel) return;

    float corners[4][3];
    if (drawHudPanel)
        panelCorners(layer == 1 ? g_options.vrHudHead : g_options.vrHudSeat,
                     static_cast<float>(state.hudHeight) /
                         static_cast<float>(state.hudWidth),
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
    gl.texEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    gl.color4f(1.0f, 1.0f, 1.0f, 1.0f);
    // The panel is premultiplied: text opaque, pribluda's backdrops black at their coverage.
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (drawHudPanel) {
        gl.bindTexture(GL_TEXTURE_2D, state.hudTexture);
        bool seat = layer == 2;
        if (monitorEye == 1 || monitorEye == 2)
            drawPanelInEye(state, picture, 0, seat, corners);
        if (monitorEye == 1 || monitorEye == 3)
            drawPanelInEye(state, picture, 1, seat, corners);
    }

    // The menu, SECOND and therefore on top -- the same order the composition layers are
    // submitted in, so the monitor and the headset agree about what covers what. It goes
    // through the same drawPanelInEye: its quad is corners plus a texture like the HUD's,
    // and the only difference is that its corners come from a published pose rather than
    // from three placement scalars.
    if (drawMenuPanel) {
        float menuCorners[4][3];
        panelPoseCorners(state.block->menuPose, state.block->menuWidthMetres,
                         state.block->menuHeightMetres, menuCorners);
        gl.bindTexture(GL_TEXTURE_2D, state.menuTexture);
        // The space the game stated the pose in, not an assumption: HUD tweak over a
        // head-locked panel publishes in the view's space, and drawing that as if it were
        // seat-relative would put the menu somewhere the headset does not have it.
        const bool menuSeat = state.block->menuSpace == kMenuSpaceSeat;
        if (monitorEye == 1 || monitorEye == 2)
            drawPanelInEye(state, picture, 0, menuSeat, menuCorners);
        if (monitorEye == 1 || monitorEye == 3)
            drawPanelInEye(state, picture, 1, menuSeat, menuCorners);
    }

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
    // whenever the panel is out of view. Only the HUD sets this -- it is "gplvr drew
    // pribluda's overlay, so pribluda must not draw it again", and the menu is not
    // pribluda's overlay. A frame where only the menu was drawn leaves pribluda's own
    // monitor call alone, which is exactly right.
    if (drawHudPanel) state.monitorHudTaken = true;

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

void presenterFrame(bool sideBySide, bool stretchToFill, const int* monoRect) {
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
            // releaseTextures because it is not one of the shared frame textures. The
            // menu's font lists are display lists, which belong to it just as much.
            cursorOverlayReleaseGl();
            vrMenuReleaseGl(state.gl, false);
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
        // The old context is gone, so the lists are forgotten rather than deleted; the
        // next draw builds them again in the new one.
        vrMenuReleaseGl(state.gl, false);
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
    if (g_options.vr && block->targetEyeWidth && block->targetEyeHeight) {
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
        if (state.width) {
            logf("vr: render size changed %dx%d -> %dx%d, recreating shared textures",
                 state.width, state.height, width, height);
            char why[96];
            sprintf_s(why, "the render size changed %dx%d -> %dx%d", state.width, state.height,
                      width, height);
            frameTraceArm(12, why);
        }
        bool wasDirect = state.directSlot >= 0;
        if (!createTextures(state, width, height)) {
            fail("the shared textures could not be created");
            return;
        }
        // On the direct path this frame was drawn into a texture that has just been thrown
        // away with the rest, so there is nothing to publish: take a slot of the new size
        // and let the next frame be the first one at it. The copy path has the frame in its
        // own target and carries on.
        if (wasDirect) {
            acquireDirect(state);
            return;
        }
    }

    // A PAIR RENDERED AT THE WRONG SIZE IS HELD. On race entry the first frames render into the
    // window-sized surface, two 1280x1440 eyes, before the target reaches the size requested
    // above; published, each tall eye is shown in a square view, squashed or clipped. Nothing is
    // published for them, so the headset keeps its last frame. Capped, because a target the
    // driver cannot allocate stays on the window for good and its frames must still go out.
    if (sideBySide && g_options.vr && block->targetEyeWidth && block->targetEyeHeight) {
        int  wantWidth = static_cast<int>(block->targetEyeWidth) * 2;
        int  wantHeight = static_cast<int>(block->targetEyeHeight);
        bool wrongSize = width != wantWidth || height != wantHeight;
        frameTracePresenter(width, height, wantWidth, wantHeight,
                            wrongSize && state.wrongSizeHeld < kWrongSizeHoldFrames);
        if (!wrongSize) {
            if (state.wrongSizeHeld && state.wrongSizeHeld <= kWrongSizeHoldFrames &&
                state.wrongSizeReports > 0) {
                --state.wrongSizeReports;
                logf("vr: %d side-by-side frame%s rendered at the wrong size were held; the "
                     "target is %dx%d now",
                     state.wrongSizeHeld, state.wrongSizeHeld == 1 ? "" : "s", width, height);
            }
            state.wrongSizeHeld = 0;
        } else if (state.wrongSizeHeld < kWrongSizeHoldFrames) {
            ++state.wrongSizeHeld;
            return;
        } else if (state.wrongSizeHeld == kWrongSizeHoldFrames) {
            ++state.wrongSizeHeld;  // past the cap: said once, then published as they are
            if (state.wrongSizeReports > 0) {
                --state.wrongSizeReports;
                logf("vr: side-by-side frames are still %dx%d after %d held, against %dx%d "
                     "wanted -- publishing them as they are; the render target is not reaching "
                     "that size (see the target: lines)",
                     width, height, kWrongSizeHoldFrames, wantWidth, wantHeight);
            }
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
    //
    // A one-image frame names a rect only when part of it is the picture: GPL's static fills
    // the 3D view, the window's size at the top left of a frame sized for the headset, and the
    // rest keeps the previous picture. A rect outside the frame is dropped and the whole frame
    // shown.
    bool haveMonoRect = !sideBySide && monoRect && monoRect[0] >= 0 && monoRect[1] >= 0 &&
                        monoRect[2] > 0 && monoRect[3] > 0 &&
                        monoRect[0] + monoRect[2] <= width &&
                        monoRect[1] + monoRect[3] <= height;
    for (int eye = 0; eye < 2; ++eye) {
        int rect[4] = {};
        bool have = sideBySide ? stereoEyeRect(eye, rect) : haveMonoRect;
        if (!sideBySide && have)
            for (int i = 0; i < 4; ++i) rect[i] = monoRect[i];
        for (int i = 0; i < 4; ++i)
            block->eyeRect[eye][i] = have ? static_cast<uint32_t>(rect[i]) : 0u;
    }
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->stretchToFill),
                        stretchToFill ? 1 : 0);

    // The off-screen target rebuilds itself on a mode change as well as a resolution
    // change, and a rebuilt framebuffer has the target's OWN texture attached again. When
    // that has happened the frame did not land in the slot being held, so the slot is given
    // back unpublished and this frame takes the copy path.
    if (state.directSlot >= 0 &&
        renderTargetAttachedColour() != state.shared[state.directSlot].name)
        dropDirect(state);

    // The poses this frame was drawn from, echoed back untouched. Written before the
    // frame sequence that advertises the pixels, so the presenter never pairs a new
    // image with the previous frame's pose.
    // The slot must be the one the published sequence names, not the one before it:
    // the presenter reads slot (frameSequence % kFrameSlots), so the frame we are about
    // to write is numbered frameSequence + 1 and belongs in that slot.
    //
    // On the direct path the frame is already IN a slot -- chosen by this same arithmetic
    // when it was locked, before the frame's first draw call -- and that one is what gets
    // published, whatever the arithmetic says now.
    const uint32_t slot = state.directSlot >= 0
                              ? static_cast<uint32_t>(state.directSlot)
                              : static_cast<uint32_t>(state.frameSequence + 1) % kFrameSlots;

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
    bool transferred;
    if (state.directSlot >= 0) {
        // The pixels are already where the presenter will read them. What is left is
        // everything that still has to happen while GL holds the texture: the pointer's
        // saved pixels go back into the frame, the texture comes off the framebuffer -- the
        // unlock fails if it does not -- and the panel rides out under the same unlock.
        //
        // The window's picture was taken in presenterEndFrame, for the same reason the
        // restore is here: after the unlock this texture belongs to Direct3D.
        cursorOverlayRestore(state.gl);
        renderTargetAttachColour(state.gl, 0);
        transferred = glInteropPublishDirect(state.gl, state.interopDevice,
                                             state.shared[slot], g_options.vrTransferFinish,
                                             copyPanel ? &panelCopy : nullptr);
        state.directSlot = -1;
    } else {
        transferred = glInteropCopyBackBuffer(state.gl, state.interopDevice,
                                              state.shared[slot], width, height,
                                              renderTargetFramebuffer(),
                                              g_options.vrTransferFinish,
                                              copyPanel ? &panelCopy : nullptr);
    }
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
    renderPacingNotePublished();

    // The next frame's slot is taken here rather than at the top of the next frame: an
    // extra render draws and publishes a whole frame of its own between the two, and the
    // game's own next frame starts drawing without passing through presenterBeginFrame
    // again when it does.
    acquireDirect(state);

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

    // On the direct path the window's picture is read out of the shared texture, so it has
    // to be taken here -- the frame is finished and the texture is still locked to GL. The
    // publish that follows hands it to Direct3D, after which GL may not read it at all.
    //
    // Only the blit moves. The panel that goes over the picture is drawn into the window's
    // own buffer and stays where it was, after the publish, so it still uses the views this
    // frame was rendered from.
    if (state.directSlot >= 0) {
        frameBudgetSwitch(kSpanWindowBlit);
        renderTargetBlitToWindow(state.gl, g_options.vrMonitorEye);
        state.windowBlitDone = true;
    }
}

void presenterBlitToWindow(int eye) {
    State& state = g_state;
    state.monitorHudTaken = false;
    // Cleared before the stage is looked at, so a stage that changed between the two calls
    // cannot leave the flag set and cost the next frame its picture.
    bool alreadyBlitted = state.windowBlitDone;
    state.windowBlitDone = false;
    if (state.stage == Stage::Off) return;
    if (!alreadyBlitted) renderTargetBlitToWindow(state.gl, eye);
    if (state.stage != Stage::Publishing || !state.block) return;
    frameBudgetSwitch(kSpanWindowHud);
    drawMonitorHud(state, eye);
}

bool presenterLatestInput(SharedInput& input, int32_t& sequence) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return false;

    ControlBlock* block = state.block;

    // Three tries, as presenterLatestViews takes for the same race: the presenter writes
    // at the display rate, we read at the game's, and a copy caught mid-write would hand
    // the edge detector a button that is down in one field and up in another.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int32_t published = block->inputSequence;
        if (!published) return false;

        input = block->input;

        if (block->inputSequence == published) {
            sequence = published;
            return true;
        }
    }
    return false;
}

void presenterUpdateVrMenu() {
    State& state = g_state;
    // Off or Failed means there is no headset and no layer to publish into. Unlike the
    // cursor this is not about the monitor: the monitor's copy is drawn from the same
    // texture later, by drawMonitorHud, and it has nothing to show until this has run.
    if (state.stage != Stage::Publishing || !state.gl.module) return;
    vrMenuUpdate(state.gl);
}

void presenterDrawVrRay(bool takeWindowPicture) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.gl.module) return;
    if (!vrMenuRayWanted()) return;

    // THE RAY GOES IN AFTER THE WINDOW'S PICTURE HAS BEEN TAKEN. That ordering is the
    // whole reason this is a function of its own rather than a line beside the cursor.
    //
    // The monitor shows the off-screen frame, so anything drawn into it before the blit
    // is on the monitor too -- which is right for the cursor and wrong for the ray: the
    // ray is a pointing aid for the person in the headset and means nothing to somebody
    // watching the flat window. Taking the blit first costs nothing (the frame is
    // finished either way) and windowBlitDone already exists to stop it happening twice.
    //
    // Nothing moves unless the menu is open, so a session that never opens it sees
    // exactly the ordering it always had.
    //
    // An EXTRA render does not take it: that path publishes a frame of its own and never
    // blits to the window, and the main frame that follows redraws the whole target
    // before taking its own picture -- so the extra frame's ray cannot reach the monitor
    // through it, and forcing a blit here would put an intermediate frame on the screen.
    if (takeWindowPicture && !state.windowBlitDone && renderTargetActive()) {
        frameBudgetSwitch(kSpanWindowBlit);
        renderTargetBlitToWindow(state.gl, g_options.vrMonitorEye);
        state.windowBlitDone = true;
    }

    // Into the off-screen target, which is the frame the headset gets. Without it the
    // frame IS the window's back buffer and there is nowhere to put the ray that the
    // monitor would not also show.
    const GLuint framebuffer = renderTargetFramebuffer();
    if (!renderTargetActive() || !framebuffer) {
        vrMenuNoteRayNeedsOffscreen();
        return;
    }

    GLint previous = 0;
    state.gl.getIntegerv(kGlFramebufferBinding, &previous);
    state.gl.bindFramebuffer(kGlFramebuffer, framebuffer);
    vrMenuDrawRay(state.gl);
    state.gl.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previous));
}

bool presenterLatestInputNames(SharedInputNames& names, int32_t& generation) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return false;

    ControlBlock* block = state.block;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int32_t published = block->inputNamesGeneration;
        if (!published) return false;
        names = block->inputNames;
        if (block->inputNamesGeneration == published) {
            generation = published;
            return true;
        }
    }
    return false;
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
    // Rendering off-screen is what VR IS here -- the eyes need a target of the runtime's own
    // size, the ray needs somewhere the monitor cannot see, and the window is a view of that
    // frame rather than the frame itself. It was an option until 2026-09-27 and turning it off
    // broke each of those in a different way.
    if (!g_options.vr) {
        // Not merely "do not bind": the engine's back buffer size has to go back to what
        // roglv2 believes, or the next frame is laid out for a surface that is no longer
        // the render target. A slot still held goes back first -- the framebuffer it is
        // attached to is about to be deleted.
        dropDirect(state);
        renderTargetRelease(state.gl);
        return;
    }
    renderTargetBeginFrame(state.gl);

    // After the bind, not before it: the framebuffer the slot is attached to is built in
    // there. Inert whenever a slot is already held, which is the usual case -- the publish
    // takes the next one as soon as it lets the last one go.
    acquireDirect(state);
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

int32_t presenterAcquiredViewSequence() { return g_state.acquiredViewSequence; }

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

bool presenterClock(PresenterClock& out) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return false;

    ControlBlock* block = state.block;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int32_t sequence = block->viewSequence;
        if (!sequence || !block->viewsValid) return false;

        SharedMotion motion = block->motion;
        if (block->viewSequence != sequence) continue;
        if (!(motion.flags & kMotionTiming)) return false;

        out.latchTime = loadQpc(motion.latchTime);
        out.displayTime = loadQpc(motion.displayTime);
        out.period = loadQpc(motion.period);
        out.viewLead = motion.viewLead;
        out.refreshRate = motion.refreshRate;
        return out.period > 0;
    }
    return false;
}

void presenterSetViewLead(int lead) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return;
    if (state.block->viewLeadDisplays == lead) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state.block->viewLeadDisplays), lead);
}

int32_t presenterPublishedSequence() { return g_state.frameSequence; }

int32_t presenterConsumedSequence() {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block) return 0;
    return state.block->consumedSequence;
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

bool presenterMenuBegin(int width, int height, int& actualWidth, int& actualHeight) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block || !state.device ||
        !state.interopDevice || !state.glContext || state.menuBound)
        return false;
    if (width <= 0 || height <= 0) return false;

    GlFunctions& gl = state.gl;
    if (!gl.bindFramebuffer) glResolveFramebuffer(gl);
    if (!gl.bindFramebuffer || !gl.framebufferTexture2D || !gl.checkFramebufferStatus ||
        !gl.genFramebuffers || !gl.deleteFramebuffers || !gl.blitFramebuffer || !gl.clear ||
        !gl.clearColor || !gl.viewport || !gl.getIntegerv || !gl.texImage2D ||
        !gl.texParameteri || !gl.dxLockObjects || !gl.dxUnlockObjects) {
        if (!state.menuReportedFailure) {
            state.menuReportedFailure = true;
            logf("vr: the VR menu's layer needs framebuffer objects, framebuffer blits and "
                 "interop locking, and GL did not provide them -- no menu layer");
        }
        return false;
    }

    if ((state.menuWidth != width || state.menuHeight != height || !state.menuFramebuffer) &&
        !createMenuTextures(state, width, height))
        return false;

    gl.getIntegerv(kGlFramebufferBinding, &state.menuPreviousFramebuffer);
    gl.bindFramebuffer(kGlFramebuffer, state.menuFramebuffer);
    gl.viewport(0, 0, state.menuWidth, state.menuHeight);
    gl.clearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl.clear(GL_COLOR_BUFFER_BIT);

    state.menuBound = true;
    actualWidth = state.menuWidth;
    actualHeight = state.menuHeight;
    return true;
}

void presenterMenuEnd() {
    State& state = g_state;
    if (!state.menuBound) return;
    state.menuBound = false;
    state.gl.bindFramebuffer(kGlFramebuffer,
                             static_cast<GLuint>(state.menuPreviousFramebuffer));
}

bool presenterMenuPublish(const SharedPose& pose, int32_t space, float widthMetres,
                          float heightMetres) {
    State& state = g_state;
    if (state.stage != Stage::Publishing || !state.block || !state.menuFramebuffer)
        return false;
    if (state.menuBound) return false;  // the draw target is still bound; nothing to copy

    ControlBlock* block = state.block;
    // The slot after the one last published, exactly as the HUD picks its own: advancing
    // the sequence below names the slot the presenter will then read.
    const uint32_t slot = static_cast<uint32_t>(block->menuSequence + 1) % kFrameSlots;
    if (!state.menuShared[slot].interopObject) return false;

    GlPanelCopy copy;
    copy.shared = &state.menuShared[slot];
    copy.sourceFramebuffer = state.menuFramebuffer;
    copy.width = state.menuWidth;
    copy.height = state.menuHeight;
    if (!glInteropPublishPanel(state.gl, state.interopDevice, copy)) return false;

    // The placement goes out BEFORE the sequence that advertises the image, so the
    // presenter can never place this frame's picture at the last frame's pose.
    block->menuPose = pose;
    block->menuSpace = space;
    block->menuWidthMetres = widthMetres;
    block->menuHeightMetres = heightMetres;
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->menuSequence),
                        block->menuSequence + 1);
    // Visible last of all: a layer switched on before its first image would show one
    // display frame of whatever the texture happened to contain.
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->menuVisible), 1);
    return true;
}

void presenterMenuHide() {
    State& state = g_state;
    if (!state.block) return;
    if (!state.block->menuVisible) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state.block->menuVisible), 0);
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
