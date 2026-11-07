#include "XrRuntimeProbe.h"

#include <windows.h>

#include <cstdio>

namespace gplvr {
namespace {

bool fileExists(const wchar_t* path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

bool xrRuntimeFind(char* out, size_t size) {
    wchar_t path[1024] = {};
    DWORD chars = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", path, 1024);
    if (chars && chars < 1024) {
        // The loader takes the variable alone when it is set, so a dead path is no runtime.
        bool exists = fileExists(path);
        _snprintf_s(out, size, _TRUNCATE, "XR_RUNTIME_JSON = %S%s", path,
                    exists ? "" : ", which names no file");
        return exists;
    }

    const wchar_t* subKey = L"SOFTWARE\\Khronos\\OpenXR\\1";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        _snprintf_s(out, size, _TRUNCATE,
                    "XR_RUNTIME_JSON is not set and HKLM\\%S does not exist (64-bit view)", subKey);
        return false;
    }
    wchar_t value[1024] = {};
    DWORD type = 0;
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    LSTATUS read =
        RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type, reinterpret_cast<BYTE*>(value),
                         &bytes);
    RegCloseKey(key);
    if (read != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || !value[0]) {
        _snprintf_s(out, size, _TRUNCATE,
                    "XR_RUNTIME_JSON is not set and HKLM\\%S has no ActiveRuntime (64-bit view)",
                    subKey);
        return false;
    }
    if (type == REG_EXPAND_SZ) {
        DWORD expanded = ExpandEnvironmentStringsW(value, path, 1024);
        if (!expanded || expanded > 1024) path[0] = 0;
    } else {
        lstrcpynW(path, value, 1024);
    }
    bool exists = path[0] && fileExists(path);
    _snprintf_s(out, size, _TRUNCATE, "HKLM\\%S\\ActiveRuntime = %S%s", subKey, value,
                exists ? "" : ", which names no file");
    return exists;
}

}  // namespace gplvr
