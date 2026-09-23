#include "PostProfile.h"

#include "GpuTimer.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"
#include "fast_text/FastText.h"

#include <windows.h>

#include <cstdio>

namespace gplvr {
namespace {

enum Phase { kPhasePre, kPhaseScene, kPhasePost, kPhasePresent };

// GPU groups, in frame order.
enum Group {
    kGroupPre = 0,         // present returned -> sceneRender
    kGroupScene,           // sceneRender, including any flush inside it
    kGroupPostGame,        // post before GPL's 2D drawing starts
    kGroupPost2D,          // from GPL's first bitmap, stamp, text, rect or line of the frame
    kGroupPresent,
    kGroups
};

// GPL's rasterizer entry points for 2D work, as gpl_rasterResolveEntryPoints 0x4E21B0 stores
// them (the addresses are the `mov <global>, eax` operands there).
struct RasterEntry {
    uintptr_t   global;
    const char* name;
};
constexpr RasterEntry kRasterEntries[kRasterKinds] = {
    {0x00866DE0, "bitmapDraw"}, {0x00869FE8, "bitmapScale"}, {0x00867628, "stampDraw"},
    {0x00866E08, "debugText"},  {0x00869748, "fillRect"},    {0x00869FF4, "drawLine"},
    {0x00869784, "aaLine"},
};
typedef int(__cdecl* RasterFn)(int, int, int, int, int, int, int, int);

struct State {
    Phase     phase = kPhasePre;
    long long last = 0;  // the previous present's end
    long long sceneBegin = 0;
    long long postBegin = 0;
    long long presentBegin = 0;

    // Sums over the window, in QPC ticks.
    long long pre = 0;
    long long scene = 0;
    long long post = 0;
    long long present = 0;
    unsigned  frames = 0;

    // GPL's 2D entry points, per kind: time and calls inside post, and inside the scene (the
    // mirror copy is drawn there).
    long long rasterPost[kRasterKinds] = {};
    unsigned  rasterPostCalls[kRasterKinds] = {};
    long long rasterScene[kRasterKinds] = {};
    unsigned  rasterSceneCalls[kRasterKinds] = {};
    bool      twoDeeOpen = false;  // the 2D GPU group is open this frame
};

State     g_state;
long long g_frequency = 0;
RasterFn  g_realRaster[kRasterKinds] = {};

long long now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int rasterCall(int kind, int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) {
    if (!g_realRaster[kind]) return 0;
    long long start = postProfileRasterEnter(kind);
    int       result = g_realRaster[kind](a0, a1, a2, a3, a4, a5, a6, a7);
    postProfileRasterLeave(kind, start);
    return result;
}

// One thunk per entry: cdecl, so passing more arguments than the callee takes is harmless and
// the caller (us) cleans up.
#define POST_RASTER_THUNK(kind)                                                              \
    int __cdecl rasterThunk##kind(int a0, int a1, int a2, int a3, int a4, int a5, int a6,     \
                                  int a7) {                                                  \
        return rasterCall(kind, a0, a1, a2, a3, a4, a5, a6, a7);                             \
    }
POST_RASTER_THUNK(0)
POST_RASTER_THUNK(1)
POST_RASTER_THUNK(2)
POST_RASTER_THUNK(3)
POST_RASTER_THUNK(4)
POST_RASTER_THUNK(5)
POST_RASTER_THUNK(6)
#undef POST_RASTER_THUNK
void* const kRasterThunks[kRasterKinds] = {
    reinterpret_cast<void*>(&rasterThunk0), reinterpret_cast<void*>(&rasterThunk1),
    reinterpret_cast<void*>(&rasterThunk2), reinterpret_cast<void*>(&rasterThunk3),
    reinterpret_cast<void*>(&rasterThunk4), reinterpret_cast<void*>(&rasterThunk5),
    reinterpret_cast<void*>(&rasterThunk6),
};

}  // namespace

const char* const kRasterNames[kRasterKinds] = {
    "bitmapDraw", "bitmapScale", "stampDraw", "drawDebugText", "fillRect", "drawLine", "aaLine",
};

long long postProfileRasterEnter(int kind) {
    State& s = g_state;
    if (kind < 0 || kind >= kRasterKinds) return 0;
    if (s.phase == kPhasePost && !s.twoDeeOpen) {
        s.twoDeeOpen = true;
        gpuTimerMark(kGroupPost2D);
    }
    if (s.phase != kPhasePost && s.phase != kPhaseScene) return 0;
    return now();
}

void postProfileRasterLeave(int kind, long long start) {
    State& s = g_state;
    if (!start || kind < 0 || kind >= kRasterKinds) return;
    long long spent = now() - start;
    if (s.phase == kPhasePost) {
        s.rasterPost[kind] += spent;
        ++s.rasterPostCalls[kind];
    } else if (s.phase == kPhaseScene) {
        s.rasterScene[kind] += spent;
        ++s.rasterSceneCalls[kind];
    }
}

