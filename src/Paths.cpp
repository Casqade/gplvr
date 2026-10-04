#include "Paths.h"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>

// The base of the module this translation unit was linked into: gplvr.dll in the game process,
// gplvr-xr.exe in the presenter. The linker fills it in, so it costs nothing at run time and --
// unlike GetModuleHandleEx -- it asks the loader nothing, which matters because the first caller
// is logOpen inside DllMain, where the loader lock is already held.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace gplvr {
namespace {

HMODULE ownModule() { return reinterpret_cast<HMODULE>(&__ImageBase); }

// Function-local statics, so the path is read once and the read is thread-safe without a lock of
// our own. logOpen already holds one when it asks, and MirrorConfig asks from the render thread.
const std::string& directoryA() {
    static const std::string value = [] {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(ownModule(), path, MAX_PATH)) return std::string();
        char* slash = strrchr(path, '\\');
        if (!slash) return std::string();
        slash[1] = '\0';
        return std::string(path);
    }();
    return value;
}

const std::wstring& directoryW() {
    static const std::wstring value = [] {
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(ownModule(), path, MAX_PATH)) return std::wstring();
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash) return std::wstring();
        slash[1] = L'\0';
        return std::wstring(path);
    }();
    return value;
}

}  // namespace

const char* pathsOwnDirectory() { return directoryA().c_str(); }

const wchar_t* pathsOwnDirectoryW() { return directoryW().c_str(); }

bool pathsOwnFile(char* out, size_t size, const char* fileName) {
    if (!out || !size) return false;
    out[0] = '\0';
    const std::string& directory = directoryA();
    if (directory.size() + strlen(fileName) + 1 > size) return false;
    strcpy_s(out, size, directory.c_str());
    strcat_s(out, size, fileName);
    return true;
}

}  // namespace gplvr
