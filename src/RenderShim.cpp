#include "RenderShim.h"

#include "Discovery.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace gplvr {

namespace {

// Which setter ended a run. Ordered so the log reads texture-first, which is the one that
// decides whether batching is possible at all.
enum Setter { kMip, kDest, kShadow, kColour, kSetterCount };

const char* const kSetterName[kSetterCount] = {"tex", "dest", "shadow", "colour"};
const char* const kGlobalName[kSetterCount] = {"mipSetCurrent", "polySetDest",
                                               "polyStartShadow", "mipSetColor"};

// Runs are bucketed by length rather than kept individually: a frame has thousands of
// them and the shape is what matters, not any one value.
const unsigned kBucketMax[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 0xFFFFFFFF};
const int kBuckets = sizeof(kBucketMax) / sizeof(kBucketMax[0]);

// A run of 16 is the point where one batched draw starts to be clearly worth more than
// sixteen separate ones; below that the per-draw saving is competing with the cost of
// accumulating. Reported as a share of POLYGONS, not of runs, because a thousand
// single-polygon runs matter less than one run of a thousand.
const unsigned kWorthBatching = 16;

// One named pointer per thunk rather than an array: the thunks jump through these from
// inline asm, where a plain symbol is unambiguous.
void* g_originalDraw = nullptr;
void* g_originalMip = nullptr;
void* g_originalDest = nullptr;
void* g_originalShadow = nullptr;
void* g_originalColour = nullptr;

// The last value each setter was given.
//
// Counting CALLS to a setter is not the same as counting state CHANGES, and the first
// run of this instrument conflated them: it reported a mean run of 1.1 polygons with
// essentially every run ended by the texture setter, which reads as "GPL changes texture
// per polygon" but is equally consistent with "GPL re-sets the same texture per polygon".
// Those two have opposite conclusions, so the argument is read and compared. It sits at
// [esp+4] for both __cdecl and __stdcall, so this needs no more of the signature than
// that the first argument is the thing being set.
void* g_lastMip = nullptr;
void* g_lastDest = nullptr;
unsigned long long g_redundantMip = 0;
unsigned long long g_redundantDest = 0;

// Distinct textures per frame.
//
// The run-length measurement asked the wrong question. Runs are short because GPL submits
// in painter's order, but roglv2's flush ALREADY introsorts the textured-opaque bin by
// texture id -- it has decided that reordering within a bin is safe, because the
// synthesised Z preserves the painter's order independently of draw order. After that
// sort, same-texture polygons are adjacent, and what limits the draw count is the number
// of DISTINCT TEXTURES, not the length of a submission run.
//
// Open-addressed, fixed size, reset per frame: a hash set that never allocates on the hot
// path. Overflow is counted rather than grown, so the number can be trusted or discarded.
const int kTexSlots = 8192;
void*    g_texSeen[kTexSlots] = {};
unsigned g_texDistinct = 0;
unsigned g_texOverflow = 0;
unsigned long long g_distinctSum = 0;
unsigned g_distinctWorst = 0;
unsigned long long g_frames = 0;

unsigned g_run = 0;                       // polygons since the last state change
unsigned long long g_polys = 0;
unsigned long long g_runs = 0;
unsigned long long g_polysInLongRuns = 0;
unsigned long long g_bucket[kBuckets] = {};
unsigned long long g_endedBy[kSetterCount] = {};
unsigned g_longestRun = 0;

void closeRun(Setter by) {
    if (g_run) {
        ++g_runs;
        ++g_endedBy[by];
        if (g_run >= kWorthBatching) g_polysInLongRuns += g_run;
        if (g_run > g_longestRun) g_longestRun = g_run;
        for (int i = 0; i < kBuckets; ++i) {
            if (g_run <= kBucketMax[i]) {
                ++g_bucket[i];
                break;
            }
        }
        g_run = 0;
    }
}

}  // namespace

// The thunks forward with the stack exactly as they found it, so they need to know
// nothing about the entry points' signatures or who cleans up. They may clobber
// eax/ecx/edx, which a __cdecl callee is free to do anyway, and the forwarded call sets
// them last regardless.
extern "C" void __cdecl gplvrShimPoly() { ++g_polys; ++g_run; }

extern "C" void __cdecl gplvrShimMip(void* mip) {
    if (mip == g_lastMip) {
        ++g_redundantMip;
        return;
    }
    g_lastMip = mip;
    closeRun(kMip);

    // Multiplicative mix, not a shift. The first version dropped the low four bits, on
    // the assumption that the argument was a pointer; it is a HANDLE, and small handles
    // all mapped to the same slot, which is what produced hundreds of thousands of
    // overflows a second against a distinct count that stopped growing.
    unsigned key = static_cast<unsigned>(reinterpret_cast<uintptr_t>(mip)) * 2654435761u;
    unsigned slot = (key >> 13) % kTexSlots;
    for (int probe = 0; probe < 256; ++probe) {
        void*& cell = g_texSeen[(slot + probe) % kTexSlots];
        if (cell == mip) return;
        if (!cell) {
            cell = mip;
            ++g_texDistinct;
            return;
        }
    }
    ++g_texOverflow;
}

extern "C" void __cdecl gplvrShimDest(void* dest) {
    if (dest == g_lastDest) {
        ++g_redundantDest;
        return;
    }
    g_lastDest = dest;
    closeRun(kDest);
}

extern "C" void __cdecl gplvrShimShadow() { closeRun(kShadow); }

