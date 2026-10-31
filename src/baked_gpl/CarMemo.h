#pragma once

// GPL's vertex and normal memos as the car walk emulates them (CarWalk.cpp; report 105 section
// 4.4). gpl_vertexTransformCached 0x4CFBB0 transforms a vertex record once and keeps the result
// until a gpl_matrixPop(1) clears every memo made since its push; an XFORM 0x0D/0x13 pops with 1,
// an XFORM 0x16 (gpl_nodeDrawAtPoseKeepCache 0x4CC6A0) with 0, and gpl_normalLightCached 0x4CFA70
// keeps a lit normal the same way. So a vertex keeps the frame it was FIRST transformed in: a
// polygon in a parent frame that names a vertex first transformed inside a 0x16 child is drawn
// with the child's position (the wishbones, the forearms), and a vertex memoised in a parent keeps
// its parent position inside a child. The walk keeps the same memo per walk: the frame of first
// transform per record and an insertion-order pool undone where GPL pops with 1. Each entry also
// keeps the condition of a union walk it was memoised under (report 105 section 4.22; 0: none).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace baked {
struct CarScene;
}

namespace gplvr {

constexpr uint32_t kMemoBits = 14;
constexpr uint32_t kMemoSlots = 1u << kMemoBits;
constexpr uint32_t kMemoFill = kMemoSlots / 4 * 3;
constexpr uint32_t kHashEntry = 0x80000000u;  // a memo entry in the hash part, | its slot
constexpr uint16_t kDeadFrame = 0xFFFD;       // a direct entry a pop(1) cleared
constexpr uint16_t kMemoNoFrame = 0xFFFF;     // CarWalk's frame past its limit: never memoised

// One baked object's records in the direct part of a memo: entry base + (record - address) / 16.
struct MemoRange {
    uint32_t address = 0;  // the object's first record, aligned as GPL indexes them
    uint32_t count = 0;
    uint32_t base = 0;
};

// The records of the objects baked before a walk began are memoised directly by object and
// index: two array reads a lookup, and a walk touches only its own objects' entries. Anything
// else -- an object baked during the walk, every record when no bake is bound (bakedCars = 1) --
// goes to an open-addressed hash table. A record keeps one part for the whole walk.
struct Memo {
    std::vector<MemoRange> ranges;       // per CarScene object, in its order
    std::vector<uint32_t>  directStamp;  // == stamp: memoised this walk
    std::vector<uint16_t>  directFrame;  // Frame::id of the first transform, or kDeadFrame
    std::vector<uint16_t>  directCondition;
    std::vector<uint16_t>  directRegion;
    uint32_t               generation = 0;
    bool                   haveGeneration = false;
    uint32_t               last = 0;  // the range the last lookup found
    uint64_t tags[kMemoSlots];    // stamp << 32 | record address; another stamp is an empty slot
    uint16_t frames[kMemoSlots];  // Frame::id of the first transform
    uint16_t conditions[kMemoSlots];
    uint16_t regions[kMemoSlots];
    uint8_t  dead[kMemoSlots];    // cleared by a pop(1) since; the slot keeps its place in the probe
    uint32_t stamp = 0;
    uint32_t used = 0;
    std::vector<uint32_t> pool;  // entries in the order they were memoised
};

// A new walk: a new stamp, and the objects the bake finished since the last walk get their
// direct entries (vertex or NORM records, as `normals` says).
void memoBegin(Memo& memo, const baked::CarScene& scene, uint32_t generation, bool normals);

// The memo entry of record `key`: direct when a range holds it, else kHashEntry | its slot.
uint32_t memoEntry(Memo& memo, uint32_t key);

// Whether `entry` (of record `key`) is memoised and alive this walk, in which frame, under which
// condition and in which region (a half of an order node of a kept list's walk, report 105
// section 4.23; 0: none).
bool memoAlive(const Memo& memo, uint32_t entry, uint32_t key, uint16_t& frame,
               uint16_t& condition, uint16_t& region);

// Memoises `entry` in frame `id` under `condition` in `region`. A full hash table, or
// kMemoNoFrame, leaves it unmemoised, as if transformed here each time.
void memoStore(Memo& memo, uint32_t entry, uint32_t key, uint16_t id, uint16_t condition,
               uint16_t region, uint32_t& overflow);

// gpl_matrixPop(1): every memo made since the pool held `mark` entries is cleared.
void memoUndo(Memo& memo, size_t mark);

}  // namespace gplvr
