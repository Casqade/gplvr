#include "baked/Mip.h"

#include <cstring>

namespace baked {
namespace {

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Multi-character constants stored little-endian, so ' MIP' is the bytes "PIM ".
bool tagIs(const uint8_t* p, const char* onDisk) { return std::memcmp(p, onDisk, 4) == 0; }

struct Chunk {
    const uint8_t* tag;
    const uint8_t* body;
    uint32_t       size;
};

// The chunks between `begin` and `end`, each padded to 4 bytes.
bool readChunks(const uint8_t* begin, const uint8_t* end, std::vector<Chunk>& out) {
    out.clear();
    const uint8_t* p = begin;
    while (p + 12 <= end) {
        uint32_t size = readU32(p + 8);
        if (static_cast<size_t>(end - (p + 12)) < size) return false;
        out.push_back(Chunk{p, p + 12, size});
        size_t padded = (static_cast<size_t>(size) + 3) & ~static_cast<size_t>(3);
        if (static_cast<size_t>(end - (p + 12)) < padded) break;
        p += 12 + padded;
    }
    return true;
}

int bitsPerPixel(int pixfmt) {
    switch (pixfmt / 3) {
    case 0: return 4;
    case 1: return 16;
    default: return 32;
    }
}

}  // namespace

size_t MipImage::bytes() const {
    size_t total = 0;
    for (const MipLevel& level : levels) total += level.data.size();
    return total;
}

bool parseMip(const uint8_t* data, size_t size, MipImage& out, std::string& error) {
    out = MipImage{};
    if (size < 12 || !tagIs(data, "PIM ")) {
        error = "not a MIP file";
        return false;
    }
    uint32_t rootSize = readU32(data + 8);
    if (rootSize > size - 12) {
        error = "MIP root chunk runs past the file";
        return false;
    }

    std::vector<Chunk> top;
    if (!readChunks(data + 12, data + 12 + rootSize, top)) {
        error = "MIP chunk framing";
        return false;
    }

    const uint8_t* sharedPalette = nullptr;
    uint32_t       levelCount = 0;
    bool           haveHeader = false;
    std::vector<Chunk> inner;

    for (const Chunk& chunk : top) {
        if (tagIs(chunk.tag, "DHPM")) {
            if (chunk.size < 18) break;
            out.pixfmt = chunk.body[0];
            out.average[0] = chunk.body[9];
            out.average[1] = chunk.body[10];
            out.average[2] = chunk.body[11];
            out.average[3] = chunk.body[12];
            out.flags = chunk.body[13];
            levelCount = readU32(chunk.body + 14);
            haveHeader = true;
        } else if (tagIs(chunk.tag, "PAMC")) {
            if (chunk.size >= 64) sharedPalette = chunk.body;
        } else if (tagIs(chunk.tag, "PAMB")) {
            if (!haveHeader) break;
            if (!readChunks(chunk.body, chunk.body + chunk.size, inner)) {
                error = "BMAP chunk framing";
                return false;
            }
            const uint8_t* palette = sharedPalette;
            const uint8_t* header = nullptr;
            const Chunk*   pixels = nullptr;
            for (const Chunk& part : inner) {
                if (tagIs(part.tag, "DHMB") && part.size >= 14) header = part.body;
                if (tagIs(part.tag, "PAMC") && part.size >= 64) palette = part.body;
                if (tagIs(part.tag, "ATAD")) pixels = &part;
            }
            if (!header || !pixels) {
                error = "BMAP without BMHD or DATA";
                return false;
            }

            int      pixfmt = header[0];
            uint32_t width = readU32(header + 1);
            uint32_t height = readU32(header + 5);
            uint32_t pitch = readU32(header + 9);
            if (pixfmt != out.pixfmt || pixfmt < 0 || pixfmt > 7 || width == 0 || height == 0 ||
                width > 16384 || height > 16384) {
                error = "BMHD format or size out of range";
                return false;
            }
            int      bpp = bitsPerPixel(pixfmt);
            uint64_t needPitch = (static_cast<uint64_t>(width) * bpp + 7) / 8;
            if (pitch < needPitch || static_cast<uint64_t>(pitch) * height > pixels->size) {
                error = "DATA shorter than the level";
                return false;
            }

            MipLevel level;
            level.width = static_cast<int>(width);
            level.height = static_cast<int>(height);
            if (bpp == 4) {
                if (!palette) {
                    error = "palette level without CMAP";
                    return false;
                }
                // High nibble is the left pixel (report 28 3.3). CMAP entries are B, G, R, A.
                level.data.resize(static_cast<size_t>(width) * height * 4);
                for (uint32_t y = 0; y < height; ++y) {
                    const uint8_t* row = pixels->body + static_cast<size_t>(pitch) * y;
                    uint8_t*       outRow = level.data.data() + static_cast<size_t>(width) * 4 * y;
                    for (uint32_t x = 0; x < width; ++x) {
                        uint8_t byte = row[x >> 1];
                        uint8_t index = (x & 1) ? (byte & 0x0F) : (byte >> 4);
                        const uint8_t* entry = palette + index * 4;
                        outRow[x * 4 + 0] = entry[0];
                        outRow[x * 4 + 1] = entry[1];
                        outRow[x * 4 + 2] = entry[2];
                        outRow[x * 4 + 3] = pixfmt == 0 ? 255 : entry[3];
                    }
                }
                out.expanded = true;
            } else {
                // Row padding is absent (pitch == width * bpp / 8), but copy row by row so a
                // padded file would still come out tight.
                size_t rowBytes = static_cast<size_t>(width) * (bpp / 8);
                level.data.resize(rowBytes * height);
                for (uint32_t y = 0; y < height; ++y)
                    std::memcpy(level.data.data() + rowBytes * y,
                                pixels->body + static_cast<size_t>(pitch) * y, rowBytes);
            }
            out.levels.push_back(std::move(level));
        }
    }

    if (!haveHeader || out.levels.empty()) {
        error = "MIP without MPHD or levels";
        return false;
    }
    if (levelCount && out.levels.size() > levelCount) out.levels.resize(levelCount);
    return true;
}

}  // namespace baked
