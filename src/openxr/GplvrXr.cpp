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
};

void releaseEye(Eye& eye) {
    for (ID3D11RenderTargetView* target : eye.targets)
        if (target) target->Release();
    eye.targets.clear();
    if (eye.swapchain) g_api.xrDestroySwapchain(eye.swapchain);
    eye.swapchain = XR_NULL_HANDLE;
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

    const char* enabledExtensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "gplvr survey");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "gplvr");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = kRequestedApiVersion;
    if (d3d11) {
        createInfo.enabledExtensionCount = 1;
        createInfo.enabledExtensionNames = enabledExtensions;
    }

    if (!XR_CHECK(g_api.xrCreateInstance(&createInfo, &g_instance))) {
        logf("xr: stopping -- no instance");
        xrUnloadLoader(g_api);
        logClose();
        return 1;
    }
    xrResolveInstanceFunctions(g_api, g_instance);

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
        consumer.publishRequirements(chosen.AdapterLuid,
                                     viewConfigs[0].recommendedImageRectWidth,
                                     viewConfigs[0].recommendedImageRectHeight);
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
    // Prefer an sRGB format: the runtime then does the colour conversion, which is what
    // GPL's 8-bit output wants.
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
        const uint32_t maxW = viewConfigs[i].maxImageRectWidth;
        const uint32_t maxH = viewConfigs[i].maxImageRectHeight;
        float scale = options.renderScale > 0.05f ? options.renderScale : 1.0f;

        uint32_t width = static_cast<uint32_t>(
            viewConfigs[i].recommendedImageRectWidth * scale);
        uint32_t height = static_cast<uint32_t>(
            viewConfigs[i].recommendedImageRectHeight * scale);
        if (maxW && width > maxW) width = maxW;
        if (maxH && height > maxH) height = maxH;
        if (width < 64) width = 64;
        if (height < 64) height = 64;
        if (i == 0 && (width != static_cast<uint32_t>(
                                    viewConfigs[i].recommendedImageRectWidth * scale) ||
                       height != static_cast<uint32_t>(
                                     viewConfigs[i].recommendedImageRectHeight * scale)))
            logf("xr: vrRenderScale %.2f asked for more than the runtime allows -- "
                 "clamped to its maximum of %ux%u",
                 scale, maxW, maxH);

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

        for (uint32_t image = 0; image < imageCount; ++image) {
            ID3D11RenderTargetView* target = nullptr;
            D3D11_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = static_cast<DXGI_FORMAT>(chosenFormat);
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateRenderTargetView(images[image].texture, &desc,
                                                      &target))) {
                logf("xr: CreateRenderTargetView failed for eye %u image %u", i, image);
                swapchainsOk = false;
                break;
            }
            eyes[i].targets.push_back(target);
        }
        logf("xr: eye %u swapchain %ux%u, %u images", i, eyes[i].width, eyes[i].height,
             imageCount);
    }

    if (presenting && !consumer.initialise(device, context)) {
        logf("xr: the blit pipeline would not build -- stopping");
        consumer.setError(2);
        swapchainsOk = false;
    }

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
            }
            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!running) {
            Sleep(10);
        } else {
            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState    frameState{XR_TYPE_FRAME_STATE};
            if (!XR_CHECK(g_api.xrWaitFrame(session, &waitInfo, &frameState))) break;

            XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
            if (!XR_CHECK(g_api.xrBeginFrame(session, &beginInfo))) break;

            XrCompositionLayerProjectionView projectionViews[2] = {};
            XrCompositionLayerProjection     layer{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            const XrCompositionLayerBaseHeader* layers[1] = {
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
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
                    // once per frame rather than once per eye.
                    consumer.update();

                    const XrView* publish = views;
                    const XrTime  publishTime = frameState.predictedDisplayTime;

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
                    XrSpaceLocation headLocation{XR_TYPE_SPACE_LOCATION};
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

                    consumer.publishViews(shared, head, headValid);
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
                                                     static_cast<int>(i));
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

                    // Submitting a projection layer whose second view was never filled
                    // in would hand the runtime an identity pose for one eye. Drop the
                    // whole layer instead; xrEndFrame with no layers is legal.
                    if (bothEyesReady) {
                        layer.space = appSpace;
                        layer.viewCount = 2;
                        layer.views = projectionViews;
                        layerCount = 1;
                    }
                }

                ++framesThisSecond;
                const DWORD now = GetTickCount();
                if (now - lastReport >= 1000) {
                    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
                    g_api.xrLocateSpace(viewSpace, appSpace,
                                        frameState.predictedDisplayTime, &head);
                    logf("xr: %u frames/s | head (%.3f, %.3f, %.3f) quat (%.3f, %.3f, "
                         "%.3f, %.3f)",
                         framesThisSecond, head.pose.position.x, head.pose.position.y,
                         head.pose.position.z, head.pose.orientation.x,
                         head.pose.orientation.y, head.pose.orientation.z,
                         head.pose.orientation.w);
                    framesThisSecond = 0;
                    lastReport = now;
                }
            }

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
