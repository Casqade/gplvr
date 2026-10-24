#include "baked/Piff.h"

#include <cstring>

namespace baked {
namespace {

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float readFloat(const uint8_t* p) {
    float value = 0.0f;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// Sub-chunk tags as the bytes appear on disk: the FourCC packed big-endian, so each name reads
// backwards.
bool tagIs(const uint8_t* p, const char* onDisk) { return std::memcmp(p, onDisk, 4) == 0; }

// sub_4CC7B0: MATERIAL size from its flags word.
uint32_t materialSize(uint32_t flags, uint32_t word3) {
    uint32_t texture = 0;
    if (((flags & 1) && !(flags & 0x10)) || (flags & 0x20))
        texture = 4;
    else if ((flags & 1) && (flags & 0x10))
        texture = 4 * word3 + 4;
    uint32_t unknown2 = (flags & 2) ? 4 : 0;
    uint32_t surfaceClass = (flags & 4) ? 4 : 0;
    uint32_t unknown8 = (flags & 8) ? 8 : 0;
    return 12 + texture + unknown2 + surfaceClass + unknown8;
}

}  // namespace

uint32_t Model3do::word(uint32_t offset, uint32_t index) const {
    if (!wordInside(offset, index)) return 0;
    return readU32(prim.data() + offset + 4u * index);
}

bool Model3do::wordInside(uint32_t offset, uint32_t index) const {
    uint64_t at = static_cast<uint64_t>(offset) + 4u * static_cast<uint64_t>(index);
    return at + 4 <= prim.size();
}

float Model3do::wordFloat(uint32_t offset, uint32_t index) const {
    if (!wordInside(offset, index)) return 0.0f;
    return readFloat(prim.data() + offset + 4u * index);
}

const char* Model3do::string(uint32_t offset) const {
    if (offset >= strings.size()) return nullptr;
    const void* end = std::memchr(strings.data() + offset, 0, strings.size() - offset);
    return end ? strings.data() + offset : nullptr;
}

bool parse3do(const uint8_t* data, size_t size, Model3do& out, std::string& error) {
    out = Model3do{};
    if (size < 12 || !tagIs(data, "4OD3")) {
        error = "not a 3DO container (magic)";
        return false;
    }
    out.version = readU32(data + 4);
    uint64_t end = 12 + static_cast<uint64_t>(readU32(data + 8));
    if (end > size) {
        error = "container size runs past the end of the file";
        return false;
    }

    bool     havePrim = false;
    uint64_t pos = 12;
    while (pos + 12 <= end) {
        const uint8_t* header = data + pos;
        uint32_t       chunkSize = readU32(header + 8);
        uint64_t       payload = pos + 12;
        if (payload + chunkSize > end) {
            error = "chunk runs past the container";
            return false;
        }
        const uint8_t* body = data + payload;

        if (tagIs(header, "SZYX") || tagIs(header, "MRON")) {
            std::vector<float>& target = tagIs(header, "SZYX") ? out.xyz : out.normals;
            uint32_t            count = chunkSize / 16;
            target.resize(static_cast<size_t>(count) * 3);
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t k = 0; k < 3; ++k)
                    target[i * 3 + k] = readFloat(body + i * 16 + 4 + k * 4);
        } else if (tagIs(header, "NALP")) {
            uint32_t count = chunkSize / 16;
            out.planes.resize(static_cast<size_t>(count) * 4);
            for (uint32_t i = 0; i < count * 4; ++i) out.planes[i] = readFloat(body + i * 4);
        } else if (tagIs(header, "NRTS")) {
            out.strings.assign(reinterpret_cast<const char*>(body),
                               reinterpret_cast<const char*>(body) + chunkSize);
        } else if (tagIs(header, "DNAH")) {
            uint32_t count = chunkSize / 8;
            out.handles.resize(static_cast<size_t>(count) * 2);
            for (uint32_t i = 0; i < count * 2; ++i) out.handles[i] = readU32(body + i * 4);
        } else if (tagIs(header, "MIRP")) {
            out.prim.assign(body, body + chunkSize);
            havePrim = true;
        } else if (tagIs(header, "CXET")) {
            // TEXC: no shipped file has one and nothing in the game reads it (report 26 3.4).
        } else {
            // PREP and unknown tags both abort the game's own load (report 26 1.4).
            error = "unsupported chunk";
            return false;
        }
        pos = payload + ((static_cast<uint64_t>(chunkSize) + 3) & ~static_cast<uint64_t>(3));
    }

