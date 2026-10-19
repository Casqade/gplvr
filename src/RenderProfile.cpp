#include "RenderProfile.h"

#include "Discovery.h"
#include "FrameBudget.h"
#include "GpuTimer.h"
#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
#include "Pe.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gplvr {

namespace {

// The instruments this file owns run under either key. `profileStages` is the VR-mode
// switch that also turns on the frame budget and its GPU marks; `profileWorkWait` is the
// part of it flat mode can have -- the `pre` markers and roglv2's import thunks -- because
// those carry no GPU marks and so leave profilePost's timer alone. Loading makes
// profileStages imply profileWorkWait, so a VR session that set the old key keeps
// everything it had and gains the work/wait lines.
bool profilingWorkWait() { return g_options.profileWorkWait; }

// ---------------------------------------------------------------------------------------
// GPL's own render counters
// ---------------------------------------------------------------------------------------
//
// The reset function is the anchor, and it is a very distinctive shape: `xor eax, eax`
// followed by SEVEN consecutive `mov [imm32], eax` stores and a `retn`, with the seven
// targets four bytes apart because they are one contiguous block of globals. Nothing else
// in the image looks like that, and requiring the addresses to be consecutive AND writable
// turns a byte pattern into something that cannot match by accident.
//
// The first five stores are the counters. The last two are the halves of a double that is
// reset and displayed but never written by anything -- dead in the shipping build, and
// skipped here rather than reported as a permanent zero.

const int kStores = 7;
const int kCounters = 5;

// 33 C0                xor eax, eax
// A3 imm32             mov [imm32], eax     x7
// C3                   retn
const size_t kResetLength = 2 + kStores * 5 + 1;

enum Counter { kPolys, kPushes, kClipVerts, kPostPoints, kPrePoints };

const char* const kCounterName[kCounters] = {"gfxPolysDrawn", "matrixPushes",
                                             "clipPlaneVerts", "postPoints", "prePoints"};

const uint32_t* g_counter[kCounters] = {};
bool            g_haveCounters = false;

// roglv2's own accepted-polygon count, reached through the rasterizer entry-point table
// the same way every other rasterizer function is.
using PolysDrawnFn = int(__cdecl*)();
PolysDrawnFn g_polysDrawn = nullptr;

// Accumulated over the report window. GPL resets its counters per sceneRender, so these
// are summed per render and the report divides by frames -- in stereo that is deliberately
// both eyes, because both eyes are what the frame actually cost.
unsigned long long g_sum[kCounters] = {};
unsigned long long g_sumRaster = 0;
unsigned long long g_renders = 0;
int                g_lastPolysDrawn = 0;

bool findCounters(const ModuleImage& image) {
    uint8_t pattern[3] = {0x33, 0xC0, 0xA3};
    std::vector<const uint8_t*> hits =
        findAll(image.textBegin, image.textEnd, pattern, sizeof(pattern));

    const uint8_t* found = nullptr;
    int            matches = 0;
    for (const uint8_t* at : hits) {
        if (at + kResetLength > image.textEnd) continue;

        uintptr_t target[kStores];
        bool      shaped = true;
        for (int i = 0; i < kStores && shaped; ++i) {
            const uint8_t* store = at + 2 + i * 5;
            if (store[0] != 0xA3) shaped = false;
            else {
                std::memcpy(&target[i], store + 1, 4);
                if (!isWritable(image, reinterpret_cast<const void*>(target[i])))
                    shaped = false;
                else if (i && target[i] != target[i - 1] + 4) shaped = false;
            }
        }
        if (!shaped || at[2 + kStores * 5] != 0xC3) continue;

        ++matches;
        found = at;
    }

    if (matches != 1) {
        logf("profile: the render-counter reset function matched %d times, not once -- "
             "the geometry figures are unavailable on this build", matches);
        return false;
    }

    for (int i = 0; i < kCounters; ++i) {
        uintptr_t target = 0;
        std::memcpy(&target, found + 2 + i * 5 + 1, 4);
        g_counter[i] = reinterpret_cast<const uint32_t*>(target);
    }
    logf("profile: GPL's render counters are at %08X..%08X (%s first), reset by the "
         "function at %08X",
         static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_counter[0])),
         static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_counter[kCounters - 1])),
         kCounterName[0], static_cast<unsigned>(reinterpret_cast<uintptr_t>(found)));
    return true;
}

// ---------------------------------------------------------------------------------------
// roglv2's calls into the driver
// ---------------------------------------------------------------------------------------
//
// The timestamps are rdtsc: glDrawArrays is called thousands of times a frame, so each
// probe has to cost tens of cycles rather than a function call into the kernel's timer.
// The counter is unitless here. The report converts it using a cycles-to-seconds ratio
// measured across the same window, so the TSC's frequency never has to be known or
// assumed stable between windows.

unsigned long long g_drawCycles = 0;
unsigned long long g_finishCycles = 0;
unsigned long long g_finishCyclesInPresent = 0;
unsigned long long g_swapCycles = 0;
unsigned long long g_drawCalls = 0;
unsigned long long g_finishCalls = 0;
unsigned long long g_swapCalls = 0;

// A glDrawArrays call that BLOCKED, and the same figures for the part of the frame inside
// screenDisplay.
//
// Submitting a polygon is CPU work; waiting inside the driver because its command queue is
// full is not, and both happen in the same function. The two are told apart by duration:
// a call at least `profileStallUs` long is charged to the stall total whole, the rest is
// submission work. The threshold is converted to TSC cycles once a window from the same
// ratio the report uses, so the TSC's frequency is never assumed.
//
// This is a LOWER BOUND on the driver stalls in a span: glDrawArrays is the only GL entry
// point timed here, so a stall inside any other call is invisible and lands in WORK.
unsigned long long g_drawStallCycles = 0;
unsigned long long g_drawStallCyclesInPresent = 0;
unsigned long long g_drawCyclesInPresent = 0;
unsigned long long g_drawStalls = 0;
unsigned long long g_drawStallsInPresent = 0;
unsigned long long g_stallCycles = 150000;  // ~50 us at 3 GHz, until a window measures it

// GEOMETRY THAT REACHES GL ONLY AFTER THE FRAME IS OVER.
//
// roglv2 does not rasterise a polygon when polyDraw is called -- it queues it in the
// ZBuffer poly drawer's bins and emits the whole batch in one flush. The flush runs when
// a 2D call switches the batch mode, when polySetDest changes the destination, or, as a
// last resort, INSIDE screenDisplay itself. Anything left for that last resort is drawn
// after gplvr's present hook has already blitted and unbound the menu framebuffer, so it
// lands in the window instead. This counts it.
unsigned long long g_drawCallsInPresent = 0;
bool               g_askedBindingThisPresent = false;

// roglv2 issues glFinish from more than one place, and the first run of this instrument
// reported a `finish` LARGER than the `present` it was supposed to be a part of -- 6.44 ms
// inside 5.60 ms -- which is impossible and meant the attribution was wrong rather than
// the number. The present hook brackets the engine's own screenDisplay with this flag, so
// each finish is charged to the half of the frame it actually happened in.
bool g_inPresent = false;
unsigned long long g_windowCycles = 0;
unsigned long long g_windowCycleStart = 0;

// The window's cycles-to-milliseconds ratio, taken ONCE.
//
// Two report lines now convert rdtsc counts, and the conversion closes the cycle window as
// a side effect: whichever line ran first used to leave the other with a window of a few
// microseconds and figures hundreds of times too large. Taken once per report and reused,
// reset by renderProfileEndWindow.
bool   g_ratioTaken = false;
double g_msPerCycle = 0.0;

double windowMsPerCycle(double seconds) {
    if (!g_ratioTaken) {
        g_ratioTaken = true;
        unsigned long long now = __rdtsc();
        g_windowCycles = now - g_windowCycleStart;
        g_windowCycleStart = now;
        g_msPerCycle = g_windowCycles ? 1000.0 * seconds / g_windowCycles : 0.0;

        // The stall threshold in the units the thunk compares against, from this window's
        // own ratio rather than an assumed clock.
        if (g_msPerCycle > 0.0 && g_options.profileStallUs > 0)
            g_stallCycles = static_cast<unsigned long long>(
                (g_options.profileStallUs / 1000.0) / g_msPerCycle);
        else if (g_options.profileStallUs <= 0)
            g_stallCycles = ~0ull;  // nothing is a stall; every draw is submission work
    }
    return g_msPerCycle;
}

using DrawArraysFn = void(WINAPI*)(unsigned mode, int first, int count);
using FinishFn = void(WINAPI*)();
using SwapFn = BOOL(WINAPI*)(HDC);

DrawArraysFn g_realDrawArrays = nullptr;
FinishFn     g_realFinish = nullptr;
SwapFn       g_realSwap = nullptr;

