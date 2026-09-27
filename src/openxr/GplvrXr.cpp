// gplvr-xr -- an OpenXR survey and end-to-end submission test.
//
// It touches nothing belonging to Grand Prix Legends. Run it on its own, before any
// game code exists, to answer the questions Phase 2 is built on:
//
//   * which runtime does the loader actually select, and why (registry / env override)
//   * does that runtime exist for this bitness at all
//   * what extensions, blend modes and swapchain formats does it offer
//   * what per-eye resolution and field of view does it want
//   * does a projection layer actually reach the headset
//
// Build it 32-bit as well as 64-bit. gplc67.exe is Win32, so "is there a 32-bit
// implementation of this runtime" is the single fact that decides whether OpenXR can
// ever live inside the game process or whether an x64 companion is mandatory. That is a
// per-runtime answer, not a general one, so it has to be measured on the user's machine
// rather than argued from what vendors are said to ship.
//
// It has two modes. With no arguments it is the survey above. With --present <instance>
// the game has launched it, and it becomes the presenter: it opens the game's shared
// texture, blits each eye into a swapchain and publishes head poses back.
//
// Usage:
//   gplvr-xr [--runtime <path-to-runtime.json>] [--seconds N] [--no-session]
//   gplvr-xr --present <instance> [--render-scale N]   (launched by the game)
//
// --runtime sets XR_RUNTIME_JSON for this process only, which is how you point the
// loader straight at one vendor's runtime without changing any machine-wide setting.

#include "Log.h"
#include "openxr/Xr.h"
#include "openxr/XrInput.h"
#include "presenter/Consumer.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

using namespace gplvr;

namespace {

XrApi      g_api;
XrInstance g_instance = XR_NULL_HANDLE;

// Shorthand so every call site reads the same way and no failure goes unlogged.
#define XR_CHECK(expr)                                                          \
    ([&]() -> bool {                                                            \
        const XrResult xrCheckResult = (expr);                                  \
        if (XR_FAILED(xrCheckResult)) {                                         \
            logf("xr: %s failed -- %s", #expr,                                  \
                 xrResultName(g_api, g_instance, xrCheckResult));               \
            return false;                                                       \
        }                                                                       \
        return true;                                                            \
    })()

struct Options {
    const wchar_t* runtimeJson = nullptr;
    // The game's instance name, handed to us by the producer when it launches us. Its
    // presence is what turns this from a diagnostic into the presenter.
    const wchar_t* presentInstance = nullptr;
    // Multiplies the runtime's recommended per-eye size. Passed by the producer from
    // gplvr.ini so both halves agree without the presenter reading the game's config
    // file. Clamped at use against the runtime's reported maximum, not against 1.0.
    float          renderScale = 1.0f;
    int            seconds = 20;
    bool           session = true;

};

// Ctrl+C, and why it needs handling rather than the default.
//
// The default handler calls ExitProcess from a second thread. With a frame in flight
// that tears the runtime down underneath itself, and PicoStreamingXR hangs -- which is
// what "hangs on Ctrl+C, had to force it" was. Setting a flag and letting the frame
// loop exit through the same path as Esc means the session is ended properly. A second
// Ctrl+C returns FALSE and lets the default handler kill us, so an unresponsive runtime
// cannot trap the process forever.
volatile bool g_stopRequested = false;

BOOL WINAPI consoleControlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_stopRequested) return FALSE;  // second press: let Windows kill us
            g_stopRequested = true;
            logf("xr: stop requested (Ctrl+C) -- ending the session cleanly. Press it "
                 "again to force.");
            return TRUE;
        default:
            return FALSE;
    }
}

bool g_fovMutable = true;

Options parseCommandLine(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--runtime") && i + 1 < argc) {
            options.runtimeJson = argv[++i];
        } else if (!wcscmp(argv[i], L"--present") && i + 1 < argc) {
            options.presentInstance = argv[++i];
            // Presenting runs until the game says stop, not for a fixed spell.
            options.seconds = 0;
        } else if (!wcscmp(argv[i], L"--render-scale") && i + 1 < argc) {
            options.renderScale = static_cast<float>(_wtof(argv[++i]));
        } else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) {
            options.seconds = _wtoi(argv[++i]);
        } else if (!wcscmp(argv[i], L"--no-session")) {
            options.session = false;
        } else {
            wprintf(L"unrecognised argument: %s\n", argv[i]);
        }
    }
    return options;
}

// -- survey ----------------------------------------------------------------------

void logApiLayers() {
    uint32_t count = 0;
    if (!XR_CHECK(g_api.xrEnumerateApiLayerProperties(0, &count, nullptr))) return;
    if (!count) {
        logf("xr: no API layers installed");
        return;
    }

    std::vector<XrApiLayerProperties> layers(count, {XR_TYPE_API_LAYER_PROPERTIES});
    if (!XR_CHECK(g_api.xrEnumerateApiLayerProperties(count, &count, layers.data())))
        return;

    // Worth listing: an implicit API layer that injects itself into every session is a
    // prime suspect when a runtime behaves differently here than in another engine.
    for (const XrApiLayerProperties& layer : layers)
        logf("xr: API layer %s v%u -- %s", layer.layerName,
             (unsigned)layer.layerVersion, layer.description);
}

bool hasExtension(const std::vector<XrExtensionProperties>& extensions,
                  const char* name) {
    for (const XrExtensionProperties& extension : extensions)
        if (!strcmp(extension.extensionName, name)) return true;
    return false;
}

std::vector<XrExtensionProperties> enumerateExtensions() {
    uint32_t count = 0;
    if (!XR_CHECK(g_api.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count,
                                                               nullptr)))
        return {};

    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (!XR_CHECK(g_api.xrEnumerateInstanceExtensionProperties(
            nullptr, count, &count, extensions.data())))
        return {};

    logf("xr: %u instance extensions", count);
    for (const XrExtensionProperties& extension : extensions)
        logf("xr:   %s (v%u)", extension.extensionName,
             (unsigned)extension.extensionVersion);
    return extensions;
}

const char* blendModeName(XrEnvironmentBlendMode mode) {
    switch (mode) {
        case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:      return "OPAQUE";
        case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:    return "ADDITIVE";
        case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return "ALPHA_BLEND";
        default:                                    return "unknown";
    }
}

const char* dxgiFormatName(int64_t format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM:      return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM:   return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_D32_FLOAT:           return "D32_FLOAT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT:   return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D16_UNORM:           return "D16_UNORM";
        default:                              return "other";
    }
}

// -- D3D11 -----------------------------------------------------------------------

// The runtime names the adapter it wants by LUID. Using any other one either fails
// outright or silently costs a cross-adapter copy every frame, so match it exactly and
// say which card it turned out to be.
IDXGIAdapter1* findAdapter(LUID luid) {
    // A zero LUID means the runtime does not care which adapter we use.
    //
    // PicoStreamingXR reports 00000000:00000000, which is legal -- the OpenXR spec only
    // requires the runtime to name an adapter when it actually needs a specific one. The
    // first version treated "no adapter matched" as fatal and stopped, so a runtime that
    // was being permissive looked like a runtime that was broken.
    const bool anyAdapterWillDo = luid.LowPart == 0 && luid.HighPart == 0;
    if (anyAdapterWillDo)
        logf("xr: the runtime named no adapter (LUID is zero) -- it accepts any, so "
             "taking the first one DXGI reports");

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        logf("xr: CreateDXGIFactory1 failed");
        return nullptr;
    }

    IDXGIAdapter1* match = nullptr;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        const bool isMatch = anyAdapterWillDo
                                 ? (match == nullptr)
                                 : (desc.AdapterLuid.LowPart == luid.LowPart &&
                                    desc.AdapterLuid.HighPart == luid.HighPart);
        logf("xr: adapter %u %S luid=%08lX:%08lX%s", index, desc.Description,
             (unsigned long)desc.AdapterLuid.HighPart,
             (unsigned long)desc.AdapterLuid.LowPart,
             isMatch ? (anyAdapterWillDo ? "  <-- using this one"
                                         : "  <-- the one OpenXR asked for")
                     : "");

        if (isMatch && !match)
            match = adapter;
        else
            adapter->Release();
    }

    factory->Release();
    return match;
}

// -- the frame loop --------------------------------------------------------------

struct Eye {
    XrSwapchain                    swapchain = XR_NULL_HANDLE;
    uint32_t                       width = 0;
    uint32_t                       height = 0;
    std::vector<ID3D11RenderTargetView*> targets;
    bool                           decodeSrgb = false;  // see swapchainViewFormat
};

void releaseEye(Eye& eye) {
    for (ID3D11RenderTargetView* target : eye.targets)
        if (target) target->Release();
    eye.targets.clear();
    if (eye.swapchain) g_api.xrDestroySwapchain(eye.swapchain);
    eye.swapchain = XR_NULL_HANDLE;
}

// The format to draw into a swapchain image through, and whether the blit must decode.
//
// GPL's pixels are already sRGB-encoded, and an _SRGB swapchain declares its contents to be
// encoded, so they belong in it byte for byte -- through a UNORM view of the image, which
// D3D11 allows only when the runtime created the image TYPELESS. A typed _SRGB image takes
// only an _SRGB view, which encodes on write, so the blit decodes first and the view puts
// the bytes back. Any other format holds linear light and needs the decode as well.
DXGI_FORMAT swapchainViewFormat(ID3D11Texture2D* texture, int64_t format, bool& decodeSrgb,
                                DXGI_FORMAT& textureFormat) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    textureFormat = desc.Format;
    decodeSrgb = false;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    decodeSrgb = true;
    return static_cast<DXGI_FORMAT>(format);
}

// -- pribluda's overlay on a quad layer ----------------------------------------------
//
// The game draws the overlay into a shared texture; each new image is copied into a
// swapchain of the same size and submitted as a quad, following the head in VIEW space or
// fixed to the seat. Between new images the last released one is submitted again, and the
// runtime places the quad from the current head pose on every display frame -- which is
// why the panel holds still however the game's own frames are paced.

struct HudLayer {
    Eye      image;  // the swapchain and its render target views, sized like the texture
    uint32_t failedWidth = 0;
    uint32_t failedHeight = 0;
    int32_t  drawnSequence = 0;
    bool     haveImage = false;
    XrSpace  seatSpace = XR_NULL_HANDLE;
    int32_t  seatGeneration = 0;
    int32_t  loggedLayer = -1;
    bool     loggedNoSeat = false;
};

