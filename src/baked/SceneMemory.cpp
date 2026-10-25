#include "baked/SceneMemory.h"

#include "baked/BakedLog.h"
#include "baked/Mip.h"
#include "baked/Piff.h"
#include "baked/Scene.h"
#include "baked/Srb.h"

#include <cstring>
#include <vector>

namespace baked {
namespace {

constexpr double kMiB = 1048576.0;

bool isSprite(const std::string& file) {
    return file.size() > 4 && lowerCase(file.substr(file.size() - 4)) == ".srb";
}

bool sameHeader(const MipImage& kept, const MipImage& read) {
    if (kept.pixfmt != read.pixfmt || kept.flags != read.flags ||
        kept.expanded != read.expanded || kept.indexed != read.indexed ||
        std::memcmp(kept.average, read.average, sizeof(kept.average)) != 0 ||
        kept.levels.size() != read.levels.size())
        return false;
    for (size_t i = 0; i < kept.levels.size(); ++i)
        if (kept.levels[i].width != read.levels[i].width ||
            kept.levels[i].height != read.levels[i].height)
            return false;
    return true;
}

}  // namespace

size_t dropPixels(MipImage& image) {
    size_t bytes = image.bytes();
    for (MipLevel& level : image.levels) std::vector<uint8_t>().swap(level.data);
    return bytes;
}

bool readPixels(const Scene& scene, size_t index, MipImage& out, std::string& error) {
    out = MipImage{};
    if (index >= scene.textures.size()) {
        error = "no such texture in the scene";
        return false;
    }
    const Texture&       texture = scene.textures[index];
    std::vector<uint8_t> bytes;
    if (texture.file.empty() || !scene.assets.read(texture.file, bytes, nullptr)) {
        error = "the file is no longer there";
        return false;
    }
    // The bake parsed it the same way (Scene.cpp textureIndex, spriteIndex): no palette indices.
    bool parsed = false;
    if (isSprite(texture.file)) {
        SrbSprite sprite;
        parsed = parseSrb(bytes.data(), bytes.size(), sprite, out, error);
    } else {
        parsed = parseMip(bytes.data(), bytes.size(), out, error);
    }
    if (!parsed) return false;
    if (!sameHeader(texture.image, out) || out.bytes() != texture.pixelBytes) {
        error = "it decodes to another image than the one baked (the file changed)";
        out = MipImage{};
        return false;
    }
    return true;
}

void countPrimPolygons(const Model3do& model, uint32_t maxCorners, uint64_t& polygons,
                       uint64_t& corners) {
    polygons = 0;
    corners = 0;
    uint32_t offset = 4;
    while (offset < model.primSize()) {
        uint32_t size = primNodeSize(model, offset);
        if (!size) return;
        uint8_t type = model.type(offset);
        if (type == kNodePolyF || type == kNodePolyFC || type == kNodePolyT ||
            type == kNodePolyTC) {
            bool     familyB = type == kNodePolyT || type == kNodePolyTC;
            uint32_t count = model.word(offset, familyB ? 3 : 2);
            if (count >= 3 && count <= maxCorners) {
                ++polygons;
                corners += count;
            }
        }
        offset += size;
    }
}

void logBakeMemory(const std::string& track, const char* what, const BakeMemory& memory,
                   size_t budget) {
    logf("baked: %s -- memory %s in %s: held %.1f of %.0f MiB = models %.1f MiB (%u files), "
         "vertices %.1f, triangles %.1f, polygon records %.1f, indices %.1f MiB; texture pixels "
         "%.1f MiB in %u files decoded and dropped (not held); at %u items, %u polygons baked "
         "(the track has %u polygon nodes), item PRIM +0x%X",
         track.c_str(), what, memory.phase, memory.held() / kMiB, budget / kMiB,
         memory.models / kMiB, memory.modelFiles, memory.vertices / kMiB,
         memory.triangles / kMiB, memory.polygons / kMiB, memory.indices / kMiB,
         memory.texturePixels / kMiB, memory.textures, memory.items, memory.polygonsBaked,
         memory.trackPolygons, memory.itemKey);
}

}  // namespace baked
