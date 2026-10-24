#include "baked/Srb.h"

#include <cstring>

namespace baked {
namespace {

uint32_t u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool parseSrb(const uint8_t* data, size_t size, SrbSprite& out, MipImage& image,
              std::string& error) {
    out = SrbSprite{};
    error.clear();
    if (size < 12 || std::memcmp(data, " BRS", 4) != 0) {
        error = "not an SRB file";
        return false;
    }
    // The SRB chunk, then the MIP chunk after it (each chunk: tag, a word, size, body).
    size_t srbSize = u32(data + 8);
    size_t mipAt = 12 + ((srbSize + 3) & ~static_cast<size_t>(3));
    if (srbSize > size - 12 || mipAt > size) {
        error = "SRB chunk runs past the file";
        return false;
    }
    bool   header = false;
    size_t at = 12;
    size_t end = 12 + srbSize;
    while (at + 12 <= end) {
        const uint8_t* chunk = data + at;
        size_t         chunkSize = u32(chunk + 8);
        if (chunkSize > end - at - 12) break;
        if (std::memcmp(chunk, "DHRS", 4) == 0 && chunkSize == 24) {
            float f[6];
            std::memcpy(f, chunk + 12, sizeof(f));
            // GPL divides by the pixel size unchecked; a zero there makes the quad infinite
            // (AAIOM has three such files), and those stay the game's.
            error = "SRHD pixel size 0";
            if (f[2] > 0.0f && f[3] > 0.0f) {
                out.width = f[0];
                out.height = f[1];
                out.leftNear = (f[4] - f[2]) / f[2];
                out.upLow = (f[5] - f[3]) / f[3];
                header = true;
            }
        } else if (std::memcmp(chunk, "2HRS", 4) == 0 && chunkSize == 36) {
            out.facing = chunk[12] != 0;
        }
        at += 12 + ((chunkSize + 3) & ~static_cast<size_t>(3));
    }
    if (!header) {
        if (error.empty()) error = "no SRHD chunk";
        return false;
    }
    if (!parseMip(data + mipAt, size - mipAt, image, error)) {
        error = "embedded MIP: " + error;
        return false;
    }
    if (image.levels.empty() || image.levels[0].height <= 0) {
        error = "embedded MIP has no height";
        return false;
    }
    out.aspect = static_cast<float>(image.levels[0].width) /
                 static_cast<float>(image.levels[0].height);
    return true;
}

}  // namespace baked