void WINAPI drawArraysThunk(unsigned mode, int first, int count) {
    unsigned long long begin = __rdtsc();
    g_realDrawArrays(mode, first, count);
    unsigned long long spent = __rdtsc() - begin;
    g_drawCycles += spent;
    ++g_drawCalls;

    bool stalled = spent >= g_stallCycles;
    if (stalled) {
        g_drawStallCycles += spent;
        ++g_drawStalls;
    }

    if (g_inPresent) {
        g_drawCyclesInPresent += spent;
        if (stalled) {
            g_drawStallCyclesInPresent += spent;
            ++g_drawStallsInPresent;
        }
        ++g_drawCallsInPresent;
        // The binding is asked for once per present, at the first such call: glGetIntegerv
        // on every draw would cost more than the frame it is measuring.
        if (!g_askedBindingThisPresent) {
            g_askedBindingThisPresent = true;
            menuScaleReportBinding("the deferred polygon flush inside screenDisplay");
        }
    }
}

// Swallow roglv2's glFinish, and time whatever is left.
//
// roglv2 has exactly two glFinish call sites -- rogl_displayPresent (0x20009B7D) and
// rogl_displayFinishAndCheckError (0x20009FB4) -- and both are the same idiom:
//
//     glFinish(); if (glGetError()) throw XOpenGL(".\DisplayOpenGL.cpp", line);
//
// The finish exists to make the error check attributable, and GL records an error when a
// command is ISSUED rather than when it executes, so the synchronisation was never needed.
// SwapBuffers flushes on its own. What the finishes cost is the entire CPU/GPU overlap:
// four full pipeline drains a frame turn a frame that should cost max(CPU, GPU) into
// CPU + GPU.
//
// Timing a finish says how long the CPU sat in there. It does NOT say the GPU needed that
// long, and reading it that way produced two wrong conclusions in this project before the
// A/B settled it: with the calls dropped, scene fell 3.41 -> 2.20 ms and present 4.00 ->
// 2.68, and the frame went from 133 fps to the 144 Hz tick rate. It was serialisation.
//
// The call is never forwarded. It is still hooked rather than left alone so that the count
// stays visible in the report -- if roglv2 ever starts issuing more of them, that shows up.
// Who calls it, answered by asking rather than by reading.
//
// roglv2 has two glFinish call sites and one of them is virtual, reached through a pointer
// static analysis did not trace: the display vtable's slot 3, which nothing in the DLL
// invokes through the `rogl_display` global. Four call sites' worth of guessing (the
// mirror, the destination change, aaFlushEdges, the batch flush) produced one confirmed
// wrong answer -- aaFlushEdges runs once per matrix pop, ~1300 times a frame, against the
// 4 measured. The return address settles it in one run and costs one compare per call.
const int      kFinishSites = 8;
const uint8_t* g_finishSite[kFinishSites] = {};
int            g_finishSites = 0;
bool           g_finishSitesLogged = false;

void noteFinishSite(const uint8_t* ret) {
    for (int i = 0; i < g_finishSites; ++i)
        if (g_finishSite[i] == ret) return;
    if (g_finishSites < kFinishSites) g_finishSite[g_finishSites++] = ret;
}

void WINAPI finishThunk() {
    noteFinishSite(static_cast<const uint8_t*>(_ReturnAddress()));
    unsigned long long begin = __rdtsc();
    unsigned long long spent = __rdtsc() - begin;
    g_finishCycles += spent;
    if (g_inPresent) g_finishCyclesInPresent += spent;
    ++g_finishCalls;
}

BOOL WINAPI swapThunk(HDC dc) {
    int                previous = frameBudgetEnter(kSpanSwap);
    unsigned long long begin = __rdtsc();
    BOOL               result = g_realSwap(dc);
    g_swapCycles += __rdtsc() - begin;
    ++g_swapCalls;
    frameBudgetLeave(previous);
    return result;
}

struct DriverHook {
    const char* name;
    void*       thunk;
    void**      original;
};

// glFinish first, because it is the one hook that installs on its own: dropping the finish
// is a fix that ships, while the other two are measurement and follow `profileStages`.
const DriverHook kDriverHooks[] = {
    {"glFinish", reinterpret_cast<void*>(&finishThunk),
     reinterpret_cast<void**>(&g_realFinish)},
    {"glDrawArrays", reinterpret_cast<void*>(&drawArraysThunk),
     reinterpret_cast<void**>(&g_realDrawArrays)},
    {"SwapBuffers", reinterpret_cast<void*>(&swapThunk),
     reinterpret_cast<void**>(&g_realSwap)},
};
const int kDriverHookCount = sizeof(kDriverHooks) / sizeof(kDriverHooks[0]);

// Redirect one module's imports by name. A copy of the display-settings walker's shape,
// kept separate because that one carries a fixed replacement table and this one has to
// hand back the value it displaced.
int patchImportsByName(HMODULE module, const DriverHook* hooks, int count) {
    ModuleImage image;
    if (!getModuleImage(module, image)) return 0;

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return 0;

    int taken = 0;
    auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        image.base + directory.VirtualAddress);
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

            for (int i = 0; i < count; ++i) {
                if (std::strcmp(reinterpret_cast<const char*>(import->Name),
                                hooks[i].name) != 0)
                    continue;

                auto value = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(hooks[i].thunk));
                if (slots->u1.Function == value) {
                    ++taken;
                    break;
                }
                // Save what is there before overwriting it: the thunk forwards through
                // this, so a failed write must leave the original untouched.
                void* displaced = reinterpret_cast<void*>(slots->u1.Function);
                if (patchBytes(&slots->u1.Function, &value, sizeof(value))) {
                    *hooks[i].original = displaced;
                    ++taken;
                    logf("profile: roglv2 imports %s -> timed", hooks[i].name);
                } else {
                    logf("profile: could not write roglv2's %s import slot", hooks[i].name);
                }
                break;
            }
        }
    }
    return taken;
}

// ---------------------------------------------------------------------------------------
// What `pre` is made of
// ---------------------------------------------------------------------------------------
//
// `pre` is present-returns -> sceneRender-entry, and at 2560x1440 flat it is 3.4-4.7 ms of
// a 6.95 ms frame -- bigger than the scene, the HUD and the present put together. The four
// top spans tile the frame, so the number is trustworthy; what it MEANS was never
// established, and "the game's own loop: sim, input, wait" is a list of guesses.
//
// The container settles it. gpl_fgEventLoop is the game's foreground loop and one
// iteration of it is one frame:
//
//     gpl_fgFrameSnapshotBegin()          <- WAITS HERE for the next simulation tick
//       gpl_fgWaitForTickAndSnapshot()
//           while (bgIsRunning() && getBGCount() == lastLatched) Sleep(1);
//           fgDisableBG();                <- spins on the BG lock, also Sleep(1)
//           lastLatched = getBGCount();
//           ...run the registered capture callbacks...
//           fgEnableBG();
//     while (eventDequeue(&e)) dispatch(e);   <- platform queue, controller poll, and one
//                                                synthetic tick event per elapsed BG tick
//     state->vt[3]()                          <- the application state's per-frame update
//     snapshotNotify(); snapshotNotify();
//     gpl_presentIfForeground()               <- window redraw: dirty rects, widget walk,
//       windowRedrawAndPresent()                 the 3D view widget, gpl_simFrameRender,
//         ... -> gpl_sceneRender                 gpl_sceneRender, the HUD, screenDisplay
//         ... -> screenDisplay
//     postFrameCallbacks(); snapshotEnd(); serviceBGRequests();
//
// THE FIRST THING IN THE LOOP IS A SLEEP(1) POLL FOR THE NEXT SIMULATION TICK. That is why
// the frame rate equals the task rate exactly, with vsync off and no other throttle, and it
// means most of `pre` is very probably HEADROOM rather than cost.
//
// "Very probably" is not a measurement, and reading a large stall as proof of where the
// work is has already produced two wrong answers in this project. So the wait gets its own
// span, and so does the second wait right behind it: fgDisableBG is itself a Sleep(1) spin
// on a lock the simulation thread holds while it steps, so a frame can also be late because
// the SIM overran, which is a completely different diagnosis with a completely different
// fix. The two are adjacent in the source and indistinguishable in the total.
//
// Five markers, each hit once per frame, each a QueryPerformanceCounter through a naked
// thunk on a call site. Nothing is wrapped and nothing returns through us -- the thunk
// timestamps and jumps to the original, so the calling convention never has to be known and
// the return path is untouched.
//
// Discovery keys off four names gplc67.exe EXPORTS -- bgIsRunning, fgDisableBG, getBGCount
// and the rasterizer's screenDisplay global -- and every step asserts its cardinality:
//
//   * exactly one function calls BOTH bgIsRunning and fgDisableBG        -> the tick wait
//   * it contains exactly one fgDisableBG call                           -> marker 1
//   * and exactly one getBGCount call after it                           -> marker 2
//   * the tick wait has exactly one caller                               -> snapshot begin
//   * snapshot begin has exactly one caller                              -> the frame loop
//   * the first call in the loop after that one                          -> event dequeue
//   * exactly one callee of the loop is called more than once in it      -> the present gate
//
// and the last two are cross-checked structurally rather than trusted: the event dequeue
// must reach a function that calls getBGCount (it synthesises the tick events from it), and
// the present gate must reach the function holding the screenDisplay call. If any step
// fails, nothing is patched and the breakdown simply does not appear.

