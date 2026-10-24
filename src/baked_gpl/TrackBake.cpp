#include "baked_gpl/TrackBake.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"

#include "baked/Assets.h"
#include "baked/BakedLog.h"
#include "baked/Scene.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace gplvr {
namespace {

// The season patch's extension table, "367\0s67\0m67\0" when it is installed (report 28 1.1).
// Past the end of `.data`, so rebase() gives it back unchanged when the image holds it.
constexpr uintptr_t kSeasonExtensions = 0x00A030A4;

// Obj3d (report 26 section 2): the XYZS array and its count, the PRIM payload and its size.
constexpr unsigned kObjXyzs = 0x00;
constexpr unsigned kObjXyzsCount = 0x04;
constexpr unsigned kObjPrim = 0x30;
constexpr unsigned kObjPrimSize = 0x34;

constexpr int kVertexSamples = 64;

// The surface classes GPL uses for decals painted onto the ground: 20 the racing groove ("Racing
// groove in forward view" in the graphics menu), 10 the start grid and the start/finish line
// (report 61, report 59 section 6). Both track compilers mark these two and nothing else.
constexpr uint32_t kDecalSurfaceClasses = (1u << 20) | (1u << 10);

// How much of the process's own address space the bake may commit. What failed at Isle of Man was
// contiguity, not total free bytes, so half of what is free when the bake starts is the honest
// share (report 60 section 10). gplc67.exe is /LARGEADDRESSAWARE since 2026-09-18.
size_t bakeMemoryBudget() {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 512u << 20;
    uint64_t half = status.ullAvailVirtual / 2;
    uint64_t ceiling = static_cast<uint64_t>(SIZE_MAX);
    if (half > ceiling) half = ceiling;
    size_t budget = static_cast<size_t>(half);
    if (budget < (256u << 20)) budget = 256u << 20;
    return budget;
}

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    DWORD readableFlags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readableFlags)) return false;
    auto begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    auto want = reinterpret_cast<uintptr_t>(address);
    return want >= begin && want + bytes <= begin + info.RegionSize;
}

template <typename T>
T readValue(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

void logSink(const char* line) { logf("%s", line); }

// -- the bake job ---------------------------------------------------------------------------------

struct Job {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};

    std::string        gameDirectory;
    std::string        season;
    uint32_t           primSize = 0;
    uint32_t           vertexCount = 0;
    std::vector<float> samples;  // the first and last kVertexSamples vertices GPL holds

    std::unique_ptr<baked::Scene> scene;
    std::string                   result;
    double                        milliseconds = 0.0;
    size_t                        freedBytes = 0;  // the previous scene's, added to the budget
};

bool samplesMatch(const std::vector<float>& vertices, const std::vector<float>& samples,
                  uint32_t vertexCount) {
    int count = static_cast<int>(samples.size() / 6);
    if (vertices.size() != static_cast<size_t>(vertexCount) * 3) return false;
    for (int i = 0; i < count; ++i) {
        if (std::memcmp(vertices.data() + i * 3, samples.data() + i * 3, 12) != 0) return false;
        size_t tail = (vertexCount - count + i) * 3;
        if (std::memcmp(vertices.data() + tail, samples.data() + (count + i) * 3, 12) != 0)
            return false;
    }
    return true;
}