// Builds a quad layer's swapchain. Two layers use it -- pribluda's HUD panel and the VR
// menu -- so `what` names which one in every line it prints. Without it the menu's
// swapchain announced itself as the HUD's, three times a session, which is the kind of
// mislabelled diagnostic that costs an hour the next time something here is wrong.
bool createHudSwapchain(XrSession session, ID3D11Device* device, int64_t format,
                        uint32_t width, uint32_t height, HudLayer& hud, const char* what) {
    releaseEye(hud.image);
    hud.haveImage = false;
    hud.drawnSequence = 0;

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!XR_CHECK(g_api.xrCreateSwapchain(session, &info, &hud.image.swapchain))) {
        hud.image.swapchain = XR_NULL_HANDLE;
        hud.failedWidth = width;
        hud.failedHeight = height;
        logf("xr: the %s swapchain %ux%u could not be created -- no panel", what, width,
             height);
        return false;
    }
    hud.image.width = width;
    hud.image.height = height;

    uint32_t count = 0;
    XR_CHECK(g_api.xrEnumerateSwapchainImages(hud.image.swapchain, 0, &count, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(g_api.xrEnumerateSwapchainImages(
        hud.image.swapchain, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    for (const XrSwapchainImageD3D11KHR& image : images) {
        ID3D11RenderTargetView* target = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC desc = {};
        viewFormat = swapchainViewFormat(image.texture, format, hud.image.decodeSrgb,
                                         textureFormat);
        desc.Format = viewFormat;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateRenderTargetView(image.texture, &desc, &target))) {
            logf("xr: CreateRenderTargetView failed for a %s image -- no panel", what);
            releaseEye(hud.image);
            hud.failedWidth = width;
            hud.failedHeight = height;
            return false;
        }
        hud.image.targets.push_back(target);
    }

    hud.failedWidth = 0;
    hud.failedHeight = 0;
    logf("xr: %s swapchain %ux%u, %u images, %s -- image format %d, drawn through %s%s",
         what, width, height, count, dxgiFormatName(format), (int)textureFormat,
         dxgiFormatName(viewFormat),
         hud.image.decodeSrgb ? " with an sRGB decode in the blit" : "");
    return true;
}

// Copy the game's newest panel image into the swapchain when there is one. True when an
// image has been released and the quad can be submitted.
// The VR menu's image, into its own swapchain. The same shape as the HUD's, with
// Consumer::drawMenu in place of drawHud -- they read different shared slots and nothing
// else about them differs.
bool updateMenuImage(Consumer& consumer, ID3D11DeviceContext* context,
                     const Consumer::MenuPanel& panel, HudLayer& menu) {
    if (!panel.sequence || panel.sequence == menu.drawnSequence) return menu.haveImage;

    uint32_t                    imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XR_CHECK(g_api.xrAcquireSwapchainImage(menu.image.swapchain, &acquireInfo,
                                                &imageIndex)))
        return menu.haveImage;

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XR_CHECK(g_api.xrWaitSwapchainImage(menu.image.swapchain, &waitInfo));

    bool drawn = imageIndex < menu.image.targets.size() &&
                 consumer.drawMenu(menu.image.targets[imageIndex], menu.image.width,
                                   menu.image.height, panel.sequence,
                                   menu.image.decodeSrgb);
    context->Flush();

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(g_api.xrReleaseSwapchainImage(menu.image.swapchain, &releaseInfo));

    if (drawn) {
        menu.drawnSequence = panel.sequence;
        menu.haveImage = true;
    }
    return menu.haveImage;
}

bool updateHudImage(Consumer& consumer, ID3D11DeviceContext* context,
                    const Consumer::HudPanel& panel, HudLayer& hud) {
    if (!panel.sequence || panel.sequence == hud.drawnSequence) return hud.haveImage;

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XR_CHECK(
            g_api.xrAcquireSwapchainImage(hud.image.swapchain, &acquireInfo, &imageIndex)))
        return hud.haveImage;

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XR_CHECK(g_api.xrWaitSwapchainImage(hud.image.swapchain, &waitInfo));

    bool drawn = imageIndex < hud.image.targets.size() &&
                 consumer.drawHud(hud.image.targets[imageIndex], hud.image.width,
                                  hud.image.height, panel.sequence, hud.image.decodeSrgb);
    // The same reason as the eyes: the copy has to reach the GPU before the image is
    // released, or the runtime composites the previous contents.
    context->Flush();

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(g_api.xrReleaseSwapchainImage(hud.image.swapchain, &releaseInfo));

    if (drawn) {
        hud.drawnSequence = panel.sequence;
        hud.haveImage = true;
    }
    return hud.haveImage;
}

// The seat space follows the game's recentre: rebuilt from the published seat pose
// whenever its generation moves.
void updateSeatSpace(XrSession session, const Consumer& consumer, HudLayer& hud,
                     const char* what) {
    SharedPose seat;
    int32_t    generation = 0;
    if (!consumer.seat(seat, generation) || generation == hud.seatGeneration) return;
    hud.seatGeneration = generation;

    if (hud.seatSpace) g_api.xrDestroySpace(hud.seatSpace);
    hud.seatSpace = XR_NULL_HANDLE;

    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace.orientation = {seat.orientationX, seat.orientationY,
                                             seat.orientationZ, seat.orientationW};
    info.poseInReferenceSpace.position = {seat.positionX, seat.positionY, seat.positionZ};
    if (!XR_CHECK(g_api.xrCreateReferenceSpace(session, &info, &hud.seatSpace))) {
        hud.seatSpace = XR_NULL_HANDLE;
        logf("xr: %s -- the seat space could not be created", what);
        return;
    }
    logf("xr: %s -- seat space built from recentre %d at (%.3f, %.3f, %.3f)", what,
         generation, seat.positionX, seat.positionY, seat.positionZ);
}

void releaseHud(HudLayer& hud) {
    if (hud.seatSpace) g_api.xrDestroySpace(hud.seatSpace);
    hud.seatSpace = XR_NULL_HANDLE;
    releaseEye(hud.image);
}

// -- the display clock, for the game's position prediction ----------------------------

constexpr const char* kQpcTimeExtension = "XR_KHR_win32_convert_performance_counter_time";
typedef XrResult(XRAPI_PTR* ConvertTimeToQpcFn)(XrInstance, XrTime, LARGE_INTEGER*);

// The display's refresh rate, asked of the runtime (XR_FB_display_refresh_rate) rather than
// taken from its frame timing. SteamVR under PICO Connect starts its compositor at 90 Hz and
// keeps predictedDisplayPeriod there after the driver switches the headset to 72.
bool g_refreshRateEnabled = false;

// The rate the runtime reports now, or 0 when the extension is off or the call fails.
float queryRefreshRate(XrSession session) {
    if (!g_refreshRateEnabled || !g_api.xrGetDisplayRefreshRateFB) return 0.0f;
    float rate = 0.0f;
    if (XR_FAILED(g_api.xrGetDisplayRefreshRateFB(session, &rate)) || !(rate > 0.0f))
        return 0.0f;
    return rate;
}

void logRefreshRates(XrSession session) {
    if (!g_refreshRateEnabled || !g_api.xrEnumerateDisplayRefreshRatesFB) return;
    uint32_t count = 0;
    if (!XR_CHECK(g_api.xrEnumerateDisplayRefreshRatesFB(session, 0, &count, nullptr)) || !count)
        return;
    std::vector<float> rates(count, 0.0f);
    if (!XR_CHECK(g_api.xrEnumerateDisplayRefreshRatesFB(session, count, &count, rates.data())))
        return;
    char   text[256] = {};
    size_t used = 0;
    for (uint32_t i = 0; i < count && used < sizeof(text); ++i) {
        int wrote = std::snprintf(text + used, sizeof(text) - used, "%s%.2f", i ? ", " : "",
                                  rates[i]);
        if (wrote < 0) break;
        used += static_cast<size_t>(wrote);
    }
    logf("xr: the display offers %u refresh rate(s): %s Hz", count, text);
}

// -- world-locked markers ---------------------------------------------------------
//
// The first version of this tool cleared each eye to a flat colour and swept a bar
// across it. That proved frames were arriving at 90 Hz, and it could not possibly show
// whether head tracking worked: the content did not depend on the pose, so it looked
// identical whichever way you turned. Reported as "the lines were flat regardless of my
// view direction", which is exactly right, and not a tracking fault.
//
// These markers sit at fixed points in the reference space and are projected through
// each eye's own pose and frustum. Turn your head and they sweep past the head-locked
// centre box. If they move, tracking works; if they are pinned to your view, it does not.

struct Marker {
    float x, y, z;
    bool  bright;
};

// Dense enough to read as a dashed horizon line rather than a few dots, because in
// presenting mode this ring is a REFERENCE laid over the game's own horizon: both sit
// in the horizontal plane through the eye, so if the frustum the game rendered matches
// the one we declare, the two stay locked together at every head angle.
constexpr int kMarkerCount = 24;
Marker g_markers[kMarkerCount] = {};
bool   g_markersPlaced = false;

void placeMarkers(const XrView& view) {
    // A ring three metres out, level with the head, index 0 straight ahead. OpenXR is
    // +Y up and -Z forward, so "ahead" is negative Z.
    constexpr float kRadius = 3.0f;
    for (int i = 0; i < kMarkerCount; ++i) {
        const float angle =
            static_cast<float>(i) * (6.2831853f / static_cast<float>(kMarkerCount));
        g_markers[i].x = view.pose.position.x + kRadius * std::sin(angle);
        g_markers[i].y = view.pose.position.y;
        g_markers[i].z = view.pose.position.z - kRadius * std::cos(angle);
        g_markers[i].bright = (i % 2) == 0;
    }
    g_markersPlaced = true;
}