enum PreMark {
    kMarkLoopBegin = 0,  // the loop's snapshot-begin call: the wait starts here
    kMarkTickArrived,    // fgDisableBG inside it: the sim tick has landed
    kMarkBgLocked,       // getBGCount right after it: the BG lock has been taken
    kMarkPump,           // the first event dequeue: the snapshot is latched
    kMarkRedraw,         // the present gate: the window redraw begins
    kPreMarks
};

long long g_preMark[kPreMarks] = {};
bool      g_preInstalled = false;

// One original per marker rather than an array, so every jump in the thunks below is a
// plain `[symbol]` with no displacement arithmetic to get wrong.
void* g_preOriginal0 = nullptr;
void* g_preOriginal1 = nullptr;
void* g_preOriginal2 = nullptr;
void* g_preOriginal3 = nullptr;
void* g_preOriginal4 = nullptr;

// The loop marker clears the rest, and every other marker takes the FIRST hit of the frame.
// The event dequeue is called once per event plus once more to terminate, and the present
// gate appears at three call sites in the loop; first-hit-wins makes both single-valued
// without the thunks having to know which is which.
// The frame-budget span each marker opens, on the same timestamp and under the same
// first-hit rule.
const BudgetSpan kPreMarkSpan[kPreMarks] = {kSpanWaitTick, kSpanWaitBg, kSpanLatch, kSpanLogic,
                                            kSpanRedraw};

void __stdcall preMarkImpl(int which) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (which == kMarkLoopBegin) {
        for (int i = 0; i < kPreMarks; ++i) g_preMark[i] = 0;
        g_preMark[kMarkLoopBegin] = now.QuadPart;
        frameBudgetSwitchAt(kPreMarkSpan[which], now.QuadPart);
    } else if (!g_preMark[which]) {
        g_preMark[which] = now.QuadPart;
        frameBudgetSwitchAt(kPreMarkSpan[which], now.QuadPart);
    }
}

using PreMarkFn = void(__stdcall*)(int);
PreMarkFn g_preMarkFn = &preMarkImpl;

// pushad/pushfd around the timestamp and then a jump to the original: every register and
// flag the caller could care about is restored, the callee's arguments are exactly where it
// left them, and the return address is untouched -- so this works whatever the convention
// and whatever the argument count, which is the same reasoning the polyDraw counter thunk
// uses. Cost is one QueryPerformanceCounter, five times a frame.
__declspec(naked) void preThunk0() {
    __asm { pushad
            pushfd
            push 0
            call dword ptr [g_preMarkFn]
            popfd
            popad
            jmp  dword ptr [g_preOriginal0] }
}

__declspec(naked) void preThunk1() {
    __asm { pushad
            pushfd
            push 1
            call dword ptr [g_preMarkFn]
            popfd
            popad
            jmp  dword ptr [g_preOriginal1] }
}

__declspec(naked) void preThunk2() {
    __asm { pushad
            pushfd
            push 2
            call dword ptr [g_preMarkFn]
            popfd
            popad
            jmp  dword ptr [g_preOriginal2] }
}

__declspec(naked) void preThunk3() {
    __asm { pushad
            pushfd
            push 3
            call dword ptr [g_preMarkFn]
            popfd
            popad
            jmp  dword ptr [g_preOriginal3] }
}

__declspec(naked) void preThunk4() {
    __asm { pushad
            pushfd
            push 4
            call dword ptr [g_preMarkFn]
            popfd
            popad
            jmp  dword ptr [g_preOriginal4] }
}

// The previous frame's TAIL, after the present gate returns: the post-frame callbacks, the
// snapshot end, and the BG request service. Same thunk shape; these only open frame-budget
// spans and leave the `pre` markers alone.
void* g_tailOriginal0 = nullptr;
void* g_tailOriginal1 = nullptr;
void* g_tailOriginal2 = nullptr;

void __stdcall tailMarkImpl(int which) {
    static const BudgetSpan kTailSpan[3] = {kSpanTailCallbacks, kSpanTailSnapshot,
                                            kSpanTailService};
    frameBudgetSwitch(kTailSpan[which]);
}

PreMarkFn g_tailMarkFn = &tailMarkImpl;

// gpl_fgFrameSnapshotEnd's release takes the BG lock before running the release callbacks,
// and fgDisableBG waits for the simulation thread to let go of it. That wait is charged to
// bgRelease; the callbacks stay in the snapshot span. fgDisableBG takes and returns nothing,
// and its one caller reloads everything it needs from the stack after the call, so an
// ordinary function can stand in for it.
typedef void(__cdecl* VoidFn)();
VoidFn g_releaseDisableOriginal = nullptr;

void __cdecl releaseDisableThunk() {
    int previous = frameBudgetEnter(kSpanWaitBgRelease);
    g_releaseDisableOriginal();
    frameBudgetLeave(previous);
}

__declspec(naked) void tailThunk0() {
    __asm { pushad
            pushfd
            push 0
            call dword ptr [g_tailMarkFn]
            popfd
            popad
            jmp  dword ptr [g_tailOriginal0] }
}

__declspec(naked) void tailThunk1() {
    __asm { pushad
            pushfd
            push 1
            call dword ptr [g_tailMarkFn]
            popfd
            popad
            jmp  dword ptr [g_tailOriginal1] }
}

__declspec(naked) void tailThunk2() {
    __asm { pushad
            pushfd
            push 2
            call dword ptr [g_tailMarkFn]
            popfd
            popad
            jmp  dword ptr [g_tailOriginal2] }
}

// Instruction patterns are only meaningful in executable sections; the same rule
// Discovery.cpp works to.
std::vector<const uint8_t*> findInCode(const ModuleImage& image, const void* pattern,
                                       size_t length) {
    std::vector<const uint8_t*> hits;
    for (const Section& section : image.sections) {
        if (!section.executable || !section.hasRawData) continue;
        std::vector<const uint8_t*> found =
            findAll(section.begin, section.end, pattern, length);
        hits.insert(hits.end(), found.begin(), found.end());
    }
    return hits;
}

std::vector<uintptr_t> ownersOf(const std::vector<uintptr_t>& entries,
                                const std::vector<uintptr_t>& sites) {
    std::vector<uintptr_t> owners;
    for (uintptr_t site : sites) {
        uintptr_t owner = functionOf(entries, site);
        if (!owner) continue;
        if (std::find(owners.begin(), owners.end(), owner) == owners.end())
            owners.push_back(owner);
    }
    return owners;
}

std::vector<uintptr_t> sitesInside(const std::vector<uintptr_t>& sites, uintptr_t begin,
                                   uintptr_t end) {
    std::vector<uintptr_t> inside;
    for (uintptr_t site : sites)
        if (site >= begin && site < end) inside.push_back(site);
    return inside;
}

// Every near call or near jump out of [begin, end). Used for the two cross-checks, because
// the compiler turns a tail call into E9 and a caller/callee test that only looks at E8
// silently misses it -- which it does here, for the present gate.
std::vector<uintptr_t> branchTargets(const ModuleImage& image, uintptr_t begin,
                                     uintptr_t end) {
    std::vector<uintptr_t> targets;
    auto p = reinterpret_cast<const uint8_t*>(begin);
    auto limit = reinterpret_cast<const uint8_t*>(end);
    if (limit > image.textEnd) limit = image.textEnd;
    if (limit < p + 5) return targets;

    for (; p <= limit - 5; ++p) {
        if (*p != 0xE8 && *p != 0xE9) continue;
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        targets.push_back(reinterpret_cast<uintptr_t>(p) + 5 + rel);
    }
    return targets;
}

bool reachesAnyOf(const ModuleImage& image, uintptr_t begin, uintptr_t end,
                  const std::vector<uintptr_t>& wanted) {
    for (uintptr_t target : branchTargets(image, begin, end))
        if (std::find(wanted.begin(), wanted.end(), target) != wanted.end()) return true;
    return false;
}

