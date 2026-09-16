#include "openxr/Xr.h"

#include "Log.h"

#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

const wchar_t* loaderSubPath() {
#if defined(_WIN64)
    return L"runtime\\openxr\\win64\\openxr_loader.dll";
#else
    return L"runtime\\openxr\\win32\\openxr_loader.dll";
#endif
}

void directoryOfThisExecutable(wchar_t (&path)[MAX_PATH]) {
    path[0] = L'\0';
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) slash[1] = L'\0';
}

template <typename T>
bool resolve(const XrApi& api, XrInstance instance, const char* name, T& out,
             bool required) {
    PFN_xrVoidFunction raw = nullptr;
    const XrResult result = api.getInstanceProcAddr(instance, name, &raw);
    if (XR_FAILED(result) || !raw) {
        if (required) logf("xr: could not resolve %s (XrResult %d)", name, (int)result);
        out = nullptr;
        return false;
    }
    out = reinterpret_cast<T>(raw);
    return true;
}

// Only the results that can happen before an instance exists, where the runtime's own
// xrResultToString is not yet reachable. Everything else falls through to the runtime.
const char* earlyResultName(XrResult result) {
    switch (result) {
        case XR_SUCCESS:                             return "XR_SUCCESS";
        case XR_ERROR_VALIDATION_FAILURE:            return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE:               return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_OUT_OF_MEMORY:                 return "XR_ERROR_OUT_OF_MEMORY";
        case XR_ERROR_API_VERSION_UNSUPPORTED:       return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_INITIALIZATION_FAILED:         return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FUNCTION_UNSUPPORTED:          return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_FEATURE_UNSUPPORTED:           return "XR_ERROR_FEATURE_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT:         return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_LIMIT_REACHED:                 return "XR_ERROR_LIMIT_REACHED";
        case XR_ERROR_SIZE_INSUFFICIENT:             return "XR_ERROR_SIZE_INSUFFICIENT";
        case XR_ERROR_HANDLE_INVALID:                return "XR_ERROR_HANDLE_INVALID";
        case XR_ERROR_INSTANCE_LOST:                 return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SYSTEM_INVALID:                return "XR_ERROR_SYSTEM_INVALID";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED:       return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE:       return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_API_LAYER_NOT_PRESENT:         return "XR_ERROR_API_LAYER_NOT_PRESENT";
        case XR_ERROR_NAME_INVALID:                  return "XR_ERROR_NAME_INVALID";
        case XR_ERROR_RUNTIME_UNAVAILABLE:           return "XR_ERROR_RUNTIME_UNAVAILABLE";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID:       return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING:
            return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
        default:                                     return nullptr;
    }
}

void logRegistryString(HKEY root, const wchar_t* rootName, const wchar_t* subKey,
                       const wchar_t* value, REGSAM view) {
    HKEY key = nullptr;
    const LSTATUS opened =
        RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | view, &key);
    const char* viewName = (view & KEY_WOW64_32KEY) ? "32-bit view" : "64-bit view";
    if (opened != ERROR_SUCCESS) {
        logf("xr: %S\\%S (%s) -- not present", rootName, subKey, viewName);
        return;
    }

    wchar_t text[1024] = {};
    DWORD bytes = sizeof(text) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS read = RegQueryValueExW(key, value, nullptr, &type,
                                          reinterpret_cast<BYTE*>(text), &bytes);
    RegCloseKey(key);

    if (read != ERROR_SUCCESS)
        logf("xr: %S\\%S\\%S (%s) -- no value", rootName, subKey, value, viewName);
    else
        logf("xr: %S\\%S\\%S (%s) = %S", rootName, subKey, value, viewName, text);
}

void logAvailableRuntimes(REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0,
                      KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
        return;
    }

    const char* viewName = (view & KEY_WOW64_32KEY) ? "32-bit view" : "64-bit view";
    for (DWORD index = 0;; ++index) {
        wchar_t name[1024] = {};
        DWORD nameChars = 1024;
        DWORD type = 0;
        DWORD data = 0;
        DWORD dataBytes = sizeof(data);
        const LSTATUS status =
            RegEnumValueW(key, index, name, &nameChars, nullptr, &type,
                          reinterpret_cast<BYTE*>(&data), &dataBytes);
        if (status != ERROR_SUCCESS) break;
        // A zero DWORD means enabled; anything else means the entry is disabled.
        logf("xr: available runtime (%s) [%s] %S", viewName,
             data == 0 ? "enabled" : "disabled", name);
    }
    RegCloseKey(key);
}

}  // namespace