// v rotated by the inverse of q, i.e. world-relative into eye-local.
void rotateByInverse(const XrQuaternionf& q, const float v[3], float out[3]) {
    const float x = -q.x, y = -q.y, z = -q.z, w = q.w;
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Project a DIRECTION -- a point at infinite distance -- into one eye's image.
//
// This exists because the 3 m marker ring was not a valid reference and the first
// version of the overlay test was invalid because of it. A ring at finite distance is
// only a level line while the eye sits at its centre; the moment the head moves off
// centre the near markers rise and the far ones fall and the line tilts, which reads
// exactly like the distortion being hunted. At infinity there is no parallax, so the
// eye's POSITION drops out of the maths entirely and the result depends on nothing but
// orientation and fov -- which is the only thing worth comparing against the game.
// Project a point in the reference space into one eye's image. Uses the four fov
// tangents rather than a symmetric half-angle, so it stays correct on a runtime whose
// frustum is not centred.
bool projectToEye(const XrView& view, const Marker& marker, uint32_t width,
                  uint32_t height, LONG halfSize, D3D11_RECT& rect) {
    const float relative[3] = {marker.x - view.pose.position.x,
                               marker.y - view.pose.position.y,
                               marker.z - view.pose.position.z};
    float local[3];
    rotateByInverse(view.pose.orientation, relative, local);

    if (local[2] > -0.05f) return false;  // behind the eye, or on the near plane
    const float invDepth = -1.0f / local[2];
    const float tanX = local[0] * invDepth;
    const float tanY = local[1] * invDepth;

    const float left = std::tan(view.fov.angleLeft);
    const float right = std::tan(view.fov.angleRight);
    const float down = std::tan(view.fov.angleDown);
    const float up = std::tan(view.fov.angleUp);

    const float u = (tanX - left) / (right - left);
    const float v = (tanY - down) / (up - down);
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;

    const LONG centreX = static_cast<LONG>(u * static_cast<float>(width));
    // Image rows run top-down while v runs bottom-up.
    const LONG centreY = static_cast<LONG>((1.0f - v) * static_cast<float>(height));

    rect.left = centreX - halfSize;
    rect.top = centreY - halfSize;
    rect.right = centreX + halfSize;
    rect.bottom = centreY + halfSize;
    if (rect.left < 0) rect.left = 0;
    if (rect.top < 0) rect.top = 0;
    if (rect.right > static_cast<LONG>(width)) rect.right = static_cast<LONG>(width);
    if (rect.bottom > static_cast<LONG>(height))
        rect.bottom = static_cast<LONG>(height);
    return rect.left < rect.right && rect.top < rect.bottom;
}

const char* sessionStateName(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_IDLE:         return "IDLE";
        case XR_SESSION_STATE_READY:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        default:                            return "UNKNOWN";
    }
}

