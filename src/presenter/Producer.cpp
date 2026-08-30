#include "presenter/Producer.h"

#include "GlInterop.h"
#include "Hooks.h"
#include "Log.h"
#include "Stereo.h"
#include "VrCamera.h"

#include <d3d11.h>
#include <dxgi.h>

#include <new>

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

    int  width = 0;
    int  height = 0;
    int  frameSequence = 0;
    bool loggedFirstFrame = false;
    bool loggedFirstViews = false;
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

void releaseTextures(State& state) {
    for (uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        glInteropUnregister(state.gl, state.interopDevice, state.shared[slot]);
        if (state.textures[slot]) state.textures[slot]->Release();
        state.textures[slot] = nullptr;
    }
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

        logf("vr: presenter is ready -- adapter %08lX:%08lX, recommends %ux%u per eye",
             (unsigned long)block->adapterLuidHigh, (unsigned long)block->adapterLuidLow,
             block->recommendedEyeWidth, block->recommendedEyeHeight);
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
        glInteropCloseDevice(state.gl, state.interopDevice);
        state.interopDevice = glInteropOpenDevice(state.gl, state.device);
        if (!state.interopDevice) {
            fail("the interop device could not be reopened for the new GL context");
            return;
        }
        state.width = 0;
        state.height = 0;
    }

    int width = 0;
    int height = 0;
    if (!glDrawableSize(state.gl, width, height)) return;

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

    if (!glInteropCopyBackBuffer(state.gl, state.interopDevice, state.shared[slot],
                                 width, height))
        return;

    // The copy above ends in glFinish, so the pixels are complete before the sequence
    // that advertises them is published. That ordering is the whole reason no lock is
    // needed here.
    ++state.frameSequence;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->frameSequence),
                        state.frameSequence);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block->producedFrames));

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

bool presenterLatestViews(SharedView views[2], SharedPose& head) {
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

        // If the sequence has not moved, nothing was published underneath the copy.
        if (block->viewSequence == sequence) return true;
    }
    return false;
}

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