    if (!havePrim || out.prim.size() < 8) {
        error = "no PRIM chunk";
        return false;
    }
    if (out.root() < 4 || out.root() >= out.prim.size()) {
        error = "root node offset outside PRIM";
        return false;
    }
    return primTiles(out, error);
}

uint32_t primNodeSize(const Model3do& model, uint32_t offset) {
    if (!model.wordInside(offset, 0)) return 0;
    uint32_t w0 = model.word(offset, 0);
    uint8_t  type = static_cast<uint8_t>(w0);

    // Header words a size formula reads must exist before the formula is trusted.
    auto need = [&](uint32_t index) { return model.wordInside(offset, index); };
    uint64_t bytes = 0;
    switch (type) {
    case 0x01: bytes = 4; break;
    case 0x03: case 0x08: bytes = 24; break;
    case 0x04: case 0x17:
        if (!need(1)) return 0;
        bytes = 4ull * model.word(offset, 1) + 8;
        break;
    case 0x05:
        if (!need(3)) return 0;
        bytes = materialSize(model.word(offset, 2), model.word(offset, 3));
        break;
    case 0x06: case 0x0E: case 0x14: bytes = 12; break;
    case 0x07: case 0x0B: case 0x15: case 0x18: bytes = 16; break;
    case 0x09: case 0x0A: bytes = 20; break;
    case 0x0C: case 0x1B:
        if (!need(2)) return 0;
        bytes = 4ull * model.word(offset, 2) + 12;
        break;
    case 0x0D: case 0x13: case 0x16: bytes = 36; break;
    case 0x0F: case 0x1A:
        if (!need(15)) return 0;
        bytes = 16ull * (static_cast<uint64_t>(model.word(offset, 15)) + 4);
        break;
    case 0x10:
        if (!need(1)) return 0;
        bytes = 4ull * model.word(offset, 1) + 8;
        break;
    case 0x11:
        if (!need(4)) return 0;
        bytes = 8ull * model.word(offset, 4) + 20;
        break;
    case 0x12: bytes = 28; break;
    case 0x19: bytes = 40; break;
    case 0x1C: case 0x1D:
        if (!need(2)) return 0;
        bytes = 8ull * model.word(offset, 2) + 12;
        break;
    case 0x1E:
        if (!need(2)) return 0;
        bytes = 12ull * (static_cast<uint64_t>(model.word(offset, 2)) + 1);
        break;
    case 0x1F:
        if (!need(3)) return 0;
        bytes = 12ull * model.word(offset, 3) + 16;
        break;
    case 0x20: case 0x21:
        if (!need(3)) return 0;
        bytes = 16ull * (static_cast<uint64_t>(model.word(offset, 3)) + 1);
        break;
    case 0x22:
        if (!need(3)) return 0;
        bytes = 20ull * model.word(offset, 3) + 16;
        break;
    default: return 0;
    }
    if (static_cast<uint64_t>(offset) + bytes > model.prim.size()) return 0;
    return static_cast<uint32_t>(bytes);
}

bool primTiles(const Model3do& model, std::string& error) {
    uint32_t offset = 4;
    while (offset < model.prim.size()) {
        uint32_t size = primNodeSize(model, offset);
        if (!size) {
            error = "PRIM node of unknown type or size";
            return false;
        }
        offset += size;
    }
    if (offset != model.prim.size()) {
        error = "PRIM nodes do not tile the payload";
        return false;
    }
    return true;
}

bool probe3doSizes(ReadAt read, void* context, uint64_t base, uint64_t size,
                   Model3doSizes& out) {
    out = Model3doSizes{};
    uint8_t header[12];
    if (size < 12 || !read(context, base, header, sizeof(header)) || !tagIs(header, "4OD3"))
        return false;
    uint64_t end = 12 + static_cast<uint64_t>(readU32(header + 8));
    if (end > size) return false;

    bool     havePrim = false;
    uint64_t pos = 12;
    while (pos + 12 <= end) {
        if (!read(context, base + pos, header, sizeof(header))) return false;
        uint32_t chunkSize = readU32(header + 8);
        if (pos + 12 + chunkSize > end) return false;
        if (tagIs(header, "SZYX")) out.vertexCount = chunkSize / 16;
        if (tagIs(header, "MIRP")) {
            out.primSize = chunkSize;
            havePrim = true;
        }
        pos += 12 + ((static_cast<uint64_t>(chunkSize) + 3) & ~static_cast<uint64_t>(3));
    }
    return havePrim;
}

}  // namespace baked
