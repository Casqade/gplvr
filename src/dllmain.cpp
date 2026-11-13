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

#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBuildWorker.h"
#include "Billboard.h"
#include "BuildStamp.h"
#include "CrashGuard.h"
#include "DataRebase.h"
#include "Discovery.h"
#include "DrawnMarks.h"
#include "FlagmanReplay.h"
#include "FlatFrames.h"
#include "GameExit.h"
#include "HeadAudio.h"
#include "Hooks.h"
#include "Hud.h"
#include "fast_text/FastText.h"
#include "Interpolation.h"
#include "Letterbox.h"
#include "FlatTarget.h"
#include "GlTrace.h"
#include "MenuScale.h"
#include "MirrorDriverArms.h"
#include "NoSignal.h"
#include "Paths.h"
#include "QuickMenu.h"
#include "ReplayQuickMenu.h"
#include "RenderPacing.h"
#include "RenderProfile.h"
#include "SkidFlood.h"
#include "Stereo.h"
#include "Log.h"
#include "LogDir.h"
#include "Window.h"

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
    if (result) glTraceInstall();
    return result;
}

HMODULE WINAPI loadLibraryWHook(LPCWSTR name) {
    HMODULE result = g_loadLibraryW ? g_loadLibraryW(name) : LoadLibraryW(name);
    if (result) patchDisplaySettingsImports(result);
    if (result) glTraceInstall();
    return result;
}