// The size one eye is rendered at, submitted at, and now also RASTERIZED at.
//
// One function, because there are three consumers of this number and they have to agree
// exactly: the swapchain, the requirements published to the game, and -- once the game
// renders off-screen -- the game's own render target. Computing it twice from the same
// inputs works right up until one copy is edited.
//
// The ceiling is the runtime's own maximum, reported beside the recommendation by
// xrEnumerateViewConfigurationViews. The old hard-coded 1.0 was wrong twice: arbitrary,
// against 3172 recommended and 8192 max here; and protecting nothing, because a
// swapchain above the maximum is refused by xrCreateSwapchain anyway.
void targetEyeSize(const XrViewConfigurationView& view, float renderScale,
                   uint32_t& width, uint32_t& height, bool& clamped) {
    const float scale = renderScale > 0.05f ? renderScale : 1.0f;

    const uint32_t askedW = static_cast<uint32_t>(view.recommendedImageRectWidth * scale);
    const uint32_t askedH =
        static_cast<uint32_t>(view.recommendedImageRectHeight * scale);

    width = askedW;
    height = askedH;
    if (view.maxImageRectWidth && width > view.maxImageRectWidth)
        width = view.maxImageRectWidth;
    if (view.maxImageRectHeight && height > view.maxImageRectHeight)
        height = view.maxImageRectHeight;
    if (width < 64) width = 64;
    if (height < 64) height = 64;

    clamped = width != askedW || height != askedH;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const Options options = parseCommandLine(argc, argv);
    logOpen("gplvr-xr.log");
    logEcho(options.presentInstance == nullptr);
    logf("gplvr-xr -- %u-bit, %s", (unsigned)(sizeof(void*) * 8),
         options.presentInstance ? "presenting for the game" : "OpenXR survey");

    Consumer consumer;
    const bool presenting =
        options.presentInstance && consumer.open(options.presentInstance);
    if (options.presentInstance && !presenting) {
        logf("xr: stopping -- launched to present, but the game's control block is not "
             "there");
        logClose();
        return 1;
    }

    if (options.runtimeJson) {
        // Set it BOTH ways, and this is not belt and braces.
        //
        // SetEnvironmentVariable writes the Win32 block; _wputenv_s writes the CRT's
        // own copy, which is snapshotted at startup and does NOT track Win32 changes.
        // The OpenXR loader reads this variable through the CRT in some builds and
        // through Win32 in others, so setting only one can leave the override silently
        // ignored -- which looks exactly like passing no --runtime at all.
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", options.runtimeJson);
        _wputenv_s(L"XR_RUNTIME_JSON", options.runtimeJson);

        wchar_t readback[1024] = {};
        const DWORD got =
            GetEnvironmentVariableW(L"XR_RUNTIME_JSON", readback, 1024);
        logf("xr: forcing XR_RUNTIME_JSON = %S for this process only%s",
             options.runtimeJson,
             (got && !wcscmp(readback, options.runtimeJson))
                 ? ""
                 : "  <-- READ BACK DIFFERENT, the override did not take");

        DWORD attrs = GetFileAttributesW(options.runtimeJson);
        if (attrs == INVALID_FILE_ATTRIBUTES)
            logf("xr: WARNING -- that runtime manifest does not exist; the loader will "
                 "fall back to the registry's ActiveRuntime and you will get SteamVR");
    }

    xrLogRuntimeSelection();

    if (!xrLoadLoader(g_api, nullptr)) {
        logf("xr: stopping -- the loader did not come up");
        logClose();
        return 1;
    }

    logApiLayers();
    const std::vector<XrExtensionProperties> extensions = enumerateExtensions();

    const bool d3d11 = hasExtension(extensions, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    const bool opengl = hasExtension(extensions, "XR_KHR_opengl_enable");
    logf("xr: XR_KHR_D3D11_enable=%s  XR_KHR_opengl_enable=%s",
         d3d11 ? "yes" : "NO", opengl ? "yes" : "no");
    // Worth stating plainly: GPL renders through roglv2's OpenGL context, so an
    // OpenGL-capable runtime would allow an in-process path with no texture handoff at
    // all. Most PC runtimes are D3D-only, which is why the plan assumes D3D11.

    // The display clock as QueryPerformanceCounter values, which the game process can
    // compare against its own: what position prediction runs on. Optional -- without it
    // the game renders from the views exactly as they were published.
    bool        qpcTime = hasExtension(extensions, kQpcTimeExtension);
    bool        refreshRateListed =
        hasExtension(extensions, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    const char* enabledExtensions[3] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    uint32_t    enabledCount = 1;
    if (qpcTime) enabledExtensions[enabledCount++] = kQpcTimeExtension;
    if (refreshRateListed)
        enabledExtensions[enabledCount++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "gplvr survey");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "gplvr");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = kRequestedApiVersion;
    if (d3d11) {
        createInfo.enabledExtensionCount = enabledCount;
        createInfo.enabledExtensionNames = enabledExtensions;
        g_refreshRateEnabled = refreshRateListed;
    }

    if (!XR_CHECK(g_api.xrCreateInstance(&createInfo, &g_instance))) {
        logf("xr: stopping -- no instance");
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }
    xrResolveInstanceFunctions(g_api, g_instance);

    ConvertTimeToQpcFn convertTimeToQpc = nullptr;
    XrResult           qpcResolve = XR_SUCCESS;
    if (qpcTime && d3d11) {
        PFN_xrVoidFunction function = nullptr;
        qpcResolve = g_api.getInstanceProcAddr(
            g_instance, "xrConvertTimeToWin32PerformanceCounterKHR", &function);
        if (XR_SUCCEEDED(qpcResolve))
            convertTimeToQpc = reinterpret_cast<ConvertTimeToQpcFn>(function);
    }
    LARGE_INTEGER qpcFrequency = {};
    QueryPerformanceFrequency(&qpcFrequency);
    if (convertTimeToQpc)
        logf("xr: %s -- enabled; the display clock is published for position prediction",
             kQpcTimeExtension);
    else
        logf("xr: %s -- NOT available (listed %s, d3d11 %s, xrGetInstanceProcAddr XrResult "
             "%d); the game renders from the views unpredicted",
             kQpcTimeExtension, qpcTime ? "yes" : "no", d3d11 ? "yes" : "no",
             (int)qpcResolve);

    XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_CHECK(g_api.xrGetInstanceProperties(g_instance, &instanceProperties)))
        logf("xr: runtime \"%s\" version %u.%u.%u", instanceProperties.runtimeName,
             (unsigned)XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
             (unsigned)XR_VERSION_MINOR(instanceProperties.runtimeVersion),
             (unsigned)XR_VERSION_PATCH(instanceProperties.runtimeVersion));

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (!XR_CHECK(g_api.xrGetSystem(g_instance, &systemInfo, &system))) {
        logf("xr: no HMD system -- headset off, not connected, or this runtime has no "
             "%u-bit implementation", (unsigned)(sizeof(void*) * 8));
        g_api.xrDestroyInstance(g_instance);
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }

    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_CHECK(g_api.xrGetSystemProperties(g_instance, system, &systemProperties))) {
        logf("xr: system \"%s\" vendor=%u", systemProperties.systemName,
             (unsigned)systemProperties.vendorId);
        logf("xr: max swapchain %ux%u, %u layers; orientation tracking=%s, position "
             "tracking=%s",
             (unsigned)systemProperties.graphicsProperties.maxSwapchainImageWidth,
             (unsigned)systemProperties.graphicsProperties.maxSwapchainImageHeight,
             (unsigned)systemProperties.graphicsProperties.maxLayerCount,
             systemProperties.trackingProperties.orientationTracking ? "yes" : "no",
             systemProperties.trackingProperties.positionTracking ? "yes" : "no");
    }

    // -- view configuration ------------------------------------------------------
    uint32_t viewCount = 0;
    XrViewConfigurationView viewConfigs[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                              {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    const XrViewConfigurationType viewType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    XrViewConfigurationProperties viewProperties{
        XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
    if (XR_CHECK(g_api.xrGetViewConfigurationProperties(g_instance, system, viewType,
                                                        &viewProperties)))
        logf("xr: primary stereo view configuration, fovMutable=%s%s",
             viewProperties.fovMutable ? "yes" : "no",
             viewProperties.fovMutable
                 ? ""
                 : "  <-- the runtime does NOT allow the application to change the "
                   "fov, so a submitted fov is ignored rather than honoured");
        g_fovMutable = viewProperties.fovMutable != 0;

    if (!XR_CHECK(g_api.xrEnumerateViewConfigurationViews(g_instance, system, viewType,
                                                          2, &viewCount, viewConfigs)) ||
        viewCount != 2) {
        logf("xr: expected 2 stereo views, got %u -- stopping", viewCount);
        g_api.xrDestroyInstance(g_instance);
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }

    for (uint32_t i = 0; i < viewCount; ++i)
        logf("xr: view %u recommended %ux%u (max %ux%u), %u samples recommended",
             i, (unsigned)viewConfigs[i].recommendedImageRectWidth,
             (unsigned)viewConfigs[i].recommendedImageRectHeight,
             (unsigned)viewConfigs[i].maxImageRectWidth,
             (unsigned)viewConfigs[i].maxImageRectHeight,
             (unsigned)viewConfigs[i].recommendedSwapchainSampleCount);

    uint32_t blendModeCount = 0;
    XrEnvironmentBlendMode blendModes[8] = {};
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (XR_CHECK(g_api.xrEnumerateEnvironmentBlendModes(
            g_instance, system, viewType, 8, &blendModeCount, blendModes)) &&
        blendModeCount) {
        blendMode = blendModes[0];
        for (uint32_t i = 0; i < blendModeCount; ++i)
            logf("xr: blend mode %s%s", blendModeName(blendModes[i]),
                 i == 0 ? "  <-- using this" : "");
    }

    if (!options.session || !d3d11) {
        logf("xr: survey complete; no session was created (%s)",
             options.session ? "the runtime has no D3D11 extension" : "--no-session");
        g_api.xrDestroyInstance(g_instance);
        xrUnloadLoader(g_api);
        logClose();
        return 0;
    }

    // -- D3D11 device on the adapter the runtime named ---------------------------
    XrGraphicsRequirementsD3D11KHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!g_api.xrGetD3D11GraphicsRequirementsKHR ||
        !XR_CHECK(g_api.xrGetD3D11GraphicsRequirementsKHR(g_instance, system,
                                                          &requirements))) {
        logf("xr: no D3D11 graphics requirements -- stopping before the session");
        g_api.xrDestroyInstance(g_instance);
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }
    logf("xr: D3D11 requirements: adapter luid=%08lX:%08lX minFeatureLevel=0x%04X",
         (unsigned long)requirements.adapterLuid.HighPart,
         (unsigned long)requirements.adapterLuid.LowPart,
         (unsigned)requirements.minFeatureLevel);

    IDXGIAdapter1* adapter = findAdapter(requirements.adapterLuid);
    if (!adapter) {
        logf("xr: no DXGI adapter matches the runtime's LUID -- stopping");
        if (presenting) consumer.setError(1);
        g_api.xrDestroyInstance(g_instance);
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }

    // Publish the adapter we ACTUALLY chose, not what the runtime asked for.
    //
    // Those differ whenever the runtime names no adapter: it reports a zero LUID, we
    // pick the first one, and the game has to create its shared textures on that same
    // card. Publishing the zero would send it looking for an adapter that does not
    // exist and it would fail exactly where we just stopped failing.
    DXGI_ADAPTER_DESC1 chosen = {};
    adapter->GetDesc1(&chosen);

    // The game cannot create its shared textures until it knows this LUID -- a texture
    // shared from a device on another adapter cannot be opened here at all -- so tell it
    // now, before we spend time on our own device and session.
    if (presenting) {
        // Before the requirements, so the game knows the fov rule from its very first
        // tracked frame rather than scaling once and being corrected afterwards.
        consumer.publishFovMutable(g_fovMutable);
        uint32_t targetW = 0;
        uint32_t targetH = 0;
        bool     targetClamped = false;
        targetEyeSize(viewConfigs[0], options.renderScale, targetW, targetH,
                      targetClamped);
        if (targetClamped)
            logf("xr: vrRenderScale %.2f asked for more than the runtime allows -- "
                 "clamped to its maximum of %ux%u",
                 options.renderScale, (unsigned)viewConfigs[0].maxImageRectWidth,
                 (unsigned)viewConfigs[0].maxImageRectHeight);
        consumer.publishRequirements(chosen.AdapterLuid, targetW, targetH);
    }

    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL    levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL    level = D3D_FEATURE_LEVEL_11_0;
    HRESULT              created = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    adapter->Release();
    if (FAILED(created)) {
        logf("xr: D3D11CreateDevice failed (hr=0x%08lX)", (unsigned long)created);
        g_api.xrDestroyInstance(g_instance);
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }
    logf("xr: D3D11 device created at feature level 0x%04X", (unsigned)level);

    // ClearView takes rectangles, so a sweeping bar needs no shader, no vertex buffer
    // and no pipeline state -- which keeps this tool free of anything that could fail
    // for a reason unrelated to OpenXR.
    ID3D11DeviceContext1* context1 = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&context1);

    // -- session -----------------------------------------------------------------
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = system;

    XrSession session = XR_NULL_HANDLE;
    if (!XR_CHECK(g_api.xrCreateSession(g_instance, &sessionInfo, &session))) {
        logf("xr: stopping -- no session");
        if (context1) context1->Release();
        context->Release();
        device->Release();
        g_api.xrDestroyInstance(g_instance);
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }
    logf("xr: session created");

    float refreshRate = queryRefreshRate(session);
    if (g_refreshRateEnabled && g_api.xrGetDisplayRefreshRateFB) {
        logf("xr: %s -- enabled; the display refresh rate query says %.2f Hz%s",
             XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, refreshRate,
             refreshRate > 0.0f ? "" : " (no rate: display timing falls back to the runtime's "
                                       "predictedDisplayPeriod)");
        logRefreshRates(session);
    } else {
        logf("xr: %s -- NOT available (listed %s, d3d11 %s); display timing comes from the "
             "runtime's predictedDisplayPeriod",
             XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, refreshRateListed ? "yes" : "no",
             d3d11 ? "yes" : "no");
        g_refreshRateEnabled = false;
    }
    // A driver can change the rate after the compositor started without sending the event, so
    // the query is repeated once a second for the first ten seconds and logged on change.
    DWORD refreshCheckStart = GetTickCount();
    DWORD refreshCheckLast = refreshCheckStart;
    int   refreshChecks = 0;
    bool  loggedFirstPeriod = false;
    bool  loggedPeriodMismatch = false;

    // -- reference space ---------------------------------------------------------
    uint32_t             spaceCount = 0;
    XrReferenceSpaceType spaceTypes[8] = {};
    bool                 stageAvailable = false;
    if (XR_CHECK(g_api.xrEnumerateReferenceSpaces(session, 8, &spaceCount, spaceTypes)))
        for (uint32_t i = 0; i < spaceCount; ++i) {
            const char* name = spaceTypes[i] == XR_REFERENCE_SPACE_TYPE_VIEW ? "VIEW"
                             : spaceTypes[i] == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL"
                             : spaceTypes[i] == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE"
                                                                              : "other";
            logf("xr: reference space %s", name);
            stageAvailable = stageAvailable ||
                             spaceTypes[i] == XR_REFERENCE_SPACE_TYPE_STAGE;
        }

    // LOCAL, not STAGE.
    //
    // Measured, and not what I assumed: LOCAL is *not* origin-at-the-head here. SteamVR
    // reports head y around 1.09 to 1.16 m in LOCAL, so its origin sits on the floor
    // much as STAGE's does, and picking LOCAL does not on its own spare a seated driver
    // from being placed a standing height above the seat.
    //
    // LOCAL is still the right choice -- it is gravity-aligned and does not move when
    // the play area is redrawn -- but the seat has to come from a recentre: capture the
    // head pose at that moment and treat it as the origin. That belongs in Phase 3;
    // this tool only needs a stable space to locate views in.
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace appSpace = XR_NULL_HANDLE;
    XR_CHECK(g_api.xrCreateReferenceSpace(session, &spaceInfo, &appSpace));
    logf("xr: using LOCAL space (STAGE %s available)",
         stageAvailable ? "is also" : "is not");

    XrSpace viewSpace = XR_NULL_HANDLE;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(g_api.xrCreateReferenceSpace(session, &spaceInfo, &viewSpace));

    // -- motion controllers ------------------------------------------------------
    //
    // Here rather than in the frame loop: xrAttachSessionActionSets may be called once per
    // session and must precede the first xrSyncActions. A refusal is not fatal -- the
    // headset still works, the hands simply never appear -- and XrInput::create has
    // already said which step said no.
    XrInput controllers;
    controllers.create(g_api, g_instance, session);

    // -- swapchains --------------------------------------------------------------
    uint32_t formatCount = 0;
    XR_CHECK(g_api.xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount ? formatCount : 1, 0);
    XR_CHECK(g_api.xrEnumerateSwapchainFormats(session, formatCount, &formatCount,
                                               formats.data()));

    int64_t chosenFormat = 0;
    for (uint32_t i = 0; i < formatCount; ++i)
        logf("xr: swapchain format %lld (%s)", (long long)formats[i],
             dxgiFormatName(formats[i]));
    // Prefer an sRGB format: GPL's 8-bit output is sRGB-encoded, which is what such a
    // swapchain declares its contents to be. How the pixels get in unchanged is
    // swapchainViewFormat's business.
    const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                 DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DXGI_FORMAT_B8G8R8A8_UNORM};
    for (int64_t want : preferred) {
        for (uint32_t i = 0; i < formatCount && !chosenFormat; ++i)
            if (formats[i] == want) chosenFormat = want;
        if (chosenFormat) break;
    }
    if (!chosenFormat && formatCount) chosenFormat = formats[0];
    logf("xr: using swapchain format %lld (%s)", (long long)chosenFormat,
         dxgiFormatName(chosenFormat));

    Eye eyes[2];
    bool swapchainsOk = true;
    for (uint32_t i = 0; i < 2 && swapchainsOk; ++i) {
        XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.format = chosenFormat;
        swapchainInfo.sampleCount = 1;
        // Scale the runtime's RECOMMENDED size, and clamp against the one number that
        // is a real limit: the MAX it also reports.
        //
        // The old ceiling was a hard-coded 1.0, which was wrong twice. It was arbitrary
        // -- xrEnumerateViewConfigurationViews returns recommended AND max side by side,
        // and this runtime reports 3172 recommended against 8192 max, so there is 2.6x of
        // advertised headroom the option refused to reach. And it protected nothing: a
        // swapchain larger than the max is refused by xrCreateSwapchain, which the clamp
        // below now prevents properly.
        //
        // Above 1.0 buys little while the source is a 1280x1440 game half -- there is no
        // detail up there to resolve, only encode time and link bandwidth on a streamed
        // headset. It is worth having anyway: the compositor's own distortion warp
        // resamples what we hand it, and once the source resolution rises the ceiling
        // should not be the thing in the way.
        uint32_t width = 0;
        uint32_t height = 0;
        bool     clamped = false;
        targetEyeSize(viewConfigs[i], options.renderScale, width, height, clamped);

        swapchainInfo.width = width;
        swapchainInfo.height = height;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        eyes[i].width = swapchainInfo.width;
        eyes[i].height = swapchainInfo.height;
        if (!XR_CHECK(g_api.xrCreateSwapchain(session, &swapchainInfo,
                                              &eyes[i].swapchain))) {
            swapchainsOk = false;
            break;
        }

        uint32_t imageCount = 0;
        XR_CHECK(g_api.xrEnumerateSwapchainImages(eyes[i].swapchain, 0, &imageCount,
                                                  nullptr));
        std::vector<XrSwapchainImageD3D11KHR> images(
            imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_CHECK(g_api.xrEnumerateSwapchainImages(
            eyes[i].swapchain, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));

        DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        for (uint32_t image = 0; image < imageCount; ++image) {
            ID3D11RenderTargetView* target = nullptr;
            D3D11_RENDER_TARGET_VIEW_DESC desc = {};
            viewFormat = swapchainViewFormat(images[image].texture, chosenFormat,
                                             eyes[i].decodeSrgb, textureFormat);
            desc.Format = viewFormat;
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateRenderTargetView(images[image].texture, &desc,
                                                      &target))) {
                logf("xr: CreateRenderTargetView failed for eye %u image %u", i, image);
                swapchainsOk = false;
                break;
            }
            eyes[i].targets.push_back(target);
        }
        logf("xr: eye %u swapchain %ux%u, %u images -- image format %d, drawn through %s%s", i,
             eyes[i].width, eyes[i].height, imageCount, (int)textureFormat,
             dxgiFormatName(viewFormat),
             eyes[i].decodeSrgb ? " with an sRGB decode in the blit" : "");
    }

    if (presenting && !consumer.initialise(device, context)) {
        logf("xr: the blit pipeline would not build -- stopping");
        consumer.setError(2);
        swapchainsOk = false;
    }

    HudLayer hud;
    // The VR menu's layer. Its own swapchain and its own seat space, because the seat
    // space is rebuilt on a recentre and the two layers must not share one object whose
    // lifetime either of them can end.
    HudLayer menu;

    // -- frame loop --------------------------------------------------------------
    XrSessionState state = XR_SESSION_STATE_IDLE;
    bool           running = false;
    bool           exitLoop = !swapchainsOk;
    bool           exitRequested = false;
    DWORD          exitDeadline = 0;
    bool           loggedFov = false;

    const DWORD startTick = GetTickCount();
    DWORD       lastReport = startTick;
    unsigned    framesThisSecond = 0;

    // Pose diagnostics, reported with the cadence line. Every published view set is kept
    // for eight frames; each submitted image's rendered pose is looked up among them BIT FOR
    // BIT, which says how many displays old it is and proves it came from us. The angle is
    // how far the head turned between that pose and this display; the unexplained part is
    // what the head's own angular speed over that age does not account for. Unmatched poses
    // or a large unexplained angle mean the image and its pose disagree; neither means the
    // round trip is exact and any judder is the runtime's or the stream's.
    struct PoseSample {
        XrTime        time = 0;
        XrQuaternionf orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        float         angularSpeed = 0.0f;  // rad/s
    };
    PoseSample poseRing[8];
    unsigned   poseRingNext = 0;
    unsigned   poseAge[5] = {};  // 0, 1, 2, 3+ displays old, unmatched
    unsigned   poseRepeats = 0;
    unsigned   poseSamples = 0;
    int32_t    poseLastSequence = 0;
    double     poseAngleSum = 0.0;
    double     poseAngleMax = 0.0;
    double     poseResidualSum = 0.0;
    double     poseResidualMax = 0.0;
    // Presenter cadence within each one-second report: the longest gap between two
    // xrWaitFrame returns, how many gaps skipped a display, and the longest stretch from a
    // wait's return to its xrEndFrame. A per-second frame count hides a late frame.
    LONGLONG lastWaitReturn = 0;
    LONGLONG worstWaitGap = 0;
    LONGLONG worstFrameWork = 0;
    unsigned skippedDisplays = 0;

    SetConsoleCtrlHandler(consoleControlHandler, TRUE);
    logf("xr: entering the frame loop for %d s -- press Esc%s to stop early",
         options.seconds, presenting ? "" : " or Ctrl+C");

    while (!exitLoop) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (g_api.xrPollEvent(g_instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                state = changed->state;
                logf("xr: session state -> %s", sessionStateName(state));

                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = viewType;
                    running = XR_CHECK(g_api.xrBeginSession(session, &beginInfo));
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    running = false;
                    XR_CHECK(g_api.xrEndSession(session));
                } else if (state == XR_SESSION_STATE_EXITING ||
                           state == XR_SESSION_STATE_LOSS_PENDING) {
                    exitLoop = true;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                logf("xr: instance loss pending");
                exitLoop = true;
            } else if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
                const auto* changed =
                    reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
                float queried = queryRefreshRate(session);
                logf("xr: display refresh rate changed %.2f -> %.2f Hz (event); the query now "
                     "says %.2f Hz",
                     changed->fromDisplayRefreshRate, changed->toDisplayRefreshRate, queried);
                if (queried > 0.0f)
                    refreshRate = queried;
                else if (g_refreshRateEnabled && changed->toDisplayRefreshRate > 0.0f)
                    refreshRate = changed->toDisplayRefreshRate;
                loggedPeriodMismatch = false;
            }
            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!running) {
            Sleep(10);
        } else {
            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState    frameState{XR_TYPE_FRAME_STATE};
            if (!XR_CHECK(g_api.xrWaitFrame(session, &waitInfo, &frameState))) break;
            LARGE_INTEGER waitReturn = {};
            QueryPerformanceCounter(&waitReturn);

            if (g_refreshRateEnabled && refreshChecks < 10 &&
                GetTickCount() - refreshCheckLast >= 1000) {
                refreshCheckLast = GetTickCount();
                ++refreshChecks;
                float queried = queryRefreshRate(session);
                if (queried > 0.0f && std::fabs(queried - refreshRate) > 0.01f) {
                    logf("xr: display refresh rate is now %.2f Hz (was %.2f), seen by the "
                         "once-a-second query %.1f s after the session was created",
                         queried, refreshRate, (refreshCheckLast - refreshCheckStart) / 1000.0);
                    refreshRate = queried;
                    loggedPeriodMismatch = false;
                }
            }

            // The period every display-time calculation here uses: one refresh at the queried
            // rate, else the runtime's own frame period. xrEndFrame still gets the runtime's
            // predictedDisplayTime.
            XrDuration runtimePeriod = frameState.predictedDisplayPeriod;
            XrDuration displayPeriod = runtimePeriod;
            if (refreshRate > 0.0f)
                displayPeriod = static_cast<XrDuration>(1.0e9 / refreshRate + 0.5);
            double runtimeHz = runtimePeriod > 0 ? 1.0e9 / static_cast<double>(runtimePeriod) : 0.0;
            if (!loggedFirstPeriod) {
                loggedFirstPeriod = true;
                logf("xr: first frame -- the runtime's predictedDisplayPeriod is %.3f ms "
                     "(%.2f Hz); the display refresh rate query says %.2f Hz%s",
                     runtimePeriod / 1.0e6, runtimeHz, refreshRate,
                     refreshRate > 0.0f ? "" : " (none: the runtime's period is used)");
            }
            if (refreshRate > 0.0f && runtimeHz > 0.0 && !loggedPeriodMismatch &&
                std::fabs(runtimeHz - refreshRate) > 0.02 * refreshRate) {
                loggedPeriodMismatch = true;
                logf("xr: the runtime's frame timing says %.2f Hz but the display refresh rate "
                     "is %.2f Hz -- using %.2f",
                     runtimeHz, refreshRate, refreshRate);
            }

            if (lastWaitReturn) {
                LONGLONG gap = waitReturn.QuadPart - lastWaitReturn;
                if (gap > worstWaitGap) worstWaitGap = gap;
                LONGLONG period = displayPeriod * qpcFrequency.QuadPart / 1000000000LL;
                if (period > 0 && gap * 2 > period * 3) ++skippedDisplays;
            }
            lastWaitReturn = waitReturn.QuadPart;

            XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
            if (!XR_CHECK(g_api.xrBeginFrame(session, &beginInfo))) break;

            // The controllers, at the same predicted display time the views below are
            // located for, and in the same appSpace -- so a hand and an eye read out of
            // one published sample belong to one instant in one frame of reference.
            //
            // Published every frame, including the frames the runtime says not to render:
            // a menu open over the game is exactly such a frame, and it is a frame where
            // the driver is most likely to be pressing something.
            if (controllers.attached()) {
                SharedInput input;
                controllers.sample(g_api, g_instance, session, appSpace,
                                   frameState.predictedDisplayTime,
                                   state == XR_SESSION_STATE_FOCUSED, input);
                consumer.publishInput(input);

                // What the runtime calls each bound control, asked only when a hand's
                // interaction profile has changed -- which the sample above detects.
                SharedInputNames names;
                if (controllers.takeChangedNames(g_api, g_instance, session, names))
                    consumer.publishInputNames(names);
            }

            XrCompositionLayerProjectionView projectionViews[2] = {};
            XrCompositionLayerProjection     layer{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            XrCompositionLayerQuad              hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            XrCompositionLayerQuad              menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            // Three: the projection, pribluda's panel and the VR menu. Layers are painted
            // in submission order, so the menu goes last and sits over both.
            const XrCompositionLayerBaseHeader* layers[3] = {};
            uint32_t layerCount = 0;

            if (frameState.shouldRender) {
                XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
                locateInfo.viewConfigurationType = viewType;
                locateInfo.displayTime = frameState.predictedDisplayTime;
                locateInfo.space = appSpace;

                XrViewState viewState{XR_TYPE_VIEW_STATE};
                XrView      views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                uint32_t    located = 0;
                const bool  ok = XR_CHECK(g_api.xrLocateViews(
                    session, &locateInfo, &viewState, 2, &located, views));
                const bool valid =
                    ok && located == 2 &&
                    (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                    (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);

                if (valid && !loggedFov) {
                    loggedFov = true;
                    // Measure the asymmetry rather than assert it. An earlier version
                    // of this line claimed the frustum is always asymmetric and that a
                    // symmetric fov could not express it; both halves are wrong.
                    // SteamVR reports a perfectly symmetric frustum for a Pico 4, and
                    // gpl_cameraCreate's cx/cy shift the principal point, which is
                    // exactly how an asymmetric one would be expressed anyway.
                    constexpr float kDegrees = 57.29578f;
                    for (uint32_t i = 0; i < 2; ++i) {
                        const XrFovf& fov = views[i].fov;
                        const float   skewH = fov.angleRight + fov.angleLeft;
                        const float   skewV = fov.angleUp + fov.angleDown;
                        const bool    symmetric =
                            std::fabs(skewH) < 1e-4f && std::fabs(skewV) < 1e-4f;
                        logf("xr: eye %u fov L%.4f R%.4f U%.4f D%.4f rad = %.1f x %.1f "
                             "deg -- %s",
                             i, fov.angleLeft, fov.angleRight, fov.angleUp,
                             fov.angleDown, (fov.angleRight - fov.angleLeft) * kDegrees,
                             (fov.angleUp - fov.angleDown) * kDegrees,
                             symmetric
                                 ? "symmetric, so cameraCreate(fov, cx=0, cy=0) is exact"
                                 : "asymmetric; cameraCreate's cx/cy carry the offset");
                    }
                    // Both eye offsets in one line. This IS the eye separation the game
                    // renders with -- it is published straight through, so a wrong
                    // number here is a wrong world scale in the headset.
                    const float dx = views[1].pose.position.x - views[0].pose.position.x;
                    const float dy = views[1].pose.position.y - views[0].pose.position.y;
                    const float dz = views[1].pose.position.z - views[0].pose.position.z;
                    logf("xr: runtime IPD %.4f m (eye separation %.4f, %.4f, %.4f)",
                         std::sqrt(dx * dx + dy * dy + dz * dz), dx, dy, dz);
                }

                // The fov can change per frame too, so watch it for the same reason.
                //
                // xrLocateViews may return a different frustum at any time -- dynamic
                // foveation, an IPD motor moving, or simply the first reading being
                // taken before the session settled. The RENDER path already follows it
                // (vrAcquireEyes uses whatever the presenter last published); it was
                // only the log that fired once and could therefore describe a frustum
                // we had long stopped using.
                if (valid) {
                    static float lastFov[2][4] = {};
                    static bool  haveLastFov = false;
                    const float now[2][4] = {
                        {views[0].fov.angleLeft, views[0].fov.angleRight,
                         views[0].fov.angleUp, views[0].fov.angleDown},
                        {views[1].fov.angleLeft, views[1].fov.angleRight,
                         views[1].fov.angleUp, views[1].fov.angleDown}};
                    bool moved = false;
                    if (haveLastFov)
                        for (int e = 0; e < 2 && !moved; ++e)
                            for (int k = 0; k < 4 && !moved; ++k)
                                moved = std::fabs(now[e][k] - lastFov[e][k]) > 0.0005f;
                    if (moved) {
                        constexpr float kDeg = 57.29578f;
                        for (int e = 0; e < 2; ++e)
                            logf("xr: eye %d fov CHANGED to L%.4f R%.4f U%.4f D%.4f rad "
                                 "= %.1f x %.1f deg",
                                 e, now[e][0], now[e][1], now[e][2], now[e][3],
                                 (now[e][1] - now[e][0]) * kDeg,
                                 (now[e][2] - now[e][3]) * kDeg);
                    }
                    if (moved || !haveLastFov) {
                        std::memcpy(lastFov, now, sizeof(lastFov));
                        haveLastFov = true;
                    }
                }

                // Keep watching it, because the first reading is taken before the
                // session is SYNCHRONIZED and may be a placeholder rather than the
                // headset's real optics.
                //
                // PicoStreamingXR's first report was 0.0710 exactly, with y and z both
                // 0.0000, while the headset itself is set to 65 mm. Numbers that clean
                // are what a default looks like. If this line never fires, 71 mm is
                // what the runtime genuinely wants us to render and the disagreement
                // with the headset setting is real; if it fires and settles elsewhere,
                // the first reading was simply too early to trust.
                if (valid) {
                    // Recomputed here: the originals are scoped to the one-shot block
                    // above and are not in scope on later frames.
                    const float sx = views[1].pose.position.x - views[0].pose.position.x;
                    const float sy = views[1].pose.position.y - views[0].pose.position.y;
                    const float sz = views[1].pose.position.z - views[0].pose.position.z;
                    const float ipd = std::sqrt(sx * sx + sy * sy + sz * sz);
                    static float reportedIpd = 0.0f;
                    if (reportedIpd == 0.0f) reportedIpd = ipd;
                    else if (std::fabs(ipd - reportedIpd) > 0.0005f) {
                        logf("xr: runtime IPD CHANGED %.4f -> %.4f m (separation %.4f, "
                             "%.4f, %.4f) -- the earlier value was taken before the "
                             "session settled",
                             reportedIpd, ipd, sx, sy, sz);
                        reportedIpd = ipd;
                    }
                }

                if (valid && !g_markersPlaced) {
                    placeMarkers(views[0]);
                    logf("xr: placed %d world-locked markers on a 3 m ring at eye "
                         "height -- turn your head and they should sweep past the grey "
                         "box in the centre, which is head-locked and never moves",
                         kMarkerCount);
                }

                if (valid && presenting) {
                    // Pick up texture recreation -- the game does that at every
                    // menu-to-sim transition -- and latch the newest published frame,
                    // once per frame rather than once per eye. The latch instant goes to
                    // the game: a frame it publishes after this is shown at a later display.
                    LARGE_INTEGER latchQpc = {};
                    QueryPerformanceCounter(&latchQpc);
                    consumer.update();

                    // While the game paces its frames, a frame rendered from these views is
                    // latched `lead` displays after this one, so the views are located for
                    // that display. The projection layer still carries the pose each image was
                    // rendered from, and the runtime reprojects whatever prediction missed.
                    int32_t       lead = consumer.viewLeadDisplays();
                    XrView        leadViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                    const XrView* publish = views;
                    XrTime        publishTime = frameState.predictedDisplayTime;
                    if (lead > 0 && displayPeriod > 0) {
                        XrViewLocateInfo leadInfo = locateInfo;
                        leadInfo.displayTime = publishTime + lead * displayPeriod;
                        XrViewState leadState{XR_TYPE_VIEW_STATE};
                        uint32_t    leadLocated = 0;
                        if (XR_SUCCEEDED(g_api.xrLocateViews(session, &leadInfo, &leadState, 2,
                                                             &leadLocated, leadViews)) &&
                            leadLocated == 2 &&
                            (leadState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                            (leadState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
                            publish = leadViews;
                            publishTime = leadInfo.displayTime;
                        } else {
                            lead = 0;
                        }
                    }
                    static int32_t loggedLead = 0;
                    static int     leadReports = 16;
                    if (lead != loggedLead && leadReports > 0) {
                        --leadReports;
                        logf("xr: published views are located %d display(s) after the current "
                             "one%s%s",
                             lead, lead ? " -- the game is pacing its frames" : "",
                             leadReports ? "" : " (further changes not logged)");
                    }
                    loggedLead = lead;

                    SharedView shared[2] = {};
                    SharedPose head = {};
                    for (int eye = 0; eye < 2; ++eye) {
                        shared[eye].pose.orientationX = publish[eye].pose.orientation.x;
                        shared[eye].pose.orientationY = publish[eye].pose.orientation.y;
                        shared[eye].pose.orientationZ = publish[eye].pose.orientation.z;
                        shared[eye].pose.orientationW = publish[eye].pose.orientation.w;
                        shared[eye].pose.positionX = publish[eye].pose.position.x;
                        shared[eye].pose.positionY = publish[eye].pose.position.y;
                        shared[eye].pose.positionZ = publish[eye].pose.position.z;
                        shared[eye].fov.angleLeft = publish[eye].fov.angleLeft;
                        shared[eye].fov.angleRight = publish[eye].fov.angleRight;
                        shared[eye].fov.angleUp = publish[eye].fov.angleUp;
                        shared[eye].fov.angleDown = publish[eye].fov.angleDown;
                    }

                    // The head pose goes with them, at the same predicted time, or the
                    // recentre origin would be sampled from a different instant than the
                    // eyes it is subtracted from.
                    XrSpaceVelocity headVelocity{XR_TYPE_SPACE_VELOCITY};
                    XrSpaceLocation headLocation{XR_TYPE_SPACE_LOCATION};
                    headLocation.next = &headVelocity;
                    const bool headValid =
                        XR_SUCCEEDED(g_api.xrLocateSpace(viewSpace, appSpace, publishTime,
                                                         &headLocation)) &&
                        (headLocation.locationFlags &
                         XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
                    head.orientationX = headLocation.pose.orientation.x;
                    head.orientationY = headLocation.pose.orientation.y;
                    head.orientationZ = headLocation.pose.orientation.z;
                    head.orientationW = headLocation.pose.orientation.w;
                    head.positionX = headLocation.pose.position.x;
                    head.positionY = headLocation.pose.position.y;
                    head.positionZ = headLocation.pose.position.z;

                    // The head's motion and the display clock, for the game's position
                    // prediction: the latch and display instants of this frame pin the
                    // presenter's cadence, so the game can count which later display its own
                    // frame will reach.
                    SharedMotion motion;
                    if (headVelocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
                        motion.linearVelocity[0] = headVelocity.linearVelocity.x;
                        motion.linearVelocity[1] = headVelocity.linearVelocity.y;
                        motion.linearVelocity[2] = headVelocity.linearVelocity.z;
                        motion.flags |= kMotionLinear;
                    }
                    if (headVelocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
                        motion.angularVelocity[0] = headVelocity.angularVelocity.x;
                        motion.angularVelocity[1] = headVelocity.angularVelocity.y;
                        motion.angularVelocity[2] = headVelocity.angularVelocity.z;
                        motion.flags |= kMotionAngular;
                    }
                    LARGE_INTEGER displayQpc = {};
                    if (convertTimeToQpc && displayPeriod > 0 &&
                        XR_SUCCEEDED(convertTimeToQpc(g_instance, publishTime, &displayQpc))) {
                        storeQpc(motion.displayTime, displayQpc.QuadPart);
                        storeQpc(motion.latchTime, latchQpc.QuadPart);
                        storeQpc(motion.period,
                                 displayPeriod * qpcFrequency.QuadPart / 1000000000LL);
                        motion.flags |= kMotionTiming;
                    }
                    motion.viewLead = lead;
                    motion.refreshRate = refreshRate;

                    consumer.publishViews(shared, head, headValid, motion);

                    PoseSample& sample = poseRing[poseRingNext++ % 8];
                    sample.time = publishTime;
                    sample.orientation = publish[0].pose.orientation;
                    sample.angularSpeed =
                        (motion.flags & kMotionAngular)
                            ? std::sqrt(motion.angularVelocity[0] * motion.angularVelocity[0] +
                                        motion.angularVelocity[1] * motion.angularVelocity[1] +
                                        motion.angularVelocity[2] * motion.angularVelocity[2])
                            : 0.0f;
                }

                if (valid) {
                    const float seconds =
                        (GetTickCount() - startTick) / 1000.0f;
                    bool bothEyesReady = true;
                    for (uint32_t i = 0; i < 2; ++i) {
                        uint32_t imageIndex = 0;
                        XrSwapchainImageAcquireInfo acquireInfo{
                            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        if (!XR_CHECK(g_api.xrAcquireSwapchainImage(
                                eyes[i].swapchain, &acquireInfo, &imageIndex))) {
                            bothEyesReady = false;
                            break;
                        }

                        XrSwapchainImageWaitInfo imageWait{
                            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        imageWait.timeout = XR_INFINITE_DURATION;
                        XR_CHECK(g_api.xrWaitSwapchainImage(eyes[i].swapchain,
                                                            &imageWait));

                        // Left blue, right red, so a swapped-eye submission is obvious
                        // at a glance rather than something you have to reason about.
                        const float base[2][4] = {{0.04f, 0.09f, 0.32f, 1.0f},
                                                  {0.32f, 0.07f, 0.04f, 1.0f}};
                        ID3D11RenderTargetView* target = eyes[i].targets[imageIndex];

                        // When presenting, the game's frame replaces the flat colour --
                        // but only once it has published one. Until then the coloured
                        // eye stays, because a blue left eye and a red right eye say
                        // "waiting for the game" far more clearly than black does.
                        bool drawn = false;
                        if (presenting)
                            drawn = consumer.drawEye(target, eyes[i].width,
                                                     eyes[i].height,
                                                     static_cast<int>(i),
                                                     eyes[i].decodeSrgb);
                        if (!drawn) context->ClearRenderTargetView(target, base[i]);

                        if (!presenting && context1) {
                            const LONG marker =
                                static_cast<LONG>(eyes[i].width / 24);

                            // Head-locked: a box at the centre of the image, which by
                            // construction never moves. It is the fixed reference the
                            // world-locked markers are judged against.
                            const float grey[4] = {0.45f, 0.45f, 0.45f, 1.0f};
                            const LONG  cx = static_cast<LONG>(eyes[i].width / 2);
                            const LONG  cy = static_cast<LONG>(eyes[i].height / 2);
                            const LONG  thin = marker / 4 + 1;
                            const D3D11_RECT centre = {cx - thin, cy - thin, cx + thin,
                                                       cy + thin};
                            context1->ClearView(target, grey, &centre, 1);

                            // World-locked: eight markers on a ring. Turn your head and
                            // these sweep past the centre box. If they stay put
                            // relative to it, the pose is not reaching the projection.
                            const float bright[4] = {0.95f, 0.95f, 0.90f, 1.0f};
                            const float dim[4] = {0.55f, 0.75f, 0.55f, 1.0f};
                            for (const Marker& point : g_markers) {
                                D3D11_RECT rect = {};
                                if (!projectToEye(views[i], point, eyes[i].width,
                                                  eyes[i].height, marker, rect))
                                    continue;
                                context1->ClearView(target, point.bright ? bright : dim,
                                                    &rect, 1);
                            }

                            // And a thin bar sweeping once a second, low in the image,
                            // so "frames are arriving" stays readable independently of
                            // whether tracking works.
                            const float phase = seconds - std::floor(seconds);
                            const LONG  barWidth = static_cast<LONG>(eyes[i].width / 24);
                            const LONG  barLeft = static_cast<LONG>(
                                phase * static_cast<float>(eyes[i].width - barWidth));
                            const D3D11_RECT bar = {
                                barLeft, static_cast<LONG>(eyes[i].height * 7 / 8),
                                barLeft + barWidth, static_cast<LONG>(eyes[i].height)};
                            context1->ClearView(target, bright, &bar, 1);
                        }

                        // Submit this eye's work before telling the runtime its image is
                        // ready.
                        //
                        // Without this the draws sit in the immediate context's buffer.
                        // Releasing eye 0 can make the runtime flush; eye 1's draw is
                        // then issued after that flush and does not reach the GPU until
                        // the next frame's flush point -- so the right eye is composited
                        // one frame late, every frame. Tied to the swapchain index
                        // rather than the content, which is why swapping the eyes on the
                        // game side did not move it. BFVR flushes here for the same
                        // reason (SharedTextureConsumer.cpp).
                        context->Flush();

                        XrSwapchainImageReleaseInfo releaseInfo{
                            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        XR_CHECK(g_api.xrReleaseSwapchainImage(eyes[i].swapchain,
                                                               &releaseInfo));

                        projectionViews[i].type =
                            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                        projectionViews[i].pose = views[i].pose;
                        projectionViews[i].fov = views[i].fov;

                        // Submit the pose the image was DRAWN from, not the one we just
                        // located. The game renders at 36 Hz and we submit at 90, so a
                        // frame is up to 28 ms old by the time it reaches the
                        // compositor. Handing the runtime the current pose tells it the
                        // stale image is current, so it leaves it alone and the world
                        // ends up pinned to the head on every fast turn. Handing it the
                        // rendered pose is what lets it reproject, which is the whole
                        // reason 36 Hz into a 90 Hz compositor is survivable.
                        SharedView rendered[2] = {};
                        if (presenting && consumer.renderedViews(rendered)) {
                            const SharedView& source = rendered[i];
                            projectionViews[i].pose.orientation = {
                                source.pose.orientationX, source.pose.orientationY,
                                source.pose.orientationZ, source.pose.orientationW};
                            projectionViews[i].pose.position = {source.pose.positionX,
                                                                source.pose.positionY,
                                                                source.pose.positionZ};

                            projectionViews[i].fov = {
                                source.fov.angleLeft, source.fov.angleRight,
                                source.fov.angleUp, source.fov.angleDown};
                        }
                        projectionViews[i].subImage.swapchain = eyes[i].swapchain;
                        projectionViews[i].subImage.imageRect.offset = {0, 0};
                        projectionViews[i].subImage.imageRect.extent = {
                            (int32_t)eyes[i].width, (int32_t)eyes[i].height};
                        projectionViews[i].subImage.imageArrayIndex = 0;
                    }

                    SharedView submitted[2] = {};
                    if (presenting && bothEyesReady && consumer.renderedViews(submitted) &&
                        displayPeriod > 0) {
                        int32_t sequence = consumer.latchedSequence();
                        if (sequence == poseLastSequence) ++poseRepeats;
                        poseLastSequence = sequence;

                        const PoseSample* match = nullptr;
                        for (const PoseSample& candidate : poseRing) {
                            if (candidate.time &&
                                candidate.orientation.x == submitted[0].pose.orientationX &&
                                candidate.orientation.y == submitted[0].pose.orientationY &&
                                candidate.orientation.z == submitted[0].pose.orientationZ &&
                                candidate.orientation.w == submitted[0].pose.orientationW) {
                                match = &candidate;
                                break;
                            }
                        }
                        if (!match) {
                            ++poseAge[4];
                        } else {
                            double periods =
                                static_cast<double>(frameState.predictedDisplayTime -
                                                    match->time) /
                                static_cast<double>(displayPeriod);
                            long age = std::lround(periods);
                            ++poseAge[age <= 0 ? 0 : (age >= 3 ? 3 : age)];

                            const XrQuaternionf& now = views[0].pose.orientation;
                            double dot = std::fabs(now.x * submitted[0].pose.orientationX +
                                                   now.y * submitted[0].pose.orientationY +
                                                   now.z * submitted[0].pose.orientationZ +
                                                   now.w * submitted[0].pose.orientationW);
                            if (dot > 1.0) dot = 1.0;
                            constexpr double kDegrees = 57.29577951308232;
                            double angle = 2.0 * std::acos(dot) * kDegrees;
                            double expected = match->angularSpeed * kDegrees * periods *
                                              displayPeriod * 1e-9;
                            double residual = std::fabs(angle - expected);
                            poseAngleSum += angle;
                            poseResidualSum += residual;
                            if (angle > poseAngleMax) poseAngleMax = angle;
                            if (residual > poseResidualMax) poseResidualMax = residual;
                            ++poseSamples;
                        }
                    }

                    // Submitting a projection layer whose second view was never filled
                    // in would hand the runtime an identity pose for one eye. Drop the
                    // whole layer instead; xrEndFrame with no layers is legal.
                    if (bothEyesReady) {
                        layer.space = appSpace;
                        layer.viewCount = 2;
                        layer.views = projectionViews;
                        layers[layerCount++] =
                            reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
                    }
                }

                // Pribluda's overlay, composited over the game's image on a quad. Layers
                // are painted in submission order, so it comes after the projection.
                Consumer::HudPanel panel;
                bool hudOn = presenting && consumer.hudPanel(panel);
                int32_t hudLayerNow = hudOn ? panel.layer : 0;
                if (hudLayerNow != hud.loggedLayer) {
                    hud.loggedLayer = hudLayerNow;
                    if (hudOn)
                        logf("xr: HUD panel %s -- %.2f m wide, centre %.2f m ahead, %.2f m "
                             "up, %.2f m right, from a %ux%u texture",
                             panel.layer == 1 ? "follows the head" : "fixed to the seat",
                             panel.width, panel.ahead, panel.up, panel.right,
                             panel.textureWidth, panel.textureHeight);
                    else
                        logf("xr: HUD panel off");
                }
                if (hudOn && (hud.image.width != panel.textureWidth ||
                              hud.image.height != panel.textureHeight)) {
                    bool failedBefore = hud.failedWidth == panel.textureWidth &&
                                        hud.failedHeight == panel.textureHeight;
                    if (failedBefore ||
                        !createHudSwapchain(session, device, chosenFormat,
                                            panel.textureWidth, panel.textureHeight, hud,
                                            "HUD panel"))
                        hudOn = false;
                }

                XrSpace hudSpace = XR_NULL_HANDLE;
                if (hudOn && panel.layer == 2) {
                    updateSeatSpace(session, consumer, hud, "HUD panel");
                    hudSpace = hud.seatSpace;
                    if (!hudSpace && !hud.loggedNoSeat) {
                        hud.loggedNoSeat = true;
                        logf("xr: HUD panel -- no seat yet; it appears once the game has "
                             "recentred");
                    }
                } else if (hudOn) {
                    hudSpace = viewSpace;
                }

                if (hudSpace && hud.image.swapchain &&
                    updateHudImage(consumer, context, panel, hud)) {
                    // A quad faces +z. Turned toward the eye from where it hangs: a yaw
                    // about +y toward the centre line, after a pitch about +x toward eye
                    // height -- negative when the panel is below the eye, which leans its
                    // face up. q = yaw * pitch.
                    float level =
                        std::sqrt(panel.right * panel.right + panel.ahead * panel.ahead);
                    float yaw = std::atan2(-panel.right, panel.ahead);
                    float pitch = std::atan2(panel.up, level);
                    float cy = std::cos(yaw * 0.5f);
                    float sy = std::sin(yaw * 0.5f);
                    float cp = std::cos(pitch * 0.5f);
                    float sp = std::sin(pitch * 0.5f);

                    hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    hudQuad.space = hudSpace;
                    hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    hudQuad.subImage.swapchain = hud.image.swapchain;
                    hudQuad.subImage.imageRect.offset = {0, 0};
                    hudQuad.subImage.imageRect.extent = {
                        static_cast<int32_t>(hud.image.width),
                        static_cast<int32_t>(hud.image.height)};
                    hudQuad.subImage.imageArrayIndex = 0;
                    hudQuad.pose.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};
                    hudQuad.pose.position = {panel.right, panel.up, -panel.ahead};
                    hudQuad.size = {panel.width, panel.width *
                                                     static_cast<float>(hud.image.height) /
                                                     static_cast<float>(hud.image.width)};
                    layers[layerCount++] =
                        reinterpret_cast<XrCompositionLayerBaseHeader*>(&hudQuad);
                }

                // -- the VR menu's quad ---------------------------------------------
                //
                // Simpler than the HUD's: the game publishes the pose outright, because
                // the menu hangs where the driver was looking when it opened and nothing
                // on this side can derive that. Always in the seat's space, which is the
                // space the game states it in.
                Consumer::MenuPanel menuPanel;
                bool menuOn = presenting && consumer.menuPanel(menuPanel);
                if (menuOn && menu.loggedLayer != 1) {
                    menu.loggedLayer = 1;
                    logf("xr: the VR menu layer is on -- %.2f x %.2f m from a %ux%u "
                         "texture, in the seat's space",
                         menuPanel.widthMetres, menuPanel.heightMetres,
                         menuPanel.textureWidth, menuPanel.textureHeight);
                } else if (!menuOn && menu.loggedLayer != 0) {
                    menu.loggedLayer = 0;
                    logf("xr: the VR menu layer is off");
                }
                if (menuOn && (menu.image.width != menuPanel.textureWidth ||
                               menu.image.height != menuPanel.textureHeight)) {
                    bool failedBefore = menu.failedWidth == menuPanel.textureWidth &&
                                        menu.failedHeight == menuPanel.textureHeight;
                    if (failedBefore ||
                        !createHudSwapchain(session, device, chosenFormat,
                                            menuPanel.textureWidth,
                                            menuPanel.textureHeight, menu, "VR menu"))
                        menuOn = false;
                }

                // Which space the game stated the pose in. Usually the seat's; the view's
                // while the menu is annotating a head-locked HUD panel, because a layer
                // drawn ON that panel has to be re-placed against the head exactly as the
                // panel itself is.
                XrSpace menuSpace = XR_NULL_HANDLE;
                if (menuOn) {
                    if (menuPanel.space == kMenuSpaceView) {
                        menuSpace = viewSpace;
                    } else {
                        updateSeatSpace(session, consumer, menu, "VR menu");
                        menuSpace = menu.seatSpace;
                        if (!menuSpace && !menu.loggedNoSeat) {
                            menu.loggedNoSeat = true;
                            logf("xr: the VR menu -- no seat yet; it appears once the game "
                                 "has recentred");
                        }
                    }
                    if (!menuSpace) menuOn = false;
                }

                if (menuOn && menu.image.swapchain &&
                    updateMenuImage(consumer, context, menuPanel, menu)) {
                    menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    menuQuad.space = menuSpace;
                    menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    menuQuad.subImage.swapchain = menu.image.swapchain;
                    menuQuad.subImage.imageRect.offset = {0, 0};
                    menuQuad.subImage.imageRect.extent = {
                        static_cast<int32_t>(menu.image.width),
                        static_cast<int32_t>(menu.image.height)};
                    menuQuad.subImage.imageArrayIndex = 0;
                    menuQuad.pose.orientation = {
                        menuPanel.pose.orientationX, menuPanel.pose.orientationY,
                        menuPanel.pose.orientationZ, menuPanel.pose.orientationW};
                    menuQuad.pose.position = {menuPanel.pose.positionX,
                                              menuPanel.pose.positionY,
                                              menuPanel.pose.positionZ};
                    menuQuad.size = {menuPanel.widthMetres, menuPanel.heightMetres};
                    layers[layerCount++] =
                        reinterpret_cast<XrCompositionLayerBaseHeader*>(&menuQuad);
                }

                ++framesThisSecond;
                const DWORD now = GetTickCount();
                if (now - lastReport >= 1000) {
                    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
                    g_api.xrLocateSpace(viewSpace, appSpace,
                                        frameState.predictedDisplayTime, &head);
                    double qpcMs = 1000.0 / static_cast<double>(qpcFrequency.QuadPart);
                    logf("xr: %u frames/s, %u skipped a display, worst wait gap %.1f ms, "
                         "worst wait-to-end %.1f ms | head (%.3f, %.3f, %.3f) quat (%.3f, "
                         "%.3f, %.3f, %.3f)",
                         framesThisSecond, skippedDisplays, worstWaitGap * qpcMs,
                         worstFrameWork * qpcMs, head.pose.position.x, head.pose.position.y,
                         head.pose.position.z, head.pose.orientation.x,
                         head.pose.orientation.y, head.pose.orientation.z,
                         head.pose.orientation.w);
                    if (poseSamples || poseAge[4]) {
                        logf("xr: rendered pose age in displays 0:%u 1:%u 2:%u 3+:%u "
                             "UNMATCHED:%u (%u submits repeated an image) | head turned since "
                             "render mean %.2f max %.2f deg, unexplained by head speed mean "
                             "%.2f max %.2f deg",
                             poseAge[0], poseAge[1], poseAge[2], poseAge[3], poseAge[4],
                             poseRepeats,
                             poseSamples ? poseAngleSum / poseSamples : 0.0, poseAngleMax,
                             poseSamples ? poseResidualSum / poseSamples : 0.0,
                             poseResidualMax);
                    }
                    for (unsigned& count : poseAge) count = 0;
                    poseRepeats = 0;
                    poseSamples = 0;
                    poseAngleSum = 0.0;
                    poseAngleMax = 0.0;
                    poseResidualSum = 0.0;
                    poseResidualMax = 0.0;
                    framesThisSecond = 0;
                    skippedDisplays = 0;
                    worstWaitGap = 0;
                    worstFrameWork = 0;
                    lastReport = now;
                }
            }

            LARGE_INTEGER endCall = {};
            QueryPerformanceCounter(&endCall);
            if (endCall.QuadPart - waitReturn.QuadPart > worstFrameWork)
                worstFrameWork = endCall.QuadPart - waitReturn.QuadPart;

            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = blendMode;
            endInfo.layerCount = layerCount;
            endInfo.layers = layerCount ? layers : nullptr;
            if (!XR_CHECK(g_api.xrEndFrame(session, &endInfo))) break;
        }

        const bool timeUp = options.seconds > 0 &&
                            (GetTickCount() - startTick) >
                                (DWORD)options.seconds * 1000u;
        // Esc is for the survey. When presenting, the keyboard belongs to the game --
        // reading it here would end the session the moment the driver braked.
        const bool escape =
            !presenting && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

        const bool gameGone = presenting && !consumer.producerAlive();
        const bool gameAsked = presenting && consumer.shutdownRequested();
        // Works in presenting mode too, unlike Esc: the console is ours even when the
        // keyboard belongs to the game.
        const bool stopped = g_stopRequested;

        if ((timeUp || escape || gameGone || gameAsked || stopped) && !exitRequested) {
            exitRequested = true;
            exitDeadline = GetTickCount() + 5000;
            logf("xr: %s -- requesting exit",
                 gameGone    ? "the game exited"
                 : gameAsked ? "the game asked us to stop"
                 : timeUp    ? "time is up"
                 : stopped   ? "Ctrl+C"
                             : "Esc pressed");
            if (!XR_CHECK(g_api.xrRequestExitSession(session))) exitLoop = true;
            // Do not break: the runtime still has to walk the session down through
            // STOPPING, and skipping xrEndSession leaves it wedged for the next run.
            lastReport = GetTickCount();
        }

        // ...but do not wait forever for it either. A session that never became READY
        // has no state machine to walk down, and a runtime that ignores the request
        // should not turn a diagnostic into a process you have to kill.
        if (exitRequested && (long)(GetTickCount() - exitDeadline) > 0) {
            logf("xr: the session did not reach EXITING within 5 s -- leaving anyway");
            exitLoop = true;
        }
    }

    logf("xr: shutting down");
    // Before the device it holds views on.
    consumer.close();
    for (Eye& eye : eyes) releaseEye(eye);
    releaseHud(hud);
    releaseHud(menu);
    // Before the session: the action spaces and the action set belong to it.
    controllers.destroy(g_api);
    if (viewSpace) g_api.xrDestroySpace(viewSpace);
    if (appSpace) g_api.xrDestroySpace(appSpace);
    g_api.xrDestroySession(session);
    if (context1) context1->Release();
    context->Release();
    device->Release();
    g_api.xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
    xrUnloadLoader(g_api);
    logf("xr: done -- the same lines are in gplvr-xr.log next to this executable");
    logClose();
    return 0;
}