void postProfilePollFrame() {
    if (!postProfileActive()) return;
    // One owner per slot. The fast text path needs the same seven globals -- stampDraw to take
    // the call, the other six as the points where anything still batched has to reach the
    // framebuffer first -- and it reports each entry's time through postProfileRasterLeave,
    // so the lines below read the same either way. It also holds them while its install is
    // still retrying, so the two cannot end up patched over each other.
    if (fastTextOwnsRasterEntries()) return;
    for (int kind = 0; kind < kRasterKinds; ++kind) {
        auto slot = reinterpret_cast<void**>(kRasterEntries[kind].global);
        if (!slot || !*slot || *slot == kRasterThunks[kind]) continue;
        // A mode change re-resolves the globals, so whatever is there now is the real entry.
        g_realRaster[kind] = reinterpret_cast<RasterFn>(*slot);
        void* thunk = kRasterThunks[kind];
        if (!patchBytes(slot, &thunk, sizeof(thunk))) continue;
        logf("post: GPL's %s entry point %p goes through gplvr, to time GPL's 2D drawing",
             kRasterEntries[kind].name, reinterpret_cast<void*>(g_realRaster[kind]));
    }
}

bool postProfileActive() { return g_options.profilePost && !g_options.profileStages; }

void postProfileSceneBegin() {
    if (!postProfileActive()) return;
    State& s = g_state;
    long long t = now();
    if (s.phase == kPhasePre && s.last) s.pre += t - s.last;
    if (s.phase != kPhaseScene) gpuTimerMark(kGroupScene);
    s.phase = kPhaseScene;
    s.sceneBegin = t;
}

void postProfileSceneEnd() {
    if (!postProfileActive()) return;
    State& s = g_state;
    if (s.phase != kPhaseScene) return;
    long long t = now();
    s.scene += t - s.sceneBegin;
    s.phase = kPhasePost;
    s.postBegin = t;
    gpuTimerMark(kGroupPostGame);
}

void postProfilePresentBegin() {
    if (!postProfileActive()) return;
    State& s = g_state;
    long long t = now();
    if (s.phase == kPhasePost) s.post += t - s.postBegin;
    s.phase = kPhasePresent;
    s.presentBegin = t;
    gpuTimerMark(kGroupPresent);
}

void postProfilePresentEnd() {
    if (!postProfileActive()) return;
    State& s = g_state;
    long long t = now();
    if (s.phase == kPhasePresent) {
        s.present += t - s.presentBegin;
        ++s.frames;
    }
    s.phase = kPhasePre;
    s.last = t;
    s.twoDeeOpen = false;
    gpuTimerEndFrame(kGroupPre);
}

void postProfileReport(double periodMs) {
    if (!postProfileActive()) return;
    State& s = g_state;
    if (!g_frequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_frequency = frequency.QuadPart;
    }
    if (s.frames) {
        double perFrame = 1000.0 / static_cast<double>(g_frequency) / s.frames;
        logf("post: per frame on the CPU -- pre %.2f + scene %.2f + post %.2f + present %.2f ms",
             s.pre * perFrame, s.scene * perFrame, s.post * perFrame, s.present * perFrame);
        char      line[640] = {};
        size_t    at = 0;
        int       span = 0;
        long long twoDee = 0;
        for (int kind = 0; kind < kRasterKinds; ++kind) {
            twoDee += s.rasterPost[kind];
            if (!s.rasterPostCalls[kind] && !s.rasterSceneCalls[kind]) continue;
            if (at + 96 >= sizeof(line)) break;
            at += static_cast<size_t>(std::snprintf(
                line + at, sizeof(line) - at, "%s%s %.1f calls %.2f ms (in the scene %.1f calls "
                                              "%.2f ms)",
                at ? ", " : "", kRasterEntries[kind].name,
                static_cast<double>(s.rasterPostCalls[kind]) / s.frames,
                s.rasterPost[kind] * perFrame,
                static_cast<double>(s.rasterSceneCalls[kind]) / s.frames,
                s.rasterScene[kind] * perFrame));
            ++span;
        }
        logf("post: GPL's 2D drawing per frame -- %.2f ms of the rest of post; %s",
             twoDee * perFrame, span ? line : "no calls (the entry points are not thunked)");
    }
    GpuTimerWindow gpu;
    if (gpuTimerTake(gpu) && gpu.frames) {
        const double* g = gpu.groupMs;
        bool gpuBound = gpu.queueMs > 0.5 * periodMs;
        logf("post: per frame on the GPU -- %.2f ms = pre %.2f + scene %.2f + post before the 2D "
             "%.2f + GPL's 2D %.2f + present %.2f; the GPU reaches the frame's end %.2f ms after "
             "the CPU issued it (worst %.2f), read %.1f frames late, %u skipped -> %s",
             gpu.frameMs, g[kGroupPre], g[kGroupScene], g[kGroupPostGame], g[kGroupPost2D],
             g[kGroupPresent], gpu.queueMs, gpu.queueWorstMs, gpu.framesLate, gpu.skipped,
             gpuBound ? "GPU-bound: the CPU is waiting on the GPU somewhere in its spans"
                      : "CPU-bound: the GPU keeps up with what the CPU issues");
    }
    State fresh;
    fresh.phase = s.phase;
    fresh.last = s.last;
    fresh.sceneBegin = s.sceneBegin;
    fresh.postBegin = s.postBegin;
    fresh.presentBegin = s.presentBegin;
    fresh.twoDeeOpen = s.twoDeeOpen;
    s = fresh;
}

}  // namespace gplvr
