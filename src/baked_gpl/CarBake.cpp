#include "baked_gpl/CarBake.h"

#include "DataRebase.h"
#include "Log.h"

#include "baked/Assets.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe; report 105 section 4.2, IDA 2026-10-24) -------------------------

// Reference-link data, placed through rebase().
constexpr uintptr_t kResourceCacheHead = 0x0086A128;  // -> the newest resource cache entry
constexpr uintptr_t kCarsetDirName = 0x005A1928;      // gpl_carsetDirName, e.g. "cars67"

// A resource cache entry (gpl_resourceCacheAcquire 0x4CA420): pushed at the head after its load
// returns, so older entries follow `next` and newer ones `prev`.
constexpr unsigned kEntryNext = 0;
constexpr unsigned kEntryType = 8;
constexpr unsigned kEntryName = 12;
constexpr unsigned kEntryNameBytes = 32;
constexpr unsigned kEntryValue = 44;
constexpr uint32_t kResourceObject = 0;  // value: the Obj3d
constexpr uint32_t kResourceMip = 1;     // value: the raster mip handle a MATERIAL binds
constexpr uint32_t kResourceSprite = 4;  // value: the sprite handle a BILLBOARD holds
constexpr int      kMaxCacheEntries = 32768;

// Obj3d (report 27 section 1): XYZS base and count, NORM base and count, PRIM payload and size.
constexpr unsigned kObjWords = 14;
constexpr unsigned kObjXyzs = 0;
constexpr unsigned kObjXyzsCount = 1;
constexpr unsigned kObjNorm = 2;
constexpr unsigned kObjNormCount = 3;
constexpr unsigned kObjPrim = 12;
constexpr unsigned kObjPrimSize = 13;
constexpr uint32_t kMaxPrimBytes = 64u << 20;
constexpr uint32_t kMaxRecords = 4u << 20;

// How far resolveFolder looks along the cache for the files loaded around a name.
constexpr int kMaxNeighbours = 4096;
constexpr int kMaxNamedMisses = 16;

// -- guarded reads -------------------------------------------------------------------------------
// The cache and the objects are GPL's and stay put while it runs, but a freed object or a torn
// list must cost a refusal, not the game. These functions hold no C++ objects, so __try is legal.

struct RawEntry {
    uint32_t type;
    uint32_t value;
    char     name[kEntryNameBytes + 1];
};

