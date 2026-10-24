#pragma once

// Where a track's files come from (docs/audit-2026-09-09/28-dat-mip-and-sidecars.md section 6):
// a loose file in the track directory wins, then the directory's own <dir>.dat. With a season
// patch installed a loose <stem>.<season ext> is tried first (report 28 1.1: the patch looks for
// the rewritten name with FindFirstFileA, so only loose files take part).

#include "baked/Piff.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace baked {

class DatArchive {
public:
    bool open(const std::string& path);
    bool isOpen() const { return !path_.empty(); }
    // Case-insensitive, as gpl_datDirectoryLookup (_strcmpi).
    bool find(const std::string& name, uint32_t& offset, uint32_t& size) const;
    bool read(uint32_t offset, uint32_t size, std::vector<uint8_t>& out) const;
    bool readAt(uint64_t offset, void* out, size_t size) const;

private:
    struct Entry {
        uint32_t offset;
        uint32_t size;
    };
    std::string                            path_;
    uint64_t                               fileSize_ = 0;
    std::unordered_map<std::string, Entry> entries_;
};

class TrackAssets {
public:
    // `directory` without a trailing separator, e.g. "...\tracks\spa67". `season` is the two
    // digits of the installed season patch ("67"), or empty for none.
    bool open(const std::string& directory, const std::string& season);

    const std::string& directory() const { return directory_; }
    const std::string& name() const { return name_; }

    // The file's bytes and where they came from ("loose name" or "archive.dat!name").
    bool read(const std::string& fileName, std::vector<uint8_t>& out, std::string* where) const;

    // Whether the directory's own <dir>.dat holds `fileName` (case-insensitive). Loose files are
    // not looked at.
    bool inArchive(const std::string& fileName) const;

    // The PRIM size and vertex count of a .3do, from its chunk headers.
    bool probe3do(const std::string& fileName, Model3doSizes& sizes, std::string* where) const;

private:
    std::string seasonName(const std::string& fileName) const;
    bool        readLoose(const std::string& fileName, std::vector<uint8_t>& out) const;

    std::string directory_;
    std::string name_;
    std::string season_;
    DatArchive  archive_;
};

std::string lowerCase(const std::string& text);

}  // namespace baked
