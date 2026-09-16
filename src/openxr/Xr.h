#pragma once

// A resolved OpenXR entry-point table, and the loader that fills it.
//
// Nothing here links against openxr_loader.lib. The loader DLL ships under
// runtime/openxr/<bitness>/ and is opened by explicit path; every function -- core and
// extension alike -- then comes from xrGetInstanceProcAddr. Two reasons:
//
//   * a missing, mismatched or crashing loader becomes a logged failure instead of a
//     process that refuses to start, which matters because this code will eventually
//     live inside the game;
//   * the same source builds 32- and 64-bit against the same headers, and the only
//     thing that changes is which DLL is opened.
//
// The 32-bit build is not dead weight. gplc67.exe is Win32, so "does this runtime have
// a 32-bit implementation at all" decides whether OpenXR can ever run in-process or
// whether the x64 companion is mandatory. Running the same survey both ways answers it.

#include <windows.h>
// openxr_platform.h names ID3D11Device and friends directly under
// XR_USE_GRAPHICS_API_D3D11, so d3d11.h has to come first or the graphics-binding
// structures do not compile.
#include <d3d11.h>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr_platform.h>

namespace gplvr {

// Requesting 1.0 rather than 1.1 keeps us compatible with runtimes that have not
// promoted their core support. We use no 1.1-only entry point.
inline constexpr XrVersion kRequestedApiVersion = XR_MAKE_VERSION(1, 0, 0);

// Callable with a null instance.
#define GPLVR_XR_GLOBAL_FUNCTIONS(X)          \
    X(xrEnumerateApiLayerProperties)          \
    X(xrEnumerateInstanceExtensionProperties) \
    X(xrCreateInstance)

// Everything else needs the instance handle.
#define GPLVR_XR_INSTANCE_FUNCTIONS(X)   \
    X(xrDestroyInstance)                 \
    X(xrResultToString)                  \
    X(xrGetInstanceProperties)           \
    X(xrGetSystem)                       \
    X(xrGetSystemProperties)             \
    X(xrEnumerateViewConfigurations)     \
    X(xrGetViewConfigurationProperties)  \
    X(xrEnumerateViewConfigurationViews) \
    X(xrEnumerateEnvironmentBlendModes)  \
    X(xrCreateSession)                   \
    X(xrDestroySession)                  \
    X(xrBeginSession)                    \
    X(xrEndSession)                      \
    X(xrRequestExitSession)              \
    X(xrEnumerateReferenceSpaces)        \
    X(xrCreateReferenceSpace)            \
    X(xrDestroySpace)                    \
    X(xrLocateSpace)                     \
    X(xrEnumerateSwapchainFormats)       \
    X(xrCreateSwapchain)                 \
    X(xrDestroySwapchain)                \
    X(xrEnumerateSwapchainImages)        \
    X(xrAcquireSwapchainImage)           \
    X(xrWaitSwapchainImage)              \
    X(xrReleaseSwapchainImage)           \
    X(xrPollEvent)                       \
    X(xrWaitFrame)                       \
    X(xrBeginFrame)                      \
    X(xrEndFrame)                        \
    X(xrLocateViews)

// Resolved only when the matching extension was enabled, so it may stay null.
#define GPLVR_XR_D3D11_FUNCTIONS(X) X(xrGetD3D11GraphicsRequirementsKHR)

// XR_FB_display_refresh_rate. Best effort like the D3D11 entry, and callable only when the
// extension was enabled on the instance.
#define GPLVR_XR_REFRESH_RATE_FUNCTIONS(X) \
    X(xrGetDisplayRefreshRateFB)           \
    X(xrEnumerateDisplayRefreshRatesFB)

struct XrApi {
    HMODULE loader = nullptr;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;

#define GPLVR_XR_DECLARE(name) PFN_##name name = nullptr;
    GPLVR_XR_GLOBAL_FUNCTIONS(GPLVR_XR_DECLARE)
    GPLVR_XR_INSTANCE_FUNCTIONS(GPLVR_XR_DECLARE)
    GPLVR_XR_D3D11_FUNCTIONS(GPLVR_XR_DECLARE)
    GPLVR_XR_REFRESH_RATE_FUNCTIONS(GPLVR_XR_DECLARE)
#undef GPLVR_XR_DECLARE
};

// Opens runtime/openxr/win32|win64/openxr_loader.dll relative to `rootDirectory` and
// resolves the global entry points. `rootDirectory` is where gplvr is installed; pass
// null to use the directory of the running executable.
bool xrLoadLoader(XrApi& api, const wchar_t* rootDirectory);

// Resolves the instance-level table. Call after xrCreateInstance. Extension functions
// are resolved best-effort and stay null if the extension was not enabled.
bool xrResolveInstanceFunctions(XrApi& api, XrInstance instance);

void xrUnloadLoader(XrApi& api);

// "XR_ERROR_FORM_FACTOR_UNAVAILABLE (-33)". Uses the runtime's own xrResultToString
// once an instance exists, and a built-in table for the results that can occur before
// there is one -- which is precisely when the message matters most.
const char* xrResultName(const XrApi& api, XrInstance instance, XrResult result);

// Writes every fact that decides *which* runtime the loader will pick: the
// XR_RUNTIME_JSON override, the ActiveRuntime registry values in both the 64- and
// 32-bit views, and the AvailableRuntimes list with its enable flags.
//
// This exists because "which runtime am I actually talking to" is otherwise invisible,
// and a headset that only works with one particular runtime installed is a question
// about this data, not about our code.
void xrLogRuntimeSelection();

}  // namespace gplvr
