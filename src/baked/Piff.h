#pragma once

// The .3do container (docs/audit-2026-09-09/26-3do-container-and-geometry.md) and the PRIM
// node sizes (27-prim-node-graph.md). Parsed from file bytes; nothing is relocated, so every
// node reference stays a byte offset into `prim` and every table reference an index.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace baked {

// PRIM node types that the bake names. Report 27 section 4.2.
enum NodeType : uint8_t {
    kNodeEmpty = 0x01,
    kNodeBillboard = 0x03,
    kNodeGroup = 0x04,
    kNodeMaterial = 0x05,
    kNodeSideGate = 0x06,
    kNodeSidePick = 0x07,
    kNodeBsp4 = 0x08,
    kNodeBsp3Asym = 0x09,
    kNodeBsp3 = 0x0A,
    kNodeBsp2 = 0x0B,
    kNodeAltSet = 0x0C,
    kNodeXform = 0x0D,
    kNodeSubObject = 0x0E,
    kNodeSortGroup = 0x0F,
    kNodeIdxList = 0x10,
    kNodeSelector = 0x11,
    kNodeXformB = 0x13,
    kNodeSetReg = 0x14,
    kNodeLine = 0x15,
    kNodeXformKeep = 0x16,
    kNodePretransform = 0x17,
    kNodeSphereCull = 0x18,
    kNodeBoxCull = 0x19,
    kNodePolyF = 0x1B,
    kNodePolyFC = 0x1C,
    kNodePolyFN = 0x1D,
    kNodePolyFCN = 0x1E,
    kNodePolyT = 0x1F,
    kNodePolyTC = 0x20,
    kNodePolyTN = 0x21,
    kNodePolyTCN = 0x22,
};

constexpr uint32_t kNoChild = 0xFFFFFFFFu;

struct Model3do {
    uint32_t              version = 0;
    std::vector<float>    xyz;      // 3 per vertex, the XYZS table without its cache word
    std::vector<float>    normals;  // 3 per normal
    std::vector<float>    planes;   // a, b, c, d per plane; a*x + b*y + c*z + d = 0
    std::vector<char>     strings;  // STRN verbatim
    std::vector<uint8_t>  prim;     // PRIM payload verbatim; word 0 is the root node offset
    std::vector<uint32_t> handles;  // HAND, name offset and node offset per entry

    uint32_t vertexCount() const { return static_cast<uint32_t>(xyz.size() / 3); }
    uint32_t planeCount() const { return static_cast<uint32_t>(planes.size() / 4); }
    uint32_t primSize() const { return static_cast<uint32_t>(prim.size()); }

    uint32_t root() const { return word(0, 0); }

    // Word `index` of the node at byte `offset`; 0 when outside the payload.
    uint32_t word(uint32_t offset, uint32_t index) const;
    bool     wordInside(uint32_t offset, uint32_t index) const;
    uint8_t  type(uint32_t offset) const { return static_cast<uint8_t>(word(offset, 0)); }
    float    wordFloat(uint32_t offset, uint32_t index) const;

    // The NUL-terminated string at `offset` in STRN, or nullptr.
    const char* string(uint32_t offset) const;
};

// Parses a whole .3do. False with a reason for anything the game's own loader would reject.
bool parse3do(const uint8_t* data, size_t size, Model3do& out, std::string& error);

// gpl_nodeSizeBytes (0x4CF1B0): the byte size of the node at `offset`. 0 for an unknown type or
// a node that does not fit in the payload.
uint32_t primNodeSize(const Model3do& model, uint32_t offset);

// The linear sweep the loader makes: node sizes from offset 4 must tile the payload exactly.
bool primTiles(const Model3do& model, std::string& error);

// The sizes the game keeps in its Obj3d, read from the chunk headers alone.
struct Model3doSizes {
    uint32_t primSize = 0;
    uint32_t vertexCount = 0;
};

// Reads `size` bytes at `offset` of some source into `out`. True when all were read.
using ReadAt = bool (*)(void* context, uint64_t offset, void* out, size_t size);

// Walks the chunk headers of a .3do that starts at `base` and is `size` bytes long, reading
// only the 12-byte headers.
bool probe3doSizes(ReadAt read, void* context, uint64_t base, uint64_t size,
                   Model3doSizes& out);

}  // namespace baked