// Target of the first near call after `after` and before `end` whose destination is a known
// function entry. Requiring the destination to be an entry is what stops a stray 0xE8 inside
// some other instruction's operand from being taken for a call.
uintptr_t firstCallAfter(const ModuleImage& image, const std::vector<uintptr_t>& entries,
                         uintptr_t after, uintptr_t end) {
    auto p = reinterpret_cast<const uint8_t*>(after);
    auto limit = reinterpret_cast<const uint8_t*>(end);
    if (limit > image.textEnd) limit = image.textEnd;
    if (limit < p + 5) return 0;

    for (; p <= limit - 5; ++p) {
        if (*p != 0xE8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        uintptr_t target = reinterpret_cast<uintptr_t>(p) + 5 + rel;
        if (std::binary_search(entries.begin(), entries.end(), target)) return target;
    }
    return 0;
}

bool redirectSites(const std::vector<uintptr_t>& sites, void* thunk, const char* what) {
    if (sites.empty()) return false;
    for (uintptr_t site : sites) {
        int32_t rel =
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
        if (!patchBytes(reinterpret_cast<void*>(site + 1), &rel, sizeof(rel))) {
            logf("pre: could not redirect the %s call site at %08X", what,
                 static_cast<unsigned>(site));
            return false;
        }
    }
    return true;
}

// The three tail markers, in loop order: the post-frame callbacks, the snapshot end, the
// BG request check. gpl_fgEventLoop runs, after its first present gate:
//
//     call gpl_presentIfForeground
//     ...  call gpl_fgRunPostFrameCallbacks  |  call gate; call gate
//     call gpl_fgFrameSnapshotEnd
//     call bgHasRequestedFGAction            <- exported
//
// so the exported call is the anchor, the call immediately before it is the snapshot end,
// and the first call after the first gate site is the callbacks. Each is asserted.
struct TailSites {
    uintptr_t site[3] = {};
    uintptr_t target[3] = {};
};

bool locateTailMarkers(const ModuleImage& image, const std::vector<uintptr_t>& entries,
                       uintptr_t loop, uintptr_t loopEnd, uintptr_t gate,
                       const std::vector<uintptr_t>& gateSites, TailSites& out) {
    HMODULE exe = GetModuleHandleA(nullptr);
    auto    requested = exe ? reinterpret_cast<uintptr_t>(GetProcAddress(
                                  exe, "?bgHasRequestedFGAction@@YA_NXZ"))
                            : 0;
    if (!requested || gateSites.empty()) {
        logf("pre: bgHasRequestedFGAction is not exported -- the tail is not split");
        return false;
    }

    std::vector<uintptr_t> serviceSites =
        sitesInside(findCallSites(image, requested), loop, loopEnd);
    if (serviceSites.size() != 1) {
        logf("pre: the frame loop calls bgHasRequestedFGAction %zu times, not once -- the "
             "tail is not split",
             serviceSites.size());
        return false;
    }
    uintptr_t serviceSite = serviceSites[0];
    uintptr_t snapshotSite = serviceSite - 5;

    auto    snapshotCall = reinterpret_cast<const uint8_t*>(snapshotSite);
    int32_t rel = 0;
    std::memcpy(&rel, snapshotCall + 1, sizeof(rel));
    uintptr_t snapshotEnd = serviceSite + rel;
    if (snapshotCall[0] != 0xE8 ||
        !std::binary_search(entries.begin(), entries.end(), snapshotEnd)) {
        logf("pre: the instruction before the BG request check at %08X is not a call -- "
             "the tail is not split",
             static_cast<unsigned>(serviceSite));
        return false;
    }

    uintptr_t firstGate = *std::min_element(gateSites.begin(), gateSites.end());
    uintptr_t callbacks = firstCallAfter(image, entries, firstGate + 5, snapshotSite);
    std::vector<uintptr_t> callbackSites;
    if (callbacks)
        callbackSites = sitesInside(findCallSites(image, callbacks), firstGate + 5, snapshotSite);
    if (!callbacks || callbacks == gate || callbacks == snapshotEnd ||
        callbackSites.size() != 1) {
        logf("pre: no single post-frame callback call between the present gate at %08X and "
             "the snapshot end -- the tail is not split",
             static_cast<unsigned>(firstGate));
        return false;
    }

    out.site[0] = callbackSites[0];
    out.site[1] = snapshotSite;
    out.site[2] = serviceSite;
    out.target[0] = callbacks;
    out.target[1] = snapshotEnd;
    out.target[2] = requested;
    return true;
}

bool installPreMarkers(const ModuleImage& image) {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;

    // The mangled names are the contract: MSVC's decoration encodes the signature, so a
    // build that changed one of these fails the lookup instead of calling something with
    // the wrong shape. TaskRate.cpp resolves the rest of this API the same way.
    auto bgIsRunning =
        reinterpret_cast<uintptr_t>(GetProcAddress(exe, "?bgIsRunning@@YA_NXZ"));
    auto fgDisableBG =
        reinterpret_cast<uintptr_t>(GetProcAddress(exe, "?fgDisableBG@@YAXXZ"));
    auto getBGCount =
        reinterpret_cast<uintptr_t>(GetProcAddress(exe, "?getBGCount@@YAHXZ"));
    if (!bgIsRunning || !fgDisableBG || !getBGCount) {
        logf("pre: the exe does not export bgIsRunning/fgDisableBG/getBGCount, so the "
             "frame loop cannot be located -- no pre breakdown");
        return false;
    }

    void** displaySlot = rasterGlobal(image, "screenDisplay");
    if (!displaySlot) {
        logf("pre: the screenDisplay global did not resolve -- no pre breakdown");
        return false;
    }

    const std::vector<uintptr_t> entries = collectCallTargets(image);
    const std::vector<uintptr_t> runSites = findCallSites(image, bgIsRunning);
    const std::vector<uintptr_t> disableSites = findCallSites(image, fgDisableBG);
    const std::vector<uintptr_t> countSites = findCallSites(image, getBGCount);

    // 1. The tick wait: the one function that calls both bgIsRunning and fgDisableBG.
    uintptr_t wait = 0;
    int       waitMatches = 0;
    for (uintptr_t owner : ownersOf(entries, runSites)) {
        uintptr_t end = functionEnd(entries, image, owner);
        if (sitesInside(disableSites, owner, end).empty()) continue;
        ++waitMatches;
        wait = owner;
    }
    if (waitMatches != 1) {
        logf("pre: %d functions call both bgIsRunning and fgDisableBG, not one -- the "
             "frame loop's shape has changed, so nothing is patched",
             waitMatches);
        return false;
    }
    uintptr_t waitEnd = functionEnd(entries, image, wait);

    // 2. Its single fgDisableBG call, and 3. the single getBGCount call after it.
    std::vector<uintptr_t> disableInside = sitesInside(disableSites, wait, waitEnd);
    if (disableInside.size() != 1) {
        logf("pre: the tick wait at %08X holds %zu fgDisableBG calls, not one",
             static_cast<unsigned>(wait), disableInside.size());
        return false;
    }
    std::vector<uintptr_t> countAfter =
        sitesInside(countSites, disableInside[0] + 5, waitEnd);
    if (countAfter.size() != 1) {
        logf("pre: the tick wait at %08X holds %zu getBGCount calls after the lock, not "
             "one",
             static_cast<unsigned>(wait), countAfter.size());
        return false;
    }

    // 4. The tick wait has one caller -- the snapshot begin -- which itself has one
    //    caller, the frame loop.
    std::vector<uintptr_t> waitSites = findCallSites(image, wait);
    if (waitSites.size() != 1) {
        logf("pre: the tick wait has %zu callers, not one", waitSites.size());
        return false;
    }
    uintptr_t snapshotBegin = functionOf(entries, waitSites[0]);
    std::vector<uintptr_t> beginSites =
        snapshotBegin ? findCallSites(image, snapshotBegin) : std::vector<uintptr_t>();
    if (beginSites.size() != 1) {
        logf("pre: the snapshot begin at %08X has %zu callers, not one",
             static_cast<unsigned>(snapshotBegin), beginSites.size());
        return false;
    }
    uintptr_t loop = functionOf(entries, beginSites[0]);
    if (!loop) {
        logf("pre: the snapshot begin's call site at %08X is in no known function",
             static_cast<unsigned>(beginSites[0]));
        return false;
    }
    uintptr_t loopEnd = functionEnd(entries, image, loop);

    // 5. The event dequeue: the first call in the loop after the snapshot begin. Checked,
    //    not assumed -- it must reach a function that calls getBGCount, because the pump
    //    synthesises one tick event per elapsed simulation tick and getBGCount is where
    //    that number comes from.
    uintptr_t dequeue = firstCallAfter(image, entries, beginSites[0] + 5, loopEnd);
    if (!dequeue ||
        !reachesAnyOf(image, dequeue, functionEnd(entries, image, dequeue),
                      ownersOf(entries, countSites))) {
        logf("pre: the call after the snapshot begin (%08X) does not reach getBGCount, so "
             "it is not the event pump",
             static_cast<unsigned>(dequeue));
        return false;
    }

    // 6. The present gate: the only callee the loop calls more than once. Checked against
    //    the rasterizer's own screenDisplay global -- the gate must reach, by call OR by
    //    tail jump, the function that issues it.
    uint8_t indirect[6] = {0xFF, 0x15};
    auto    slotAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(displaySlot));
    std::memcpy(indirect + 2, &slotAddress, sizeof(slotAddress));
    std::vector<uintptr_t> displaySites;
    for (const uint8_t* at : findInCode(image, indirect, sizeof(indirect)))
        displaySites.push_back(reinterpret_cast<uintptr_t>(at));
    const std::vector<uintptr_t> displayOwners = ownersOf(entries, displaySites);

    std::vector<uintptr_t> calledTargets;
    std::vector<int>       calledCounts;
    {
        auto p = reinterpret_cast<const uint8_t*>(loop);
        auto limit = reinterpret_cast<const uint8_t*>(loopEnd);
        if (limit > image.textEnd) limit = image.textEnd;
        for (; p + 5 <= limit; ++p) {
            if (*p != 0xE8) continue;
            int32_t rel = 0;
            std::memcpy(&rel, p + 1, sizeof(rel));
            uintptr_t target = reinterpret_cast<uintptr_t>(p) + 5 + rel;
            if (!std::binary_search(entries.begin(), entries.end(), target)) continue;
            auto found = std::find(calledTargets.begin(), calledTargets.end(), target);
            if (found == calledTargets.end()) {
                calledTargets.push_back(target);
                calledCounts.push_back(1);
            } else {
                ++calledCounts[found - calledTargets.begin()];
            }
        }
    }
    uintptr_t gate = 0;
    int       gateMatches = 0;
    for (size_t i = 0; i < calledTargets.size(); ++i) {
        if (calledCounts[i] < 2) continue;
        ++gateMatches;
        gate = calledTargets[i];
    }
    if (gateMatches != 1 ||
        !reachesAnyOf(image, gate, functionEnd(entries, image, gate), displayOwners)) {
        logf("pre: %d loop callees are called more than once and the survivor (%08X) does "
             "not reach screenDisplay -- the present gate is not identifiable",
             gateMatches, static_cast<unsigned>(gate));
        return false;
    }

    // Everything is identified. Patch, and stop at the first failure with the originals
    // still in place -- a half-installed set of markers would tile nothing.
    g_preOriginal0 = reinterpret_cast<void*>(snapshotBegin);
    g_preOriginal1 = reinterpret_cast<void*>(fgDisableBG);
    g_preOriginal2 = reinterpret_cast<void*>(getBGCount);
    g_preOriginal3 = reinterpret_cast<void*>(dequeue);
    g_preOriginal4 = reinterpret_cast<void*>(gate);

    // Every site is located before the first byte is written: a scan run between two
    // patches would be reading an image we had already changed.
    const std::vector<uintptr_t> dequeueSites =
        sitesInside(findCallSites(image, dequeue), loop, loopEnd);
    const std::vector<uintptr_t> gateSites =
        sitesInside(findCallSites(image, gate), loop, loopEnd);
    TailSites tail;
    bool      tailLocated =
        locateTailMarkers(image, entries, loop, loopEnd, gate, gateSites, tail);

    // The lock inside the snapshot end's release: the one callee of the snapshot end that
    // holds exactly one fgDisableBG call, other than the tick wait's.
    uintptr_t releaseDisableSite = 0;
    if (tailLocated) {
        uintptr_t snapshotEnd = tail.target[1];
        for (uintptr_t target :
             branchTargets(image, snapshotEnd, functionEnd(entries, image, snapshotEnd))) {
            if (!std::binary_search(entries.begin(), entries.end(), target)) continue;
            std::vector<uintptr_t> inside =
                sitesInside(disableSites, target, functionEnd(entries, image, target));
            if (inside.size() == 1 && inside[0] != disableInside[0]) {
                releaseDisableSite = inside[0];
                break;
            }
        }
    }

    if (!redirectSites(beginSites, reinterpret_cast<void*>(&preThunk0), "snapshot begin") ||
        !redirectSites(disableInside, reinterpret_cast<void*>(&preThunk1), "tick wait") ||
        !redirectSites(countAfter, reinterpret_cast<void*>(&preThunk2), "bg lock") ||
        !redirectSites(dequeueSites, reinterpret_cast<void*>(&preThunk3), "event dequeue") ||
        !redirectSites(gateSites, reinterpret_cast<void*>(&preThunk4), "present gate"))
        return false;

    logf("pre: the frame loop is at %08X..%08X. Markers on snapshotBegin %08X (site "
         "%08X), the tick wait %08X (fgDisableBG site %08X, bgLock site %08X), the event "
         "pump %08X, and the present gate %08X (%zu sites).",
         static_cast<unsigned>(loop), static_cast<unsigned>(loopEnd),
         static_cast<unsigned>(snapshotBegin), static_cast<unsigned>(beginSites[0]),
         static_cast<unsigned>(wait), static_cast<unsigned>(disableInside[0]),
         static_cast<unsigned>(countAfter[0]), static_cast<unsigned>(dequeue),
         static_cast<unsigned>(gate), gateSites.size());
    logf("pre: the first thing the loop does is Sleep(1) until getBGCount() advances, so "
         "the frame rate IS the task rate and most of `pre` is expected to be idle. The "
         "two WAIT spans on the report line are that sleep and the fgDisableBG spin "
         "behind it; anything they do not account for is real work.");

    if (tailLocated) {
        g_tailOriginal0 = reinterpret_cast<void*>(tail.target[0]);
        g_tailOriginal1 = reinterpret_cast<void*>(tail.target[1]);
        g_tailOriginal2 = reinterpret_cast<void*>(tail.target[2]);
        if (redirectSites(std::vector<uintptr_t>(1, tail.site[0]),
                          reinterpret_cast<void*>(&tailThunk0), "post-frame callbacks") &&
            redirectSites(std::vector<uintptr_t>(1, tail.site[1]),
                          reinterpret_cast<void*>(&tailThunk1), "snapshot end") &&
            redirectSites(std::vector<uintptr_t>(1, tail.site[2]),
                          reinterpret_cast<void*>(&tailThunk2), "BG request check"))
            logf("pre: tail markers on the post-frame callbacks %08X (site %08X), the "
                 "snapshot end %08X (site %08X) and the BG request check (site %08X)",
                 static_cast<unsigned>(tail.target[0]), static_cast<unsigned>(tail.site[0]),
                 static_cast<unsigned>(tail.target[1]), static_cast<unsigned>(tail.site[1]),
                 static_cast<unsigned>(tail.site[2]));

        g_releaseDisableOriginal = reinterpret_cast<VoidFn>(fgDisableBG);
        if (releaseDisableSite &&
            redirectSites(std::vector<uintptr_t>(1, releaseDisableSite),
                          reinterpret_cast<void*>(&releaseDisableThunk),
                          "snapshot release lock"))
            logf("pre: the snapshot release's BG lock (site %08X) is timed as bgRelease",
                 static_cast<unsigned>(releaseDisableSite));
        else
            logf("pre: the snapshot release's BG lock was not found -- it stays inside "
                 "tail.snapshot");
    }
    return true;
}

}  // namespace