void shimEndFrame() {
    if (!g_options.shimMeasure) return;
    ++g_frames;
    g_distinctSum += g_texDistinct;
    if (g_texDistinct > g_distinctWorst) g_distinctWorst = g_texDistinct;
    for (int i = 0; i < kTexSlots; ++i) g_texSeen[i] = nullptr;
    g_texDistinct = 0;
    g_lastMip = nullptr;
}
extern "C" void __cdecl gplvrShimColour() { closeRun(kColour); }

namespace {

__declspec(naked) void polyThunk() {
    __asm { call gplvrShimPoly
            jmp dword ptr [g_originalDraw] }
}
// The argument is fetched before anything is pushed, so [esp+4] is still the caller's
// first argument, and the stack is put back exactly before forwarding.
__declspec(naked) void mipThunk() {
    __asm { mov eax, [esp+4]
            push eax
            call gplvrShimMip
            add esp, 4
            jmp dword ptr [g_originalMip] }
}
__declspec(naked) void destThunk() {
    __asm { mov eax, [esp+4]
            push eax
            call gplvrShimDest
            add esp, 4
            jmp dword ptr [g_originalDest] }
}
__declspec(naked) void shadowThunk() {
    __asm { call gplvrShimShadow
            jmp dword ptr [g_originalShadow] }
}
__declspec(naked) void colourThunk() {
    __asm { call gplvrShimColour
            jmp dword ptr [g_originalColour] }
}

bool takeOver(void** slot, void* thunk, void** saved, const char* name) {
    if (!slot || !*slot) {
        logf("shim: %s is not resolved -- its state changes will be invisible", name);
        return false;
    }
    *saved = *slot;
    if (!patchBytes(slot, &thunk, sizeof(thunk))) {
        logf("shim: could not write the %s global", name);
        return false;
    }
    return true;
}

}  // namespace

void shimInstall(const Binding& binding) {
    if (!g_options.shimMeasure) return;

    if (!takeOver(binding.raster.polyDraw, &polyThunk, &g_originalDraw, "polyDraw")) {
        logf("shim: without polyDraw there is nothing to measure -- not installed");
        return;
    }

    int taken = 0;
    taken += takeOver(rasterGlobal(binding.image, kGlobalName[kMip]), &mipThunk,
                      &g_originalMip, kGlobalName[kMip]);
    taken += takeOver(binding.raster.polySetDest, &destThunk, &g_originalDest,
                      kGlobalName[kDest]);
    taken += takeOver(rasterGlobal(binding.image, kGlobalName[kShadow]), &shadowThunk,
                      &g_originalShadow, kGlobalName[kShadow]);
    taken += takeOver(rasterGlobal(binding.image, kGlobalName[kColour]), &colourThunk,
                      &g_originalColour, kGlobalName[kColour]);

    logf("shim: stage 1 measuring -- polyDraw and %d of %d state setters forwarded, "
         "nothing changed", taken, kSetterCount);
}

bool shimFormat(char* out, size_t size, double seconds) {
    if (!g_options.shimMeasure || !g_polys) return false;

    // The run in progress is left open and carries into the next window; closing it here
    // would both attribute it to a setter that did not end it and cut a real run in two.
    char hist[128] = {};
    int at = 0;
    for (int i = 0; i < kBuckets && at >= 0 && at < static_cast<int>(sizeof(hist)) - 1; ++i) {
        if (!g_bucket[i]) continue;
        at += _snprintf_s(hist + at, sizeof(hist) - at, _TRUNCATE, "%s%u:%llu",
                          at ? " " : "", kBucketMax[i], g_bucket[i]);
    }

    char by[96] = {};
    int bat = 0;
    for (int i = 0; i < kSetterCount && bat >= 0 && bat < static_cast<int>(sizeof(by)) - 1; ++i) {
        if (!g_endedBy[i]) continue;
        bat += _snprintf_s(by + bat, sizeof(by) - bat, _TRUNCATE, "%s%s %llu",
                           bat ? " " : "", kSetterName[i], g_endedBy[i]);
    }

    _snprintf_s(out, size, _TRUNCATE,
                "shim %.0f polys/s in %.0f runs/s, mean run %.1f, longest %u, %.0f%% of "
                "polys in runs >=%u | DISTINCT TEX/frame %.0f (worst %u, overflow %u) | "
                "redundant sets: tex %.0f/s dest %.0f/s | runs<=N [%s] | ended by [%s]",
                g_polys / seconds, g_runs / seconds,
                g_runs ? double(g_polys) / g_runs : 0.0, g_longestRun,
                g_polys ? 100.0 * g_polysInLongRuns / g_polys : 0.0, kWorthBatching,
                g_frames ? double(g_distinctSum) / g_frames : 0.0, g_distinctWorst,
                g_texOverflow, g_redundantMip / seconds, g_redundantDest / seconds, hist,
                by);

    g_polys = 0;
    g_runs = 0;
    g_polysInLongRuns = 0;
    g_longestRun = 0;
    g_redundantMip = 0;
    g_redundantDest = 0;
    g_distinctSum = 0;
    g_distinctWorst = 0;
    g_texOverflow = 0;
    g_frames = 0;
    for (int i = 0; i < kBuckets; ++i) g_bucket[i] = 0;
    for (int i = 0; i < kSetterCount; ++i) g_endedBy[i] = 0;
    return true;
}

}  // namespace gplvr
