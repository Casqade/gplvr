#include "baked_gpl/CarMemo.h"

#include "DataRebase.h"
#include "Log.h"

#include "baked/CarScene.h"

#include <algorithm>
#include <cstring>

namespace gplvr {
namespace {

// GPL's transformed-vertex memo (gpl_vertexTransformCached 0x4CFBB0): the pool, slot 0 of which
// holds clip xyzw at +0, the frustum outcode at +28, the user-plane bits at +32 and the plane
// distances at +40; the slots in use; and the record each slot belongs to. Reference-link data.
constexpr uintptr_t kVertexPool = 0x00578730;       // gpl_transformedVertexPool, -> slot 0
constexpr uintptr_t kVertexPoolIndex = 0x005807F0;  // gpl_transformedVertexPoolIndex, slots in use
constexpr uintptr_t kVertexSources = 0x00570730;    // gpl_transformedVertexSourceList[8192]
constexpr uint32_t  kVertexSourceCount = 8192;

// Placed through rebase() by carMemoGplBind; 0 when refused.
uintptr_t g_vertexPool = 0;
uintptr_t g_vertexPoolIndex = 0;
uintptr_t g_vertexSources = 0;

template <typename T>
T at(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// The hash slot holding `key` this walk, dead or alive, or the empty slot it goes to.
uint32_t memoSlot(const Memo& memo, uint32_t key) {
    uint64_t tag = (static_cast<uint64_t>(memo.stamp) << 32) | key;
    uint32_t slot = ((key >> 2) * 2654435761u) >> (32 - kMemoBits);
    while (true) {
        uint64_t held = memo.tags[slot];
        if (held == tag || static_cast<uint32_t>(held >> 32) != memo.stamp) return slot;
        slot = (slot + 1) & (kMemoSlots - 1);
    }
}

}  // namespace

void memoBegin(Memo& memo, const baked::CarScene& scene, uint32_t generation, bool normals) {
    if (!memo.haveGeneration || memo.generation != generation) {
        memo.ranges.clear();
        memo.directStamp.clear();
        memo.directFrame.clear();
        memo.directCondition.clear();
        memo.directRegion.clear();
        memo.generation = generation;
        memo.haveGeneration = true;
        memo.last = 0;
    }
    for (size_t i = memo.ranges.size(); i < scene.objects.size(); ++i) {
        const baked::CarObject& object = scene.objects[i];
        MemoRange range;
        range.address = normals ? object.normalAddress : object.vertexAddress;
        range.count = normals ? object.normalCount : object.vertexCount;
        range.base = static_cast<uint32_t>(memo.directStamp.size());
        memo.ranges.push_back(range);
        memo.directStamp.resize(memo.directStamp.size() + range.count, 0);
        memo.directFrame.resize(memo.directFrame.size() + range.count, 0);
        memo.directCondition.resize(memo.directCondition.size() + range.count, 0);
        memo.directRegion.resize(memo.directRegion.size() + range.count, 0);
    }
    if (++memo.stamp == 0) {
        std::memset(memo.tags, 0, sizeof(memo.tags));
        std::fill(memo.directStamp.begin(), memo.directStamp.end(), 0u);
        memo.stamp = 1;
    }
    memo.used = 0;
    if (memo.pool.capacity() < kMemoFill) memo.pool.reserve(kMemoFill);
    memo.pool.clear();
}

uint32_t memoEntry(Memo& memo, uint32_t key) {
    size_t count = memo.ranges.size();
    if (count) {
        const MemoRange& last = memo.ranges[memo.last];
        uint32_t offset = key - last.address;
        if (!(offset & 15) && (offset >> 4) < last.count) return last.base + (offset >> 4);
        for (size_t i = 0; i < count; ++i) {
            const MemoRange& range = memo.ranges[i];
            offset = key - range.address;
            if (!(offset & 15) && (offset >> 4) < range.count) {
                memo.last = static_cast<uint32_t>(i);
                return range.base + (offset >> 4);
            }
        }
    }
    return kHashEntry | memoSlot(memo, key);
}

bool memoAlive(const Memo& memo, uint32_t entry, uint32_t key, uint16_t& frame,
               uint16_t& condition, uint16_t& region) {
    if (entry & kHashEntry) {
        uint32_t slot = entry & ~kHashEntry;
        if (memo.tags[slot] != ((static_cast<uint64_t>(memo.stamp) << 32) | key) ||
            memo.dead[slot])
            return false;
        frame = memo.frames[slot];
        condition = memo.conditions[slot];
        region = memo.regions[slot];
        return true;
    }
    if (memo.directStamp[entry] != memo.stamp || memo.directFrame[entry] == kDeadFrame)
        return false;
    frame = memo.directFrame[entry];
    condition = memo.directCondition[entry];
    region = memo.directRegion[entry];
    return true;
}

void memoStore(Memo& memo, uint32_t entry, uint32_t key, uint16_t id, uint16_t condition,
               uint16_t region, uint32_t& overflow) {
    if (id == kMemoNoFrame) return;
    if (entry & kHashEntry) {
        uint32_t slot = entry & ~kHashEntry;
        uint64_t tag = (static_cast<uint64_t>(memo.stamp) << 32) | key;
        bool     known = memo.tags[slot] == tag;
        if (!known && memo.used >= kMemoFill) {
            ++overflow;
            return;
        }
        if (!known) ++memo.used;
        memo.tags[slot] = tag;
        memo.dead[slot] = 0;
        memo.frames[slot] = id;
        memo.conditions[slot] = condition;
        memo.regions[slot] = region;
    } else {
        memo.directStamp[entry] = memo.stamp;
        memo.directFrame[entry] = id;
        memo.directCondition[entry] = condition;
        memo.directRegion[entry] = region;
    }
    memo.pool.push_back(entry);
}

void memoUndo(Memo& memo, size_t mark) {
    for (size_t i = mark; i < memo.pool.size(); ++i) {
        uint32_t entry = memo.pool[i];
        if (entry & kHashEntry)
            memo.dead[entry & ~kHashEntry] = 1;
        else
            memo.directFrame[entry] = kDeadFrame;
    }
    memo.pool.resize(mark);
}

void carMemoGplBind() {
    // The three operands gpl_vertexTransformCached reads them through (0x4CFBC0 `A1 index`,
    // 0x4CFBC5 `8B 3D pool`, 0x4CFBD3 `8D 14 85 sources`) must name the rebased addresses.
    uintptr_t index = rebase(kVertexPoolIndex);
    uintptr_t pool = rebase(kVertexPool);
    uintptr_t sources = rebase(kVertexSources);
    bool code = at<uint8_t>(0x004CFBC0) == 0xA1 && at<uint16_t>(0x004CFBC5) == 0x3D8B &&
                at<uint8_t>(0x004CFBD3) == 0x8D && at<uint16_t>(0x004CFBD4) == 0x8514;
    if (index && pool && sources && code && at<uint32_t>(0x004CFBC1) == index &&
        at<uint32_t>(0x004CFBC7) == pool && at<uint32_t>(0x004CFBD6) == sources) {
        g_vertexPoolIndex = index;
        g_vertexPool = pool;
        g_vertexSources = sources;
        return;
    }
    logf("baked cars: GPL's vertex memo NOT read -- gpl_vertexTransformCached does not name the "
         "rebased pool (%08X %08X %08X); what GPL memoised before a walk counts as `memo words "
         "not a live slot`",
         static_cast<unsigned>(index), static_cast<unsigned>(pool),
         static_cast<unsigned>(sources));
}

uintptr_t carMemoGplSlot(uintptr_t record, uint32_t& staleWords) {
    uint32_t slot = at<uint32_t>(record);
    if (!slot) return 0;
    if (!g_vertexPool) {
        ++staleWords;
        return 0;
    }
    uint32_t pool = at<uint32_t>(g_vertexPool);
    int32_t  used = at<int32_t>(g_vertexPoolIndex);
    uint32_t offset = slot - pool;
    uint32_t index = offset >> 6;
    if (slot < pool || (offset & 63) || used <= 0 || index >= static_cast<uint32_t>(used) ||
        index >= kVertexSourceCount || at<uint32_t>(g_vertexSources + 4u * index) != record) {
        ++staleWords;
        return 0;
    }
    return slot;
}

uintptr_t carMemoGplPoolIndex() { return g_vertexPoolIndex; }
uintptr_t carMemoGplSources() { return g_vertexSources; }

}  // namespace gplvr