void renderProfileDiscover(const Binding& binding) {
    if (!profilingWorkWait()) return;

    g_haveCounters = findCounters(binding.image);

    void** slot = rasterGlobal(binding.image, "polysDrawn");
    if (slot && *slot) {
        g_polysDrawn = reinterpret_cast<PolysDrawnFn>(*slot);
        g_lastPolysDrawn = g_polysDrawn();
    } else {
        logf("profile: polysDrawn did not resolve -- the rasterizer's own polygon count "
             "will not be reported, so dropped polygons cannot be counted");
    }

    // Under the same key rather than its own. `pre` is the largest of the boxes
    // profileStages exists to open -- larger than scene, post and present together -- so
    // the same session that wants the geometry counts and the present split wants this,
    // and a reader looking at one line without the other would be missing the term that
    // dominates. It is also by far the cheapest of the three: five timestamps a frame
    // against an rdtsc pair on every one of ~5,000 glDrawArrays calls.
    g_preInstalled = installPreMarkers(binding.image);
}

void renderProfileInstallDriverThunks() {
    // glFinish alone unless the measurement is wanted too. The finish's two call sites
    // exist only to make a glGetError attributable, GL records errors at command issue
    // rather than execution, and SwapBuffers flushes by itself -- so the drain bought
    // nothing and cost the whole CPU/GPU overlap.
    //
    int wanted = profilingWorkWait() ? kDriverHookCount : 1;

    HMODULE roglv2 = GetModuleHandleA("roglv2.dll");
    if (!roglv2) {
        logf("raster: roglv2.dll is not loaded, so its glFinish calls cannot be dropped");
        return;
    }

    int taken = patchImportsByName(roglv2, kDriverHooks, wanted);
    if (taken != wanted)
        logf("raster: only %d of %d driver imports were taken", taken, wanted);
    else if (wanted == 1)
        logf("raster: roglv2's glFinish is DROPPED -- four pipeline drains a frame were "
             "serialising the CPU against the GPU");
    else
        logf("raster: glFinish is DROPPED; glDrawArrays and SwapBuffers are timed");

    g_windowCycleStart = __rdtsc();
}

