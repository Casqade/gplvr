// gplvr -- the mod DLL.
//
// Injected into gplc67.exe by gplvr.exe (src/loader), which creates the process
// suspended -- so this DllMain runs before the game's entry point and the IAT hooks
// below are in place before the game calls anything.
//
// An earlier version shipped as a version.dll proxy. That was a mistake: occupying a
// system DLL's name means every module in the process resolves its imports against
// us, and the GPU driver roglv2 loads needed exports we had not provided. Injection
// has no such shared namespace.

#include "Discovery.h"
#include "Hooks.h"
#include "Log.h"

#include <windows.h>

#include <cstring>

namespace gplvr {
namespace {

// -- LoadLibrary interception ----------------------------------------------
// Gives us a deterministic moment to patch a freshly loaded rasterizer's imports,
// rather than racing it from the poll thread.

using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);

LoadLibraryAFn   g_loadLibraryA = nullptr;
LoadLibraryWFn   g_loadLibraryW = nullptr;
LoadLibraryExAFn g_loadLibraryExA = nullptr;

HMODULE WINAPI loadLibraryAHook(LPCSTR name) {
    HMODULE result = g_loadLibraryA ? g_loadLibraryA(name) : LoadLibraryA(name);
    if (result) patchDisplaySettingsImports(result);
    return result;
}

HMODULE WINAPI loadLibraryWHook(LPCWSTR name) {
    HMODULE result = g_loadLibraryW ? g_loadLibraryW(name) : LoadLibraryW(name);
    if (result) patchDisplaySettingsImports(result);
    return result;
}

HMODULE WINAPI loadLibraryExAHook(LPCSTR name, HANDLE file, DWORD flags) {
    HMODULE result =
        g_loadLibraryExA ? g_loadLibraryExA(name, file, flags) : LoadLibraryExA(name, file, flags);
    if (result && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)))
        patchDisplaySettingsImports(result);
    return result;
}

// Redirect one named import of the main module to `replacement`, remembering the
// original so we can still call through.
template <typename Fn>
void hookMainModuleImport(const char* name, void* replacement, Fn* original) {
    ModuleImage image;
    if (!getModuleImage(GetModuleHandleA(nullptr), image)) return;

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return;

    auto descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            image.base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                         : descriptor->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            const_cast<uint8_t*>(image.base) + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                image.base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), name) != 0) continue;

            *original = reinterpret_cast<Fn>(static_cast<uintptr_t>(slots->u1.Function));
            auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement));
            if (patchBytes(&slots->u1.Function, &value, sizeof(value)))
                logf("IAT: main module %s -> hooked", name);
            return;
        }
    }
    logf("IAT: main module does not import %s", name);
}

// -- deferred install -------------------------------------------------------
// The rasterizer globals stay null until the game has picked and loaded a
// rasterizer DLL, so we wait for them rather than guessing at timing.

DWORD WINAPI installThread(LPVOID) {
    logf("waiting for a rasterizer to be loaded...");

    Binding binding;
    for (int attempt = 0; attempt < 4000; ++attempt) {  // ~100 s, then give up
        ModuleImage image;
        RasterTable probe;
        if (getModuleImage(GetModuleHandleA(nullptr), image) &&
            discoverRasterTable(image, probe)) {
            break;
        }
        Sleep(25);
    }

    if (!discover(binding)) {
        logf("discovery failed -- installing nothing. The game is unmodified.");
        return 0;
    }

    logf("--- compare against the static analysis of gplc67.exe ---");
    logf("    expected cameraBind 004C7AB0, sceneRender 00487620, simFrameRender 0048B240");

    logf("patching import tables...");
    patchDisplaySettingsImportsEverywhere();
    logf("installing frame counter...");

    if (!installFrameCounter(binding)) {
        logf("frame counter NOT installed -- aborting, the scene hook depends on it");
        return 0;
    }
    requestSceneRenderHook(binding);

    logf("ready. '1' toggles stereo.");
    return 0;
}

}  // namespace
}  // namespace gplvr

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    using namespace gplvr;

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        setSelfModule(module);
        logOpen("gplvr.log");

        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        // Stamp the build. Two runs have now been discussed without knowing whether
        // the binary contained a given fix, which wastes the test.
        logf("gplvr attached to %s -- built %s %s", exe, __DATE__, __TIME__);

        loadOptions("gplvr.ini");

        // Deterministic: this runs before the game's entry point, so every
        // LoadLibrary the game makes afterwards passes through us.
        hookMainModuleImport("LoadLibraryA", reinterpret_cast<void*>(&loadLibraryAHook), &g_loadLibraryA);
        hookMainModuleImport("LoadLibraryW", reinterpret_cast<void*>(&loadLibraryWHook), &g_loadLibraryW);
        hookMainModuleImport("LoadLibraryExA", reinterpret_cast<void*>(&loadLibraryExAHook), &g_loadLibraryExA);
        patchDisplaySettingsImports(GetModuleHandleA(nullptr));

        if (HANDLE thread = CreateThread(nullptr, 0, &installThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf("detaching");
        logClose();
    }
    return TRUE;
}
