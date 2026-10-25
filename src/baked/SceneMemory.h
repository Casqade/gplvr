#pragma once

// What a track scene keeps in memory, and what its bake commits on the way
// (docs/audit-2026-09-18/105-baked-renderer-rewrite.md section 4.15).
//
// A scene holds no texture pixels. The bake decodes each texture once, keeps what it decides from
// it -- the format (the pass), the MPHD average colour (the flat colour), the wrap flags and each
// level's size -- and drops the pixels; the renderer reads the file again at upload, one texture
// at a time, and frees it after. At Isle of Man the pixels were 632 MiB against a 562 MiB budget.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace baked {

struct MipImage;
struct Model3do;
struct Scene;

// std::vector grows geometrically, so a big track's arrays ask the allocator for a block twice
// the size of the one they already hold, and both are live during the move. In a 32-bit host that
// fails long before the memory runs out: at Isle of Man the bake died with 1.2 GB on the heap and
// no free region above 45.9 MB (report 60 section 10). Growing by a fixed step instead keeps the
// transient cost of a move at one step, and the pre-flight reserve of Baker::run means the
// track's own geometry never moves.
template <class T>
void growBy(std::vector<T>& target, size_t extra, size_t floorStep) {
    size_t want = target.size() + extra;
    if (want <= target.capacity()) return;
    target.reserve(want + std::max(floorStep, target.capacity() / 4));
}

// Drops the image's pixels and keeps the rest of it, each level's size included. Returns the
// bytes the pixels held.
size_t dropPixels(MipImage& image);

// Reads texture `index` of `scene` again from the file it was baked from (Texture::file) and
// checks it decodes to the image the bake kept: format, flags, average colour, every level's size
// and the pixel bytes. False with a reason when the file is gone or differs.
bool readPixels(const Scene& scene, size_t index, MipImage& out, std::string& error);

// The loader's own linear sweep of the PRIM payload (Piff primTiles), counting the polygon nodes
// of 3 to `maxCorners` corners and their corners. Every track polygon is emitted at most once, so
// this is the exact size of the track's own geometry; object instances add their models' polygons
// on top, which is why the arrays still grow by a step afterwards.
void countPrimPolygons(const Model3do& model, uint32_t maxCorners, uint64_t& polygons,
                       uint64_t& corners);

// What the bake holds against BakeOptions::memoryBudgetBytes, by part, and where it was.
struct BakeMemory {
    size_t   models = 0;  // the .3do payloads
    uint32_t modelFiles = 0;
    // Capacities in bytes: a reserve is memory the process is holding.
    size_t vertices = 0;
    size_t triangles = 0;
    size_t polygons = 0;  // the per-polygon records
    size_t indices = 0;
    // Decoded and dropped: counted for the log, not held.
    size_t   texturePixels = 0;
    uint32_t textures = 0;

    const char* phase = "";
    uint32_t    items = 0;          // items kept so far
    uint32_t    polygonsBaked = 0;  // polygons in them
    uint32_t    trackPolygons = 0;  // the track's own polygon nodes (countPrimPolygons)
    uint32_t    itemKey = 0;        // the PRIM offset of the item being baked

    size_t held() const { return models + vertices + triangles + polygons + indices; }
};

// One log line for `track`: `what` ("OVER THE BUDGET", "peak"), the parts, the budget and where
// the bake was.
void logBakeMemory(const std::string& track, const char* what, const BakeMemory& memory,
                   size_t budget);

}  // namespace baked