namespace {

// ---------------------------------------------------------------------------------------
// The hitch log
// ---------------------------------------------------------------------------------------
//
// A per-second average cannot find a stall. A single 102 ms frame inside a one-second window
// reads as "128 fps" and nothing else, and every span that window reports is an average over
// 130 healthy frames plus one sick one. So keep the sick ones whole.
//
// The four spans come from hooks that already exist, and between them they tile the frame:
//
//   [present returns] --pre--> [sceneRender entry] --scene--> [sceneRender returns]
//        --post--> [present entry] --present--> [present returns]
//
//   pre      the game's own frame loop: the sim step, input, frame setup, and any wait
//            for the next tick. This is where `other` mostly lives.
//   scene    the world, both passes, both eyes
//   post     the HUD and the 2D overlays, which GPL draws after sceneRender returns
//   present  roglv2's flush and SwapBuffers
//
// Nothing is written from the hot path. A stalled frame is recorded into a fixed slot and
// printed by the per-second report, because opening a log file inside the hitch you are
// trying to measure would be measuring the instrument.

struct Hitch {
    double total = 0.0;
    double pre = 0.0;
    double scene = 0.0;
    double post = 0.0;
    double present = 0.0;
};

// The worst frame of the window, kept whole.
//
// This started as a separate "hitch log" with a millisecond threshold and four slots, and
// that was the wrong shape twice over: at any threshold near the frame period it printed
// every frame, and above it, it printed nothing on a healthy lap. The frame line already
// reports `worst`; what it never said was what the worst frame was MADE OF. So keep one
// sample, break it into the same four spans as the mean, and put it on the same line.
// No threshold to tune, nothing to enable, and it is always the frame worth looking at.
Hitch g_worst;

// The same four spans, summed, for the ordinary per-second line. This is the part that
// earns its keep: `other` was the largest term in the frame and it was a SUBTRACTION, so
// nobody could say what it held. pre + post IS other, measured instead of inferred.
double   g_sumPre = 0.0;
double   g_sumPost = 0.0;
unsigned g_spanFrames = 0;

// The other two spans and the whole frame, summed over the same frames, so the work/wait
// line can tile the frame without borrowing the caller's own scene and present totals --
// which are summed over RENDERS and over a different set of frames.
double g_sumScene = 0.0;
double g_sumPresent = 0.0;
double g_sumFrame = 0.0;

long long g_qpcFreq = 0;
long long g_lastPresentEnd = 0;
long long g_sceneBegin = 0;
long long g_sceneEnd = 0;

// The FIRST sceneRender entry of the frame, as against g_sceneBegin which is the last.
// In stereo they differ by a whole eye, and `pre` is measured to the last one -- so
// without this the eye renders between them would be charged to the redraw span and the
// sub-spans would still tile while saying something false.
long long g_sceneFirstBegin = 0;

// The sub-spans of `pre`, in the order they happen.
enum PreSpan {
    kPreTail = 0,    // the previous iteration's tail: post-frame callbacks, BG requests
    kPreWaitTick,    // IDLE -- Sleep(1) until the simulation tick advances
    kPreWaitBg,      // IDLE -- Sleep(1) spinning for the BG lock
    kPreLatch,       // the sim-state capture callbacks, BG suspended
    kPreLogic,       // event pump, controller poll, dispatch, the state's frame update
    kPreRedraw,      // dirty rects, the widget walk, the mirror instance and camera
    kPreEyes,        // earlier eye renders, when sceneRender ran more than once
    kPreSpans
};

double   g_preSum[kPreSpans] = {};
double   g_preTotal = 0.0;
unsigned g_preFrames = 0;
unsigned g_preOrdered = 0;

double msBetween(long long a, long long b) {
    if (!g_qpcFreq || b <= a) return 0.0;
    return 1000.0 * static_cast<double>(b - a) / g_qpcFreq;
}

// One frame's worth, only if the five markers and the two scene timestamps came out in the
// order the loop guarantees. They do not always: with frameRate = 2 the scene is rendered
// again AFTER the present, so the frame's first render lands before its own loop iteration
// began, and every span computed from it would be nonsense. Counting the rejects and
// printing the ratio is what turns that from a silent lie into a visible condition.
void notePreSpans(long long previousPresentEnd) {
    ++g_preFrames;

    long long mark[kPreMarks];
    for (int i = 0; i < kPreMarks; ++i) {
        mark[i] = g_preMark[i];
        if (!mark[i]) return;
    }
    if (previousPresentEnd > mark[kMarkLoopBegin]) return;
    for (int i = 1; i < kPreMarks; ++i)
        if (mark[i - 1] > mark[i]) return;
    if (mark[kMarkRedraw] > g_sceneFirstBegin || g_sceneFirstBegin > g_sceneBegin) return;

    g_preSum[kPreTail] += msBetween(previousPresentEnd, mark[kMarkLoopBegin]);
    g_preSum[kPreWaitTick] += msBetween(mark[kMarkLoopBegin], mark[kMarkTickArrived]);
    g_preSum[kPreWaitBg] += msBetween(mark[kMarkTickArrived], mark[kMarkBgLocked]);
    g_preSum[kPreLatch] += msBetween(mark[kMarkBgLocked], mark[kMarkPump]);
    g_preSum[kPreLogic] += msBetween(mark[kMarkPump], mark[kMarkRedraw]);
    g_preSum[kPreRedraw] += msBetween(mark[kMarkRedraw], g_sceneFirstBegin);
    g_preSum[kPreEyes] += msBetween(g_sceneFirstBegin, g_sceneBegin);
    g_preTotal += msBetween(previousPresentEnd, g_sceneBegin);
    ++g_preOrdered;
}

}  // namespace

void renderProfileSetInPresent(bool inPresent) {
    g_inPresent = inPresent;
    if (inPresent) g_askedBindingThisPresent = false;
}

unsigned long long renderProfileDrawCycles() { return g_drawCycles; }

void renderProfileNoteScene(long long begin, long long end) {
    if (!g_sceneFirstBegin) g_sceneFirstBegin = begin;
    g_sceneBegin = begin;
    g_sceneEnd = end;
}

bool renderProfileTakeSpans(FrameSpans& mean, FrameSpans& worst) {
    if (!g_spanFrames) return false;
    mean.pre = g_sumPre / g_spanFrames;
    mean.post = g_sumPost / g_spanFrames;
    mean.scene = 0.0;
    mean.present = 0.0;
    mean.total = 0.0;

    worst.pre = g_worst.pre;
    worst.scene = g_worst.scene;
    worst.post = g_worst.post;
    worst.present = g_worst.present;
    worst.total = g_worst.total;
    return true;
}

void renderProfileNoteFrame(long long presentBegin, long long presentEnd) {
    if (!g_qpcFreq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart;
    }

    // The first frame after a load has no predecessor to measure `pre` against, and a frame
    // whose scene began before the last present ended is a menu frame that never rendered.
    if (g_lastPresentEnd && g_sceneBegin > g_lastPresentEnd) {
        Hitch sample;
        sample.total = msBetween(g_lastPresentEnd, presentEnd);
        sample.pre = msBetween(g_lastPresentEnd, g_sceneBegin);
        sample.scene = msBetween(g_sceneBegin, g_sceneEnd);
        sample.post = msBetween(g_sceneEnd, presentBegin);
        sample.present = msBetween(presentBegin, presentEnd);

        g_sumPre += sample.pre;
        g_sumPost += sample.post;
        g_sumScene += sample.scene;
        g_sumPresent += sample.present;
        g_sumFrame += sample.total;
        ++g_spanFrames;

        if (sample.total > g_worst.total) g_worst = sample;

        if (g_preInstalled) notePreSpans(g_lastPresentEnd);
    }

    g_lastPresentEnd = presentEnd;
    // Cleared here rather than at the top of the next render: the frame boundary is the
    // present, and an extra render issued after it belongs to the frame it precedes.
    g_sceneFirstBegin = 0;
}

int renderProfilePolysDrawn() { return g_polysDrawn ? g_polysDrawn() : -1; }

void renderProfileAfterRender() {
    if (!profilingWorkWait()) return;

    ++g_renders;
    if (g_haveCounters)
        for (int i = 0; i < kCounters; ++i) g_sum[i] += *g_counter[i];

    // roglv2's counter is free-running rather than reset per render, so it is read as a
    // difference. It is also the one number here that is NOT reset by GPL, which is
    // exactly why it can be compared against gfxPolysDrawn to see what was rejected.
    if (g_polysDrawn) {
        int now = g_polysDrawn();
        g_sumRaster += static_cast<unsigned>(now - g_lastPolysDrawn);
        g_lastPolysDrawn = now;
    }
}

// Drain every window accumulator, whether or not a line was printed for it.
//
// THE RESETS USED TO LIVE AT THE END OF THE FORMAT FUNCTIONS, WHICH MEANT THEY ONLY RAN ON
// THE PATHS THAT PRINTED. Any window that bailed early -- no frames, the rasterizer not
// resolved yet, a zero cycle delta -- carried its counts into the next one, so a single
// skipped window silently doubled the next window's per-frame figures. A menu-to-race
// transition produces exactly that, and "calls per frame" is precisely the kind of derived
// number that looks plausible while being twice what it should be. Reset unconditionally,
// from one place, at the moment the window closes.
void renderProfileEndWindow() {
    for (int i = 0; i < kCounters; ++i) g_sum[i] = 0;
    g_sumRaster = 0;
    g_renders = 0;

    g_drawCycles = 0;
    g_finishCycles = 0;
    g_finishCyclesInPresent = 0;
    g_swapCycles = 0;
    g_drawCalls = 0;
    g_drawCallsInPresent = 0;
    g_finishCalls = 0;
    g_swapCalls = 0;
    g_drawStallCycles = 0;
    g_drawStallCyclesInPresent = 0;
    g_drawCyclesInPresent = 0;
    g_drawStalls = 0;
    g_drawStallsInPresent = 0;
    g_ratioTaken = false;

    g_sumPre = 0.0;
    g_sumPost = 0.0;
    g_sumScene = 0.0;
    g_sumPresent = 0.0;
    g_sumFrame = 0.0;
    g_spanFrames = 0;
    g_worst = Hitch();

    for (int i = 0; i < kPreSpans; ++i) g_preSum[i] = 0.0;
    g_preTotal = 0.0;
    g_preFrames = 0;
    g_preOrdered = 0;
}