// Copies the cache list, newest first, into `out`. The entry count, or -1 when a read faulted.
int readCacheList(uintptr_t headSlot, RawEntry* out, int capacity, uint32_t* headOut) {
    __try {
        uintptr_t entry = *reinterpret_cast<const uint32_t*>(headSlot);
        *headOut = static_cast<uint32_t>(entry);
        int count = 0;
        while (entry && count < capacity) {
            RawEntry& raw = out[count++];
            raw.type = *reinterpret_cast<const uint32_t*>(entry + kEntryType);
            raw.value = *reinterpret_cast<const uint32_t*>(entry + kEntryValue);
            std::memcpy(raw.name, reinterpret_cast<const void*>(entry + kEntryName),
                        kEntryNameBytes);
            raw.name[kEntryNameBytes] = '\0';
            entry = *reinterpret_cast<const uint32_t*>(entry + kEntryNext);
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool copyGuarded(void* to, uintptr_t from, size_t bytes) {
    __try {
        std::memcpy(to, reinterpret_cast<const void*>(from), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t readWord(uintptr_t address) {
    uint32_t value = 0;
    copyGuarded(&value, address, sizeof(value));
    return value;
}

// -- state ---------------------------------------------------------------------------------------

struct CacheEntry {
    uint32_t    type = 0;
    uint32_t    value = 0;
    std::string name;  // lower case
};

// A team folder of the car set, or the car set's own folder (mirror.mip lives there).
struct Folder {
    std::string                     name;
    baked::TrackAssets              assets;
    std::unordered_set<std::string> loose;  // lower case
};

// A loaded .3do: where its PRIM lives, the Obj3d words that identify it, and its bake.
struct Known {
    uintptr_t   obj3d = 0;
    uintptr_t   prim = 0;
    uint32_t    primSize = 0;
    uint32_t    words[kObjWords] = {};
    std::string name;
    int32_t     object = -1;  // CarScene::objects
    bool        failed = false;
};

struct State {
    bool      bound = false;
    bool      refused = false;
    uintptr_t cacheHead = 0;
    uintptr_t carsetName = 0;

    std::string gameDirectory;
    std::string carset;
    std::vector<Folder> folders;

    std::vector<RawEntry>   raw;
    std::vector<CacheEntry> cache;  // newest first
    std::unordered_map<uint64_t, uint32_t> cacheByValue;  // type << 32 | value -> cache index
    uint32_t                lastHead = 0;
    bool                    scanned = false;
    std::vector<Known>      known;  // by PRIM address

    baked::CarScene scene;
    std::unordered_map<std::string, int32_t> textureByFile;  // "folder|name" -> scene.textures
    std::unordered_map<std::string, int32_t> spriteByFile;   // -> scene.sprites
    CarBakeTotals totals;
    int           namedMisses = 0;
    uint32_t      generation = 0;
};

State g_bake;

uint64_t cacheKey(uint32_t type, uint32_t value) {
    return (static_cast<uint64_t>(type) << 32) | value;
}

bool sameObject(const Known& a, const uint32_t words[kObjWords]) {
    return std::memcmp(a.words, words, sizeof(a.words)) == 0;
}

bool folderHolds(const Folder& folder, const std::string& lowerName) {
    return folder.loose.count(lowerName) != 0 || folder.assets.inArchive(lowerName);
}

// The car set's folders, as gpl_driverModelLoad 0x406500 changes into them: home\cars\<set> and
// each team folder under it, loose files first, then <folder>.dat.
void openFolders(State& s, const std::string& carset) {
    s.folders.clear();
    s.carset = carset;
    std::string root = s.gameDirectory + "\\cars\\" + carset;
    std::vector<std::string> paths = {root};
    WIN32_FIND_DATAA found = {};
    HANDLE           search = FindFirstFileA((root + "\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (found.cFileName[0] == '.') continue;
            paths.push_back(root + "\\" + found.cFileName);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    for (const std::string& path : paths) {
        Folder folder;
        folder.assets.open(path, std::string());
        folder.name = folder.assets.name();
        HANDLE files = FindFirstFileA((path + "\\*").c_str(), &found);
        if (files != INVALID_HANDLE_VALUE) {
            do {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                folder.loose.insert(baked::lowerCase(found.cFileName));
            } while (FindNextFileA(files, &found));
            FindClose(files);
        }
        s.folders.push_back(std::move(folder));
    }
    logf("baked cars: car set %s -- %zu folders (the set's own and its team folders) to read "
         "textures and sprites from",
         carset.c_str(), s.folders.size());
}

// Everything baked belongs to one car set; another set loads other objects at other addresses.
void checkCarset(State& s) {
    char name[64] = {};
    if (!copyGuarded(name, s.carsetName, sizeof(name) - 1) || !name[0]) return;
    if (s.carset == name && !s.folders.empty()) return;
    s.scene = baked::CarScene{};
    ++s.generation;
    s.known.clear();
    s.textureByFile.clear();
    s.spriteByFile.clear();
    s.scanned = false;
    openFolders(s, name);
}

// Re-reads GPL's resource cache and the Obj3d words of every loaded .3do. A known object whose
// words are unchanged keeps its bake.
void scanCache(State& s) {
    s.raw.resize(kMaxCacheEntries);
    uint32_t head = 0;
    int      count = readCacheList(s.cacheHead, s.raw.data(), kMaxCacheEntries, &head);
    ++s.totals.cacheScans;
    s.scanned = true;
    s.lastHead = head;
    if (count < 0) {
        logf("baked cars: GPL's resource cache could not be read (a fault in the list) -- no car "
             "object can be found until it changes");
        s.cache.clear();
        s.cacheByValue.clear();
        return;
    }
    if (count == kMaxCacheEntries)
        logf("baked cars: GPL's resource cache holds more than %d entries; only the newest are "
             "looked at",
             kMaxCacheEntries);
    s.cache.resize(static_cast<size_t>(count));
    s.cacheByValue.clear();
    for (int i = 0; i < count; ++i) {
        s.cache[i].type = s.raw[i].type;
        s.cache[i].value = s.raw[i].value;
        s.cache[i].name = baked::lowerCase(s.raw[i].name);
        s.cacheByValue.emplace(cacheKey(s.raw[i].type, s.raw[i].value), static_cast<uint32_t>(i));
    }

    std::unordered_map<uintptr_t, size_t> previous;
    for (size_t i = 0; i < s.known.size(); ++i) previous.emplace(s.known[i].obj3d, i);
    std::vector<Known> known;
    for (const CacheEntry& entry : s.cache) {
        if (entry.type != kResourceObject || !entry.value) continue;
        Known object;
        object.obj3d = entry.value;
        if (!copyGuarded(object.words, entry.value, sizeof(object.words))) continue;
        object.prim = object.words[kObjPrim];
        object.primSize = object.words[kObjPrimSize];
        if (!object.prim || object.primSize < 8 || object.primSize > kMaxPrimBytes) continue;
        object.name = entry.name;
        auto old = previous.find(object.obj3d);
        if (old != previous.end() && sameObject(s.known[old->second], object.words)) {
            object.object = s.known[old->second].object;
            object.failed = s.known[old->second].failed;
        }
        known.push_back(std::move(object));
    }
    std::sort(known.begin(), known.end(),
              [](const Known& a, const Known& b) { return a.prim < b.prim; });
    s.known.swap(known);
}

bool cacheChanged(const State& s) {
    return !s.scanned || readWord(s.cacheHead) != s.lastHead;
}

// The folder GPL read the cache entry `index` from. Every file a load acquires comes from the
// folder the load changed into, so where a name exists in more than one folder, the names
// loaded just before and after it -- its neighbours in the cache -- say which: each one narrows
// the candidates to the folders that also hold it, until one is left or a neighbour from
// another load (held by none of them) ends that direction.
int resolveFolder(State& s, size_t index) {
    const std::string& name = s.cache[index].name;
    std::vector<int>   candidates;
    for (size_t f = 0; f < s.folders.size(); ++f)
        if (folderHolds(s.folders[f], name)) candidates.push_back(static_cast<int>(f));
    if (candidates.empty()) return -1;
    if (candidates.size() == 1) {
        ++s.totals.folderUnique;
        return candidates[0];
    }
    for (int direction : {-1, 1}) {
        auto at = static_cast<long long>(index);
        for (int step = 0; step < kMaxNeighbours && candidates.size() > 1; ++step) {
            at += direction;
            if (at < 0 || at >= static_cast<long long>(s.cache.size())) break;
            const std::string& other = s.cache[static_cast<size_t>(at)].name;
            std::vector<int>   narrowed;
            for (int f : candidates)
                if (folderHolds(s.folders[static_cast<size_t>(f)], other)) narrowed.push_back(f);
            if (narrowed.empty()) break;
            candidates.swap(narrowed);
        }
        if (candidates.size() == 1) {
            ++s.totals.folderByLoadOrder;
            return candidates[0];
        }
    }
    ++s.totals.folderAmbiguous;
    return candidates[0];
}

void namedMiss(State& s, const char* kind, uint32_t handle, const std::string& name,
               const char* why) {
    if (s.namedMisses >= kMaxNamedMisses) return;
    ++s.namedMisses;
    logf("baked cars: %s handle %08X (%s) -- %s", kind, handle,
         name.empty() ? "no name" : name.c_str(), why);
}

// A mip or sprite handle to a CarScene index, through the cache entry's name and the folder
// GPL loaded it from.
int32_t resolveResource(State& s, uint32_t type, uint32_t handle) {
    bool sprite = type == kResourceSprite;
    std::unordered_map<uint32_t, int32_t>& byHandle =
        sprite ? s.scene.spriteByHandle : s.scene.textureByHandle;
    auto known = byHandle.find(handle);
    if (known != byHandle.end()) return known->second;

    const char* kind = sprite ? "sprite" : "texture";
    int32_t     result = -1;
    auto        entry = s.cacheByValue.find(cacheKey(type, handle));
    if (entry == s.cacheByValue.end() && cacheChanged(s)) {
        scanCache(s);
        entry = s.cacheByValue.find(cacheKey(type, handle));
    }
    if (entry == s.cacheByValue.end()) {
        namedMiss(s, kind, handle, std::string(), "not in GPL's resource cache");
    } else {
        const std::string& name = s.cache[entry->second].name;
        int folder = resolveFolder(s, entry->second);
        if (folder < 0) {
            namedMiss(s, kind, handle, name, "in no folder of the car set");
        } else {
            std::string key = s.folders[static_cast<size_t>(folder)].name + "|" + name;
            std::unordered_map<std::string, int32_t>& byFile =
                sprite ? s.spriteByFile : s.textureByFile;
            auto file = byFile.find(key);
            if (file != byFile.end()) {
                result = file->second;
            } else {
                LARGE_INTEGER frequency, start, end;
                QueryPerformanceFrequency(&frequency);
                QueryPerformanceCounter(&start);
                std::vector<uint8_t> bytes;
                std::string          where;
                std::string          error = "could not be read";
                const Folder&        from = s.folders[static_cast<size_t>(folder)];
                if (from.assets.read(name, bytes, &where))
                    result = sprite ? s.scene.addSprite(name, bytes.data(), bytes.size(), error)
                                    : s.scene.addTexture(name, bytes.data(), bytes.size(), error);
                if (result < 0) namedMiss(s, kind, handle, from.name + "\\" + name, error.c_str());
                byFile.emplace(key, result);
                QueryPerformanceCounter(&end);
                s.totals.fileMilliseconds += 1000.0 *
                                             static_cast<double>(end.QuadPart - start.QuadPart) /
                                             static_cast<double>(frequency.QuadPart);
            }
        }
    }
    if (sprite && result >= 0)
        ++s.totals.sprites;
    else if (sprite)
        ++s.totals.spritesMissing;
    else if (result >= 0)
        ++s.totals.textures;
    else
        ++s.totals.texturesMissing;
    byHandle.emplace(handle, result);
    return result;
}

int32_t spriteLookup(void* context, uint32_t handle) {
    return resolveResource(*static_cast<State*>(context), kResourceSprite, handle);
}

// Copies the object's PRIM, XYZS and NORM out of GPL's memory.
bool copyObject(const Known& known, baked::CarObjectImage& image, std::string& error) {
    uint32_t vertexCount = known.words[kObjXyzsCount];
    uint32_t normalCount = known.words[kObjNormCount];
    if (vertexCount > kMaxRecords || normalCount > kMaxRecords) {
        error = "XYZS or NORM count out of range";
        return false;
    }
    image.address = static_cast<uint32_t>(known.obj3d);
    image.name = known.name;
    image.primAddress = static_cast<uint32_t>(known.prim);
    image.vertexAddress = (known.words[kObjXyzs] + 15) & ~15u;
    image.normalAddress = (known.words[kObjNorm] + 15) & ~15u;
    image.model.prim.resize(known.primSize);
    if (!copyGuarded(image.model.prim.data(), known.prim, known.primSize)) {
        error = "the PRIM could not be read";
        return false;
    }
    std::vector<float> records;
    auto unpack = [&](uint32_t base, uint32_t count, std::vector<float>& out) {
        records.resize(static_cast<size_t>(count) * 4);
        if (count && !copyGuarded(records.data(), base, records.size() * sizeof(float)))
            return false;
        out.resize(static_cast<size_t>(count) * 3);
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(out.data() + i * 3, records.data() + i * 4 + 1, 3 * sizeof(float));
        return true;
    };
    if (!unpack(image.vertexAddress, vertexCount, image.model.xyz)) {
        error = "the XYZS records could not be read";
        return false;
    }
    if (!unpack(image.normalAddress, normalCount, image.model.normals)) {
        error = "the NORM records could not be read";
        return false;
    }
    return true;
}

void logObject(const baked::CarObject& object, uint32_t vertexCount, uint32_t normalCount,
               uint32_t resolved, uint32_t missing, double milliseconds) {
    const baked::CarObjectStats& c = object.stats;
    logf("baked cars: object %s (Obj3d %08X, PRIM %u bytes, %u vertices, %u normals) baked in "
         "%.2f ms -- %u leaves: %u polygons (1B %u, 1C %u, 1D %u, 1E %u, 1F %u, 20 %u, 21 %u, 22 "
         "%u; lit %u, shaded %u, under 3 corners %u, palette colour %u, literal alpha 0 %u, alpha "
         "below 255 %u), %u sprites (%u .srb unresolved); %u corners, %u triangles; %u "
         "MATERIALs naming %zu textures (%u resolved, %u missing)",
         object.name.empty() ? "?" : object.name.c_str(), object.address, object.primSize,
         vertexCount, normalCount, milliseconds, c.leaves, c.polygons, c.byType[0x1B],
         c.byType[0x1C], c.byType[0x1D], c.byType[0x1E], c.byType[0x1F], c.byType[0x20],
         c.byType[0x21], c.byType[0x22], c.lit, c.shaded, c.degenerate, c.palette, c.invisible,
         c.cornerAlpha, c.sprites, c.spritesMissing, c.corners, c.triangles, c.materials,
         object.textureHandles.size(), resolved, missing);
    for (const auto& refusal : c.refused)
        logf("baked cars: object %s -- %u leaves left without geometry: %s",
             object.name.empty() ? "?" : object.name.c_str(), refusal.second,
             refusal.first.c_str());
}

// Resolving a sprite or texture can re-read the cache, which rebuilds `known`; so the object is
// baked from a copy of its entry and the result is written to whatever entry holds it after.
void bakeKnown(State& s, Known source) {
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    baked::CarObjectImage image;
    baked::CarObject      object;
    std::string           error;
    bool ok = copyObject(source, image, error) &&
              baked::bakeCarObject(image, &spriteLookup, &s, s.scene, object, error);
    int32_t index = -1;
    if (!ok) {
        ++s.totals.objectsFailed;
        logf("baked cars: object %s (Obj3d %08X) NOT baked -- %s; its leaves are counted as "
             "refused",
             source.name.c_str(), static_cast<unsigned>(source.obj3d), error.c_str());
    } else {
        uint32_t resolved = 0;
        uint32_t missing = 0;
        for (uint32_t handle : object.textureHandles) {
            if (resolveResource(s, kResourceMip, handle) >= 0)
                ++resolved;
            else
                ++missing;
        }
        QueryPerformanceCounter(&end);
        double milliseconds = 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
        s.totals.bakeMilliseconds += milliseconds;
        logObject(object, image.model.vertexCount(),
                  static_cast<uint32_t>(image.model.normals.size() / 3), resolved, missing,
                  milliseconds);
        index = static_cast<int32_t>(s.scene.objects.size());
        s.scene.objects.push_back(std::move(object));
        ++s.totals.objects;
    }
    for (Known& known : s.known)
        if (known.obj3d == source.obj3d && sameObject(known, source.words)) {
            known.object = index;
            known.failed = index < 0;
        }
}

// The known object whose PRIM holds `node`, or null.
Known* findKnown(State& s, uintptr_t node) {
    auto after = std::upper_bound(s.known.begin(), s.known.end(), node,
                                  [](uintptr_t value, const Known& k) { return value < k.prim; });
    if (after == s.known.begin()) return nullptr;
    Known& known = *(after - 1);
    return node - known.prim < known.primSize ? &known : nullptr;
}

int32_t objectOf(State& s, uintptr_t node, uintptr_t& primAddress, uint32_t& primSize) {
    checkCarset(s);
    if (!s.scanned) scanCache(s);
    Known* known = findKnown(s, node);
    if (!known && cacheChanged(s)) {
        scanCache(s);
        known = findKnown(s, node);
    }
    if (!known) return -1;

    // The same address can hold another object after a reload; the Obj3d words tell.
    uint32_t words[kObjWords];
    if (!copyGuarded(words, known->obj3d, sizeof(words)) || !sameObject(*known, words)) {
        if (known->object >= 0) ++s.totals.objectsRebaked;
        scanCache(s);
        known = findKnown(s, node);
        if (!known) return -1;
    }
    if (known->object < 0 && !known->failed) {
        bakeKnown(s, *known);
        known = findKnown(s, node);
        if (!known) return -1;
    }
    if (known->object < 0) return -1;
    primAddress = known->prim;
    primSize = known->primSize;
    return known->object;
}

}  // namespace

bool carBakeBind() {
    State& s = g_bake;
    if (s.bound) return !s.refused;
    s.bound = true;
    s.cacheHead = rebase(kResourceCacheHead);
    s.carsetName = rebase(kCarsetDirName);
    if (!s.cacheHead || !s.carsetName) {
        s.refused = true;
        logf("baked cars: stage 2 NOT bound -- rebase refused the resource cache or the car set "
             "name (%s)",
             rebaseSummary());
        return false;
    }
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* slash = strrchr(exe, '\\')) *slash = '\0';
    s.gameDirectory = exe;
    return true;
}

// A bake runs inside the walk, which a fault ends without unwinding; anything thrown here is
// caught first and costs the object, not the walk's state.
int32_t carBakeObjectOf(uintptr_t node, uintptr_t& primAddress, uint32_t& primSize) {
    try {
        return objectOf(g_bake, node, primAddress, primSize);
    } catch (const std::exception& thrown) {
        logf("baked cars: the object bake threw (%s); car objects are left unbaked", thrown.what());
    } catch (...) {
        logf("baked cars: the object bake threw an unknown exception");
    }
    return -1;
}

// A walk can bind a texture before it reaches its first leaf, so the car set's folders are opened
// here too; a later change of car set is caught by the object lookup.
int32_t carBakeTexture(uint32_t handle) {
    if (!handle) return -1;
    try {
        if (g_bake.folders.empty()) checkCarset(g_bake);
        return resolveResource(g_bake, kResourceMip, handle);
    } catch (...) {
        return -1;
    }
}

const baked::CarScene& carBakeScene() { return g_bake.scene; }

const CarBakeTotals& carBakeTotals() { return g_bake.totals; }

uint32_t carBakeGeneration() { return g_bake.generation; }

}  // namespace gplvr