void bakeOneTrack(const std::shared_ptr<Job>& job) {
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    std::string              tracks = job->gameDirectory + "\\tracks";
    WIN32_FIND_DATAA         found = {};
    HANDLE                   search = FindFirstFileA((tracks + "\\*").c_str(), &found);
    std::vector<std::string> candidates;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (found.cFileName[0] == '.') continue;
            baked::TrackAssets assets;
            if (!assets.open(tracks + "\\" + found.cFileName, job->season)) continue;
            baked::Model3doSizes sizes;
            if (!assets.probe3do(std::string(found.cFileName) + ".3do", sizes, nullptr)) continue;
            if (sizes.primSize == job->primSize && sizes.vertexCount == job->vertexCount)
                candidates.push_back(found.cFileName);
        } while (!job->cancel.load() && FindNextFileA(search, &found));
        FindClose(search);
    }

    if (candidates.empty())
        job->result = "no tracks\\<dir>\\<dir>.3do has the PRIM size and vertex count GPL loaded";

    for (const std::string& name : candidates) {
        if (job->cancel.load()) break;
        baked::TrackAssets assets;
        assets.open(tracks + "\\" + name, job->season);
        auto        scene = std::make_unique<baked::Scene>();
        std::string error;
        logf("baked: loading %s from %s", name.c_str(), assets.directory().c_str());
        baked::BakeOptions options;
        options.decalClasses = kDecalSurfaceClasses;
        options.memoryBudgetBytes = bakeMemoryBudget() + job->freedBytes;
        if (!baked::bakeTrack(assets, name + ".3do", options, *scene, error, &job->cancel)) {
            job->result = name + ": " + error;
            continue;
        }
        if (!samplesMatch(scene->trackVertices, job->samples, job->vertexCount)) {
            job->result = name + ": sizes match but the vertices differ from GPL's";
            continue;
        }
        job->scene = std::move(scene);
        job->result = name;
        break;
    }

    QueryPerformanceCounter(&end);
    job->milliseconds = 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                        static_cast<double>(frequency.QuadPart);
}

// std::thread calls std::terminate on an exception that reaches the top of the thread function,
// which aborts the game. The bake is an addition, so anything thrown below here drops the scene
// and leaves the track to GPL (report 60 section 10). baked::bakeTrack catches the common case
// itself; this is the backstop for everything outside it.
void runJob(std::shared_ptr<Job> job) {
    try {
        bakeOneTrack(job);
    } catch (const std::bad_alloc&) {
        job->scene.reset();
        job->result = "out of memory -- too large for this process";
    } catch (const std::exception& thrown) {
        job->scene.reset();
        job->result = std::string("the bake failed: ") + thrown.what();
    } catch (...) {
        job->scene.reset();
        job->result = "the bake failed with an unknown exception";
    }
    job->done.store(true);
}

// -- state ---------------------------------------------------------------------------------------

struct State {
    bool        bound = false;
    std::string gameDirectory;
    std::string season;

    // The track the scene belongs to, as GPL holds it.
    void*    trackObject = nullptr;
    uint8_t* prim = nullptr;
    uint32_t primSize = 0;
    bool     trackFailed = false;

    std::shared_ptr<Job>          job;
    std::unique_ptr<baked::Scene> scene;
    uint32_t                      generation = 0;  // scenes landed so far
};

State g_state;

void bind(State& s) {
    if (s.bound) return;
    s.bound = true;
    baked::setLogSink(&logSink);

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* slash = strrchr(exe, '\\')) *slash = '\0';
    s.gameDirectory = exe;

    auto season = reinterpret_cast<const char*>(rebase(kSeasonExtensions));
    if (season && readable(season, 12) && season[0] == '3' && season[3] == '\0' &&
        season[4] == 's' && season[8] == 'm' && season[1] == season[5] &&
        season[2] == season[6] && season[1] == season[9] && season[2] == season[10])
        s.season.assign(season + 1, 2);

    logf("baked: game directory %s, season patch %s", s.gameDirectory.c_str(),
         s.season.empty() ? "not installed" : s.season.c_str());
}

// What a scene holds in the heap: its textures' pixels and its vertex and index arrays.
size_t sceneBytes(const baked::Scene& scene) {
    size_t bytes = scene.vertices.capacity() * sizeof(scene.vertices[0]) +
                   scene.indices.capacity() * sizeof(scene.indices[0]);
    for (const baked::Texture& texture : scene.textures) bytes += texture.image.bytes();
    return bytes;
}

// Returns the bytes the dropped scene held. Most of its blocks stay in the CRT heap's segments
// once freed, which GlobalMemoryStatusEx does not count as available, yet the next bake reuses
// them: a second race on spa67 measured 423 MiB against 611 at the first and refused itself.
size_t resetScene(State& s) {
    if (s.job) s.job->cancel.store(true);
    s.job.reset();
    size_t freed = s.scene ? sceneBytes(*s.scene) : 0;
    s.scene.reset();
    return freed;
}