bool renderProfileFormatGeometry(char* out, size_t size, unsigned long long frames) {
    if (!profilingWorkWait() || !g_renders || !frames) return false;
    if (!g_haveCounters && !g_polysDrawn) return false;

    double perFrame = 1.0 / frames;
    double polys = g_sum[kPolys] * perFrame;
    double pre = g_sum[kPrePoints] * perFrame;
    double post = g_sum[kPostPoints] * perFrame;
    double raster = g_sumRaster * perFrame;

    // Emitted minus accepted. roglv2 rejects a polygon below three vertices or while its
    // draw gate is clear, so a non-zero figure here is work GPL did and threw away.
    char dropped[64] = {};
    if (g_polysDrawn && g_haveCounters)
        _snprintf_s(dropped, sizeof(dropped), _TRUNCATE, " (raster %.0f, dropped %.0f)",
                    raster, polys - raster);
    else if (g_polysDrawn)
        _snprintf_s(dropped, sizeof(dropped), _TRUNCATE, " (raster %.0f)", raster);

    if (!g_haveCounters) {
        _snprintf_s(out, size, _TRUNCATE, "geom%s per frame, over %.2f renders/frame",
                    dropped, static_cast<double>(g_renders) * perFrame);
    } else {
        _snprintf_s(out, size, _TRUNCATE,
                    "geom %.0f polys%s | verts %.0f ref -> %.0f transformed (%.2fx reuse) "
                    "| %.0f matrix pushes | clipVerts %.0f | %.2f renders/frame",
                    polys, dropped, pre, post, post ? pre / post : 0.0,
                    g_sum[kPushes] * perFrame, g_sum[kClipVerts] * perFrame,
                    static_cast<double>(g_renders) * perFrame);
    }

    return true;
}

void** renderProfileSnapshotBeginSlot() {
    return g_preInstalled && g_preOriginal0 ? &g_preOriginal0 : nullptr;
}

void** renderProfileSnapshotEndSlot() { return g_tailOriginal1 ? &g_tailOriginal1 : nullptr; }

bool renderProfileFormatPre(char* out, size_t size) {
    if (!g_preInstalled || !g_preFrames) return false;

    if (!g_preOrdered) {
        _snprintf_s(out, size, _TRUNCATE,
                    "pre: not one of this window's %u frames had its markers in order, so "
                    "nothing is reported. The usual cause is frameRate = 2 -- an extra "
                    "frame is rendered AFTER the present, so the frame's first sceneRender "
                    "no longer lies inside the frame's own loop iteration. Lower "
                    "frameRate to read this line.",
                    g_preFrames);
        return true;
    }

    double perFrame = 1.0 / g_preOrdered;
    double tail = g_preSum[kPreTail] * perFrame;
    double waitTick = g_preSum[kPreWaitTick] * perFrame;
    double waitBg = g_preSum[kPreWaitBg] * perFrame;
    double latch = g_preSum[kPreLatch] * perFrame;
    double logic = g_preSum[kPreLogic] * perFrame;
    double redraw = g_preSum[kPreRedraw] * perFrame;
    double eyes = g_preSum[kPreEyes] * perFrame;
    double pre = g_preTotal * perFrame;
    double idle = waitTick + waitBg;

    // The spans tile `pre` the way the four top spans tile the frame, so `rest` is clock
    // error and nothing else. If it is not near zero the decomposition has stopped being
    // true and none of the parts should be read.
    double rest = pre - (tail + waitTick + waitBg + latch + logic + redraw + eyes);

    // Every span carries its verdict, so the line can be read without knowing which of the
    // seven names is a wait. WORK is the render thread doing something; WAIT is the render
    // thread blocked on the simulation thread.
    _snprintf_s(out, size, _TRUNCATE,
                "pre %.2f = tail %.2f WORK + tick %.2f WAIT + bg %.2f WAIT + latch %.2f "
                "WORK + logic %.2f WORK + redraw %.2f WORK + eyes %.2f WORK (rest %.2f) | "
                "WORK %.2f + WAIT %.2f (%.0f%% of pre) -- the wait is headroom, not cost | "
                "%u of %u frames ordered",
                pre, tail, waitTick, waitBg, latch, logic, redraw, eyes, rest,
                tail + latch + logic + redraw + eyes, idle,
                pre > 0.0 ? 100.0 * idle / pre : 0.0, g_preOrdered, g_preFrames);
    return true;
}

// ---------------------------------------------------------------------------------------
// The whole frame as WORK or WAIT
// ---------------------------------------------------------------------------------------

namespace {

// One frame of the window, already split. Both lines below read the same numbers, so they
// are computed once here rather than twice in two places that could drift apart.
struct WorkWait {
    double frame = 0.0;
    double preWork = 0.0;   // tail, latch, logic, redraw and any earlier eye renders
    double waitTick = 0.0;
    double waitBg = 0.0;
    double sceneWork = 0.0;
    double sceneStall = 0.0;
    double post = 0.0;
    double presentWork = 0.0;
    double presentStall = 0.0;
    double swap = 0.0;
    double renders = 1.0;  // sceneRender calls per frame: 1 flat, 2 in stereo, more with panes
};

bool takeWorkWait(WorkWait& w, double seconds, unsigned long long frames) {
    if (!profilingWorkWait() || !g_preInstalled || !g_preOrdered || !g_spanFrames || !frames)
        return false;

    double perPre = 1.0 / g_preOrdered;
    w.waitTick = g_preSum[kPreWaitTick] * perPre;
    w.waitBg = g_preSum[kPreWaitBg] * perPre;
    w.preWork = (g_preSum[kPreTail] + g_preSum[kPreLatch] + g_preSum[kPreLogic] +
                 g_preSum[kPreRedraw] + g_preSum[kPreEyes]) *
                perPre;

    double perFrame = 1.0 / g_spanFrames;
    double scene = g_sumScene * perFrame;
    double present = g_sumPresent * perFrame;
    w.post = g_sumPost * perFrame;
    w.frame = g_sumFrame * perFrame;

    double msPerCycle = windowMsPerCycle(seconds);
    double perWindowFrame = msPerCycle / frames;
    w.swap = g_swapCycles * perWindowFrame;
    w.presentStall = g_drawStallCyclesInPresent * perWindowFrame;
    w.sceneStall = (g_drawStallCycles - g_drawStallCyclesInPresent) * perWindowFrame;

    // A term measured on the TSC over the window's frames against one measured on the QPC
    // over the tiling's frames: a menu window or a window that lost frames to the ordering
    // check can make the first exceed the second, and a negative span would be read as a
    // measurement rather than as the mismatch it is.
    if (w.swap > present) w.swap = present;
    if (w.presentStall > present - w.swap) w.presentStall = present - w.swap;
    if (w.sceneStall > scene) w.sceneStall = scene;

    w.sceneWork = scene - w.sceneStall;
    w.presentWork = present - w.swap - w.presentStall;

    w.renders = g_renders ? static_cast<double>(g_renders) / frames : 1.0;
    if (w.renders < 1.0) w.renders = 1.0;
    return true;
}

}  // namespace