HMODULE WINAPI loadLibraryExAHook(LPCSTR name, HANDLE file, DWORD flags) {
    HMODULE result =
        g_loadLibraryExA ? g_loadLibraryExA(name, file, flags) : LoadLibraryExA(name, file, flags);
    if (result && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        patchDisplaySettingsImports(result);
        glTraceInstall();
    }
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

// -- timer resolution -------------------------------------------------------
// GPL's frame loop and its BG lock both wait in Sleep(1) polls, and gplvr's own waits sleep
// too. Sleep(1) lasts until the next system timer interrupt, which is up to 15.6 ms away at the
// default resolution and about 1 ms away once a process has asked for that. Since Windows 10
// 2004 the request only counts for the process that made it, so it is made here whatever else
// is running. Resolved by name: winmm is the game's own dependency, and this keeps it out of
// gplvr's import table.

void raiseTimerResolution() {
    using TimeBeginPeriodFn = UINT(WINAPI*)(UINT);
    HMODULE winmm = LoadLibraryA("winmm.dll");
    auto    begin = winmm ? reinterpret_cast<TimeBeginPeriodFn>(
                                GetProcAddress(winmm, "timeBeginPeriod"))
                          : nullptr;
    if (!begin) {
        logf("timer: winmm's timeBeginPeriod did not resolve -- the system timer resolution is "
             "left as it is");
        return;
    }
    // 0 is TIMERR_NOERROR. The request lasts until the process exits.
    UINT result = begin(1);
    logf("timer: 1 ms system timer resolution requested for this process -- %s",
         result == 0 ? "accepted" : "REFUSED");
}

// -- deferred install -------------------------------------------------------
// The rasterizer globals stay null until the game has picked and loaded a
// rasterizer DLL, so we wait for them rather than guessing at timing.

DWORD WINAPI installThread(LPVOID) {
    raiseTimerResolution();
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

    // Flat mode too, and before anything below can abort the install: a crash fix in the
    // sim, not part of the VR render path. Still in the front end here, where no contact
    // search is running while its bytes are rewritten.
    crashGuardInstall();
    // Flat mode too, and here for the same reason: in the front end no skid mark is being laid
    // while the three immediates are rewritten.
    skidFloodInstall();
    // Flat mode too, and here for the same reason: in the front end no replay is playing while
    // the handler and the two reads are rewritten.
    flagmanReplayInstall();
    // Flat mode too, and nothing downstream depends on it: one data byte, read only on the way
    // out of the game.
    creditsSkipInstall();
    // Flat mode too: one option byte, and the yellow square it removes is in vanilla GPL.
    mirrorDriverArmsInstall();
    // Flat mode too: a stock overflow in the render walk (report 53). The patch stays on the
    // stock list until the next clear, so it may go in with a pass under way.
    drawnMarksInstall();

    logf("installing frame counter...");

    if (!installFrameCounter(binding)) {
        logf("frame counter NOT installed -- aborting, the scene hook depends on it");
        return 0;
    }
    requestSceneRenderHook(binding);
    // Flat mode too. Installs the car, flagman and mirror hooks now when rendererType() is on, and
    // otherwise when the VR menu first switches it on; the node filter goes in on the render
    // thread, at the first baked main walk.
    bakedPassBind(binding);

    // Headset only. The per-eye HUD thunks and the billboard fix change what the game
    // draws, and flat mode is the vanilla game in a borderless window.
    if (g_options.vr) {
        hudBind(binding);
        billboardInstall(binding);
        headAudioInstall();
        // Likewise for the replay's stick and radial.
        replayQuickMenuInstall();
    }
    // Flat mode too. Patches nothing: it checks the functions and globals the radial calls and
    // reads, and the game exit's back to pits is the radial's.
    quickMenuInstall();
    // Flat mode too, after quickMenuInstall: checks GPL's main-menu Exit, which Alt+F4 and the
    // window's Close end in.
    gameExitInstall();
    // AFTER hudBind, and the order matters. In flat mode this takes all seven 2D globals, which
    // Hud.cpp has not touched. In VR it takes stampDraw alone -- Hud.cpp has already left that
    // one slot for it, so there is exactly one owner per slot and neither module can adopt the
    // other's thunk as "the original".
    fastTextBind(binding);

    // Before the first menu-to-race transition, which is the moment the screenBegin
    // thunk earns its keep. Discovery has already run, so the rasterizer is loaded and
    // its window manager exists.
    windowInstall(binding);
    menuScaleInstall(binding);

    // After windowInstall, which is what resolves the engine's mode: the layout thunks
    // size the middle panel from it the first time a race is entered.
    letterboxInstall();
    noSignalInstall();

    // The driver thunks go in here: roglv2 is loaded by now, because discovery keys off
    // its entry points.
    // Pacing and the profiler both redirect the fgDisableBG call in the tick wait. Pacing
    // locates it while the bytes are still stock, the profiler patches it, and pacing then
    // takes the site and chains to the profiler's thunk.
    renderPacingDiscover(binding);
    renderProfileDiscover(binding);
    renderProfileInstallDriverThunks();
    // After the profiler's: its SwapBuffers timer, when it is on, becomes this one's original.
    flatTargetInstall();
    glTraceInstall();
    renderPacingInstall();
    // Locates the latch from the tick wait pacing installed at.
    interpolationInstall();
    // After it: an extra frame with nothing to re-blend is a duplicate, so this declines to
    // install unless the blend is running.
    flatFramesInstall();
    // The segment-lookup fix of report 84 is not a VR one. It needs a BLENDED position judged
    // against the latch's KEPT seed, and flat mode does that as soon as frames are drawn between
    // the ticks -- so it goes in whenever the blend is actually running. In VR stereoRender
    // installs it itself, between passes.
    if (!g_options.vr && interpolationActive()) stereoInstallWalkLookup();

    if (g_options.vr)
        logf("ready. '1' toggles stereo.");
    else
        logf("ready -- flat mode: borderless window, letterbox and widescreen fixes, "
             "front-end scaling, roglv2's glFinish drop and swap interval, mouselook, the fast "
             "text path%s. Stereo, the HUD split, billboards and the headset profilers need "
             "vr = 1.",
             g_options.frameRate > 0
                 ? ", and frames drawn between the simulation's ticks with the world "
                   "interpolated onto them"
                 : " (frameRate = 0: one frame per simulation tick, as GPL ships)");
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
        // the binary contained a given fix, which wastes the test. The stamp is the link's
        // (BuildStamp.cpp is recompiled every build); this file's own __DATE__/__TIME__ only
        // moved when it was recompiled.
        //
        // And gplvr's own folder, which is where this log, gplvr.ini and everything else of
        // ours is: the game can be launched with any executable in any layout, so "which ini
        // did it read" is a question the log has to answer rather than the reader assume.
        logf("gplvr %s (%s, built %s) attached to %s, from %s", buildVersion(), buildCommit(),
             buildStamp(), exe, pathsOwnDirectory());
        logf("logs in %s", logDirectory());

        // Where this image keeps its globals, before anything reads or writes one. Links of
        // 1.2.0.2 ship identical code with the tail of `.data` 8 or 16 bytes apart, so a guard
        // that only checks code bytes passes on every one of them and then uses an address that
        // belongs to another. Everything that goes through rebase() depends on this line.
        dataRebaseResolve();

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
        // Before the log closes, and before this module's code stops being callable.
        windowProcUnchain();
        carBuildWorkerStop();
        crashGuardLogSummary();
        flagmanReplayLogSummary();
        drawnMarksLogSummary();
        logf("detaching");
        logClose();
    }
    return TRUE;
}