void logBake(const Job& job, const baked::Scene& scene) {
    const baked::BakeStats& stats = scene.stats;
    size_t opaque = 0, cutout = 0, translucent = 0, decal = 0, upright = 0, textureBytes = 0;
    for (const baked::Batch& batch : scene.batches) {
        if (batch.decal) ++decal;
        if (batch.upright) ++upright;
        if (batch.pass == baked::kPassOpaque) ++opaque;
        if (batch.pass == baked::kPassCutout) ++cutout;
        if (batch.pass == baked::kPassTranslucent) ++translucent;
    }
    for (const baked::Texture& texture : scene.textures) textureBytes += texture.image.bytes();
    logf("baked: track %s matched and baked in %.0f ms -- items %u (track %u, object instances "
         "%u), polygons %u, corners %u, batches %zu (opaque %zu, cutout %zu, translucent %zu, of "
         "them decal %zu, upright cutout %zu), textures %zu (%.1f MiB, missing %u), objects %u",
         job.result.c_str(), job.milliseconds, stats.trackItems + stats.instanceItems,
         stats.trackItems, stats.instanceItems, stats.polygons, stats.corners,
         scene.batches.size(), opaque, cutout, translucent, decal, upright, scene.textures.size(),
         textureBytes / 1048576.0, stats.texturesMissing, stats.modelsLoaded);
    logf("baked: copies -- %u items are exact copies of an earlier one (the same geometry in the "
         "same place, %u polygons not baked again); each marks its original, drawn once at the "
         "place GPL reaches last",
         stats.copyItems, stats.copyPolygons);

    const baked::OverlayStats& overlay = stats.overlay;
    logf("baked: overlays -- accepted %u (same item %u, GROUP child order %u, swept plane %u, "
         "containment %u), rejected %u (view-dependent plane %u, never drawn together %u, "
         "cross-segment %u, cross-band %u, other ancestor %u, edge sliver %u); %u polygons over "
         "another, deepest level %u",
         overlay.sameItem + overlay.groupAncestor + overlay.sweptPlane + overlay.containment,
         overlay.sameItem, overlay.groupAncestor, overlay.sweptPlane, overlay.containment,
         overlay.viewDependent + overlay.neverTogether + overlay.crossSegment + overlay.crossBand +
             overlay.otherAncestor + overlay.smallArea,
         overlay.viewDependent, overlay.neverTogether, overlay.crossSegment, overlay.crossBand,
         overlay.otherAncestor, overlay.smallArea, overlay.overlayPolygons, overlay.maxLevel);
    logf("baked: decals -- classes %08X: %u polygons in %u ranges, %u found by coverage, %u "
         "translucent object polygons on the track (shadows), %u decal levels; road under %u "
         "ground polygons: by GROUP child order %u, by the geometric search only %u, none %u; "
         "%u decals promoted onto an overlay road; %u with a piece over 30 cm from its road",
         scene.decalClasses, overlay.decalPolygons, overlay.decalRanges, overlay.coverageDecals,
         overlay.objectDecals, overlay.decalMaxLevel + 1, overlay.cutPolygons,
         overlay.baseAncestry, overlay.baseGeometry, overlay.baseNone, overlay.promotedDecals,
         overlay.wrongRoad);
    logf("baked: decals -- cut into %u pieces, %u triangles where there were %u; gap to the road "
         "per corner (mm) min %.1f median %.2f p99 %.1f max %.1f, %u of %u under their road; "
         "skirts sunk %u over %.1f m2",
         overlay.pieces, overlay.trianglesAfter, overlay.trianglesBefore, overlay.gapMinMm,
         overlay.gapMedianMm, overlay.gapP99Mm, overlay.gapMaxMm, overlay.gapPositive,
         overlay.gapVertices, overlay.sunkSkirts, overlay.sunkArea);

    const baked::ObjectStats& objects = stats.objects;
    logf("baked: objects -- %u sprites (%u sized by the node, %u tinted, %u screen-aligned) from "
         "%u .srb files (%u missing), %u lines, %u SELECTORs over 64 cases (%u cases, %u children "
         "baked once per window)",
         objects.sprites, objects.spritesScaled, objects.spritesShaded, objects.spritesFacing,
         objects.spriteFiles, objects.spriteFilesMissing, objects.lines, objects.wideSelectors,
         objects.wideSelectorCases, objects.wideRepeats);
    logf("baked: walk order -- %u items hold geometry roglv2 paints without depth: %u triangles "
         "in %u runs over %u leaves, ordered per pass by %u splitting and SORTGROUP nodes",
         objects.orderedItems, objects.orderTriangles, objects.orderRuns, objects.orderLeaves,
         objects.orderNodes);

    if (stats.refused.empty()) {
        logf("baked: refused -- nothing; every item is baked");
        return;
    }
    for (const auto& refusal : stats.refused)
        logf("baked: refused %u items -- %s (left to GPL)", refusal.second, refusal.first.c_str());
}

}  // namespace