// The question the VR budget turns on: of the 6.9 ms a flat frame costs on a heavy map, how
// much is the render thread doing something and how much is it blocked? Time spent blocked
// is budget a ring-buffer hand-off could give back (report 72 job 2); time spent working is
// not, and a 90 Hz headset frame would have to fit inside what is left.
//
// The four top spans tile the frame, `pre` is tiled by its own seven, and each leaf is WORK
// or WAIT by where it blocks:
//
//   WAIT  pre.tick          Sleep(1) until getBGCount advances -- the simulation thread
//   WAIT  pre.bg            Sleep(1) spinning for the BG lock -- the simulation thread
//   WAIT  present.swap      inside SwapBuffers -- the frame cap, vsync or a full driver
//                           queue, i.e. the display or the GPU
//   WAIT  scene.driver      glDrawArrays calls at least profileStallUs long, during the
//   WAIT  present.driver    scene and inside screenDisplay -- the driver's queue
//   WORK  everything else
//
// The two driver terms are LOWER BOUNDS, and they are the one attribution here that is a
// rule rather than a span boundary: glDrawArrays is the only GL entry point timed, so a
// stall inside any other call is charged to WORK, and a submission genuinely slower than
// the threshold is charged to WAIT. The line prints the threshold and the call counts so
// the rule can be judged, and profileStallUs = 0 turns it off.
bool renderProfileFormatWorkWait(char* out, size_t size, double seconds,
                                 unsigned long long frames) {
    WorkWait w;
    if (!takeWorkWait(w, seconds, frames)) return false;

    double work = w.preWork + w.sceneWork + w.post + w.presentWork;
    double wait = w.waitTick + w.waitBg + w.swap + w.sceneStall + w.presentStall;

    char stalls[160] = {};
    if (g_options.profileStallUs > 0)
        _snprintf_s(stalls, sizeof(stalls), _TRUNCATE,
                    " | a driver stall is a glDrawArrays call of %d us or more (%.1f/frame "
                    "in the scene, %.1f in the flush, of %.0f calls) -- no other GL call is "
                    "timed, so both are LOWER bounds",
                    g_options.profileStallUs,
                    static_cast<double>(g_drawStalls - g_drawStallsInPresent) / frames,
                    static_cast<double>(g_drawStallsInPresent) / frames,
                    static_cast<double>(g_drawCalls) / frames);
    else
        _snprintf_s(stalls, sizeof(stalls), _TRUNCATE,
                    " | profileStallUs = 0: every glDrawArrays counts as submission WORK");

    _snprintf_s(out, size, _TRUNCATE,
                "work/wait %.2f ms/frame = WORK %.2f + WAIT %.2f (rest %.2f) | WAIT = "
                "pre.tick %.2f + pre.bg %.2f + present.swap %.2f + scene.driver %.2f + "
                "present.driver %.2f | WORK = pre %.2f + scene %.2f + post %.2f + "
                "present %.2f%s",
                w.frame, work, wait, w.frame - work - wait, w.waitTick, w.waitBg, w.swap,
                w.sceneStall, w.presentStall, w.preWork, w.sceneWork, w.post, w.presentWork,
                stalls);
    return true;
}

// What the measurement is FOR: does a headset frame fit in what the work leaves?
//
// The arithmetic is deliberately crude and stated in full on the line, because the honest
// answer is a bound rather than a prediction. Per-eye cost is the scene and the submission
// work of one render; everything else in the frame -- the latch, the game's logic, the
// window redraw, the HUD -- happens once whatever the eye count is. A second eye therefore
// adds one eye's worth of scene and submission and nothing else, and the GPU doubles.
bool renderProfileFormatHeadroom(char* out, size_t size, double seconds,
                                 unsigned long long frames) {
    WorkWait w;
    if (!takeWorkWait(w, seconds, frames)) return false;

    double budget = 1000.0 / g_options.profileTargetHz;

    // A stereo frame already contains both eyes and both mirror passes, so projecting a
    // second eye from one render's cost would count the eyes twice. There the measured
    // frame IS the headset frame and the only question is whether it fits the budget.
    if (g_options.vr && w.renders >= 2.0) {
        double work = w.preWork + w.sceneWork + w.post + w.presentWork;

        char gpu[200] = {};
        GpuTimerWindow window;
        if (gpuTimerLastWindow(window) && window.frameMs > 0.0) {
            _snprintf_s(gpu, sizeof(gpu), _TRUNCATE,
                        " | GPU busy %.2f ms/frame, %.0f%% of the budget (the last window "
                        "the GPU timer's owner drained)",
                        window.frameMs, 100.0 * window.frameMs / budget);
        } else {
            _snprintf_s(gpu, sizeof(gpu), _TRUNCATE,
                        " | no GPU total in this window (the frame budget's or the post "
                        "split's GPU line is what fills it)");
        }

        _snprintf_s(out, size, _TRUNCATE,
                    "work/wait headroom: a %d Hz frame is %.2f ms. This frame already draws "
                    "both eyes (%.2f renders a frame, mirrors included): CPU works %.2f ms "
                    "= pre %.2f + scene %.2f + post %.2f + submission %.2f, %.0f%% of the "
                    "budget, leaving %.2f ms%s",
                    g_options.profileTargetHz, budget, w.renders, work, w.preWork,
                    w.sceneWork, w.post, w.presentWork, 100.0 * work / budget, budget - work,
                    gpu);
        return true;
    }

    // Flat: one render is one eye, so a second eye repeats the scene and its submission and
    // nothing else. This is the projection that answers "would two eyes fit".
    double perEye = (w.sceneWork + w.presentWork) / w.renders;
    double once = w.preWork + w.post;
    double twoEyes = once + 2.0 * perEye;

    char gpu[200] = {};
    GpuTimerWindow window;
    if (gpuTimerLastWindow(window) && window.frameMs > 0.0) {
        double gpuPerEye = window.frameMs / w.renders;
        _snprintf_s(gpu, sizeof(gpu), _TRUNCATE,
                    " | GPU busy %.2f ms/frame (the last window the GPU timer's owner "
                    "drained), %.2f of it per render -> two eyes %.2f ms (%.0f%% of the "
                    "budget) at this resolution",
                    window.frameMs, gpuPerEye, 2.0 * gpuPerEye,
                    100.0 * 2.0 * gpuPerEye / budget);
    } else {
        _snprintf_s(gpu, sizeof(gpu), _TRUNCATE,
                    " | no GPU total in this window (the frame budget's or the post "
                    "split's GPU line is what fills it)");
    }

    _snprintf_s(out, size, _TRUNCATE,
                "work/wait headroom: a %d Hz frame is %.2f ms. Per render the CPU works "
                "%.2f ms (scene %.2f + submission %.2f over %.2f renders/frame); the "
                "once-a-frame work is %.2f ms -> two eyes cost %.2f ms, %.0f%% of the "
                "budget, leaving %.2f ms%s",
                g_options.profileTargetHz, budget, perEye, w.sceneWork / w.renders,
                w.presentWork / w.renders, w.renders, once, twoEyes,
                100.0 * twoEyes / budget, budget - twoEyes, gpu);
    return true;
}

bool renderProfileFormatPresent(char* out, size_t size, double seconds,
                                unsigned long long frames) {
    if (!profilingWorkWait() || !frames || !g_realDrawArrays) return false;

    // Cycles to milliseconds, from this window's own ratio rather than an assumed clock,
    // and taken once per report however many lines need it.
    double msPerCycle = windowMsPerCycle(seconds);
    if (msPerCycle <= 0.0) return false;

    double draw = g_drawCycles * msPerCycle / frames;
    double finish = g_finishCycles * msPerCycle / frames;
    double inPresent = g_finishCyclesInPresent * msPerCycle / frames;
    double swap = g_swapCycles * msPerCycle / frames;
    double calls = static_cast<double>(g_drawCalls) / frames;
    double late = static_cast<double>(g_drawCallsInPresent) / frames;
    double finishes = static_cast<double>(g_finishCalls) / frames;

    // Once, when enough of a race has run to have seen them all. Module-relative, because
    // roglv2's load address moves and the IDB's does not.
    if (!g_finishSitesLogged && g_finishSites) {
        g_finishSitesLogged = true;
        HMODULE roglv2 = GetModuleHandleA("roglv2.dll");
        auto    base = reinterpret_cast<const uint8_t*>(roglv2);
        char    sites[256] = {};
        int     at = 0;
        for (int i = 0; i < g_finishSites && at >= 0 && at < 200; ++i)
            at += _snprintf_s(sites + at, sizeof(sites) - at, _TRUNCATE, "%s%08X",
                              at ? ", " : "",
                              base ? 0x20000000u + static_cast<unsigned>(
                                                       g_finishSite[i] - base)
                                   : static_cast<unsigned>(reinterpret_cast<uintptr_t>(
                                         g_finishSite[i])));
        logf("raster: glFinish is called from %d site(s), returning to %s "
             "(roglv2 image addresses, so they read against the IDB directly)",
             g_finishSites, sites);
    }

    // The draw total, and the part of it that blocked. Both halves are per frame, and the
    // swap is called out as a WAIT because SwapBuffers is where the frame cap, vsync and a
    // full driver queue all land.
    double stall = g_drawStallCycles * msPerCycle / frames;
    double inPresentDraw = g_drawCyclesInPresent * msPerCycle / frames;

    _snprintf_s(out, size, _TRUNCATE,
                "present = draw %.2f ms (%.0f calls, %.0f ns each; %.2f of it in calls over "
                "%d us = driver stalls, WAIT) + swap %.2f WAIT (%.1f calls/frame) | %.0f "
                "draw calls/frame and %.2f ms of that draw time reached GL INSIDE "
                "screenDisplay, after the menu blit -- the rest is the scene's | glFinish "
                "DROPPED, %.1f calls/frame would have been made (%llu in the window over "
                "%llu frames)",
                draw, calls, calls ? 1e6 * draw / calls : 0.0, stall,
                g_options.profileStallUs, swap,
                static_cast<double>(g_swapCalls) / frames, late, inPresentDraw, finishes,
                g_finishCalls, frames);
    (void)finish;
    (void)inPresent;
    return true;
}

}  // namespace gplvr