bool xrLoadLoader(XrApi& api, const wchar_t* rootDirectory) {
    wchar_t path[MAX_PATH] = {};
    if (rootDirectory && *rootDirectory) {
        wcsncpy_s(path, rootDirectory, _TRUNCATE);
        const size_t length = wcslen(path);
        if (length && path[length - 1] != L'\\') wcsncat_s(path, L"\\", _TRUNCATE);
    } else {
        directoryOfThisExecutable(path);
    }
    wcsncat_s(path, loaderSubPath(), _TRUNCATE);

    api.loader = LoadLibraryW(path);
    if (!api.loader) {
        logf("xr: could not load %S (error %lu)", path, GetLastError());
        return false;
    }

    api.getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(api.loader, "xrGetInstanceProcAddr"));
    if (!api.getInstanceProcAddr) {
        logf("xr: %S does not export xrGetInstanceProcAddr", path);
        FreeLibrary(api.loader);
        api.loader = nullptr;
        return false;
    }

    logf("xr: loader %S (%u-bit)", path, (unsigned)(sizeof(void*) * 8));

    bool ok = true;
#define GPLVR_XR_RESOLVE(name) \
    ok = resolve(api, XR_NULL_HANDLE, #name, api.name, true) && ok;
    GPLVR_XR_GLOBAL_FUNCTIONS(GPLVR_XR_RESOLVE)
#undef GPLVR_XR_RESOLVE
    return ok;
}

bool xrResolveInstanceFunctions(XrApi& api, XrInstance instance) {
    bool ok = true;
#define GPLVR_XR_RESOLVE(name) ok = resolve(api, instance, #name, api.name, true) && ok;
    GPLVR_XR_INSTANCE_FUNCTIONS(GPLVR_XR_RESOLVE)
#undef GPLVR_XR_RESOLVE

    // Best effort: null simply means the extension was not enabled on this instance.
#define GPLVR_XR_RESOLVE(name) resolve(api, instance, #name, api.name, false);
    GPLVR_XR_D3D11_FUNCTIONS(GPLVR_XR_RESOLVE)
    GPLVR_XR_REFRESH_RATE_FUNCTIONS(GPLVR_XR_RESOLVE)
#undef GPLVR_XR_RESOLVE
    return ok;
}

void xrUnloadLoader(XrApi& api) {
    if (api.loader) FreeLibrary(api.loader);
    api = XrApi{};
}

const char* xrResultName(const XrApi& api, XrInstance instance, XrResult result) {
    static char buffer[XR_MAX_RESULT_STRING_SIZE + 16];

    if (const char* early = earlyResultName(result)) {
        _snprintf_s(buffer, _TRUNCATE, "%s (%d)", early, (int)result);
        return buffer;
    }
    if (api.xrResultToString && instance != XR_NULL_HANDLE) {
        char text[XR_MAX_RESULT_STRING_SIZE] = {};
        if (XR_SUCCEEDED(api.xrResultToString(instance, result, text))) {
            _snprintf_s(buffer, _TRUNCATE, "%s (%d)", text, (int)result);
            return buffer;
        }
    }
    _snprintf_s(buffer, _TRUNCATE, "XrResult %d", (int)result);
    return buffer;
}

void xrLogRuntimeSelection() {
    wchar_t override_[1024] = {};
    const DWORD chars =
        GetEnvironmentVariableW(L"XR_RUNTIME_JSON", override_, 1024);
    if (chars && chars < 1024)
        logf("xr: XR_RUNTIME_JSON = %S  (this overrides everything below)", override_);
    else
        logf("xr: XR_RUNTIME_JSON is not set");

    const wchar_t* activeKey = L"SOFTWARE\\Khronos\\OpenXR\\1";
    // Ask for each view by name rather than relying on the default. A 32-bit process
    // under WOW64 defaults to the 32-bit view, so "no flag" would mean opposite things
    // in the two builds -- and telling the two views apart is the whole point here. A
    // runtime present only in the 64-bit view has no 32-bit implementation, which is
    // exactly what decides whether OpenXR could ever run inside gplc67.exe.
    logRegistryString(HKEY_LOCAL_MACHINE, L"HKLM", activeKey, L"ActiveRuntime",
                      KEY_WOW64_64KEY);
    logRegistryString(HKEY_LOCAL_MACHINE, L"HKLM", activeKey, L"ActiveRuntime",
                      KEY_WOW64_32KEY);
    logRegistryString(HKEY_CURRENT_USER, L"HKCU", activeKey, L"ActiveRuntime",
                      KEY_WOW64_64KEY);
    logRegistryString(HKEY_CURRENT_USER, L"HKCU", activeKey, L"ActiveRuntime",
                      KEY_WOW64_32KEY);
    logAvailableRuntimes(KEY_WOW64_64KEY);
    logAvailableRuntimes(KEY_WOW64_32KEY);
}

}  // namespace gplvr