void trackBakeMainWalk(void* sceneParams) {
    if (g_options.bakedRenderer == 0 || !sceneParams) return;
    State& s = g_state;
    bind(s);

    void* object = nullptr;
    std::memcpy(&object, sceneParams, sizeof(object));
    if (!object || !readable(object, 0x3C)) return;
    auto at = reinterpret_cast<uintptr_t>(object);
    auto prim = readValue<uint8_t*>(at + kObjPrim);
    auto primSize = readValue<uint32_t>(at + kObjPrimSize);
    auto xyzs = readValue<uintptr_t>(at + kObjXyzs);
    auto vertexCount = readValue<uint32_t>(at + kObjXyzsCount);

    if (object != s.trackObject || prim != s.prim || primSize != s.primSize) {
        size_t freed = resetScene(s);
        s.trackObject = object;
        s.prim = prim;
        s.primSize = primSize;
        s.trackFailed = false;

        // First and last bytes of each block: a heap block can straddle VirtualQuery regions.
        uintptr_t base = (xyzs + 15) & ~static_cast<uintptr_t>(15);
        uintptr_t xyzsEnd = base + static_cast<uintptr_t>(vertexCount) * 16;
        if (!prim || primSize < 16 || !readable(prim, 16) || !readable(prim + primSize - 16, 16) ||
            !xyzs || vertexCount < kVertexSamples ||
            !readable(reinterpret_cast<const void*>(base), 16) ||
            !readable(reinterpret_cast<const void*>(xyzsEnd - 16), 16)) {
            s.trackFailed = true;
            logf("baked: the track object %p is not readable as an Obj3d -- nothing baked",
                 object);
            return;
        }

        auto job = std::make_shared<Job>();
        job->gameDirectory = s.gameDirectory;
        job->season = s.season;
        job->primSize = primSize;
        job->vertexCount = vertexCount;
        job->freedBytes = freed;
        job->samples.resize(kVertexSamples * 6);
        for (int i = 0; i < kVertexSamples; ++i) {
            std::memcpy(job->samples.data() + i * 3,
                        reinterpret_cast<const void*>(base + 16 * i + 4), 12);
            std::memcpy(job->samples.data() + (kVertexSamples + i) * 3,
                        reinterpret_cast<const void*>(
                            base + 16 * (vertexCount - kVertexSamples + i) + 4),
                        12);
        }
        logf("baked: new track object %p (PRIM %u bytes, %u vertices) -- finding and baking its "
             "file on a worker thread; the game draws the track meanwhile (the previous scene's "
             "%.0f MiB freed and added to the budget)",
             object, primSize, vertexCount, freed / 1048576.0);
        s.job = job;
        std::thread(runJob, job).detach();
        return;
    }

    if (s.trackFailed || !s.job || !s.job->done.load()) return;
    std::shared_ptr<Job> job = std::move(s.job);
    if (!job->scene) {
        s.trackFailed = true;
        logf("baked: nothing baked after %.0f ms -- %s; left to the game's own renderer",
             job->milliseconds, job->result.c_str());
        return;
    }
    s.scene = std::move(job->scene);
    ++s.generation;
    logBake(*job, *s.scene);
}

const baked::Scene* trackBakeScene() { return g_state.scene.get(); }

uint32_t trackBakeSceneGeneration() { return g_state.generation; }

bool trackBakePrim(const unsigned char*& prim, unsigned& size) {
    if (!g_state.scene) return false;
    prim = g_state.prim;
    size = g_state.primSize;
    return true;
}

}  // namespace gplvr
