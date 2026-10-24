#include "baked/Assets.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace baked {
namespace {

#if defined(_WIN32)
constexpr char kSeparator = '\\';
#else
constexpr char kSeparator = '/';
#endif

FILE* openRead(const std::string& path) {
#if defined(_MSC_VER)
    FILE* file = nullptr;
    return fopen_s(&file, path.c_str(), "rb") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool seekTo(FILE* file, uint64_t offset) {
#if defined(_MSC_VER)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
#endif
}

uint64_t fileSize(FILE* file) {
#if defined(_MSC_VER)
    if (_fseeki64(file, 0, SEEK_END) != 0) return 0;
    __int64 size = _ftelli64(file);
#else
    if (std::fseek(file, 0, SEEK_END) != 0) return 0;
    long size = std::ftell(file);
#endif
    return size > 0 ? static_cast<uint64_t>(size) : 0;
}

bool readRange(const std::string& path, uint64_t offset, void* out, size_t size) {
    FILE* file = openRead(path);
    if (!file) return false;
    bool ok = seekTo(file, offset) && std::fread(out, 1, size, file) == size;
    std::fclose(file);
    return ok;
}

struct LooseContext {
    const std::string* path;
};

bool readLooseAt(void* context, uint64_t offset, void* out, size_t size) {
    return readRange(*static_cast<LooseContext*>(context)->path, offset, out, size);
}

bool readArchiveAt(void* context, uint64_t offset, void* out, size_t size) {
    return static_cast<const DatArchive*>(context)->readAt(offset, out, size);
}

}  // namespace

std::string lowerCase(const std::string& text) {
    std::string out = text;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool DatArchive::open(const std::string& path) {
    path_.clear();
    entries_.clear();

    FILE* file = openRead(path);
    if (!file) return false;
    uint64_t size = fileSize(file);
    uint8_t  countBytes[2];
    bool     ok = size >= 2 && seekTo(file, 0) && std::fread(countBytes, 1, 2, file) == 2;
    uint32_t count = ok ? (static_cast<uint32_t>(countBytes[0]) | (countBytes[1] << 8)) : 0;
    std::vector<uint8_t> directory(static_cast<size_t>(count) * 27);
    ok = ok && count > 0 && 2 + directory.size() <= size &&
         std::fread(directory.data(), 1, directory.size(), file) == directory.size();
    std::fclose(file);
    if (!ok) return false;

    // 27-byte entries: u16 flags, u32 size, u32 size again, char name[13], u32 offset.
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = directory.data() + static_cast<size_t>(i) * 27;
        if (entry[1] != 0) continue;  // gpl_datDirectoryLookup skips a non-zero flags high byte
        uint32_t entrySize = 0;
        uint32_t offset = 0;
        std::memcpy(&entrySize, entry + 2, 4);
        std::memcpy(&offset, entry + 23, 4);
        char name[14] = {};
        std::memcpy(name, entry + 10, 13);
        if (static_cast<uint64_t>(offset) + entrySize > size) continue;
        entries_.emplace(lowerCase(name), Entry{offset, entrySize});
    }
    path_ = path;
    fileSize_ = size;
    return true;
}

bool DatArchive::find(const std::string& name, uint32_t& offset, uint32_t& size) const {
    auto found = entries_.find(lowerCase(name));
    if (found == entries_.end()) return false;
    offset = found->second.offset;
    size = found->second.size;
    return true;
}

bool DatArchive::read(uint32_t offset, uint32_t size, std::vector<uint8_t>& out) const {
    out.resize(size);
    return size == 0 || readAt(offset, out.data(), size);
}

bool DatArchive::readAt(uint64_t offset, void* out, size_t size) const {
    if (path_.empty() || offset + size > fileSize_) return false;
    return readRange(path_, offset, out, size);
}

bool TrackAssets::open(const std::string& directory, const std::string& season) {
    directory_ = directory;
    while (!directory_.empty() &&
           (directory_.back() == '\\' || directory_.back() == '/'))
        directory_.pop_back();
    size_t slash = directory_.find_last_of("\\/");
    name_ = slash == std::string::npos ? directory_ : directory_.substr(slash + 1);
    season_ = season;
    // A track without an archive is legal; everything is then loose.
    archive_.open(directory_ + kSeparator + name_ + ".dat");
    return !name_.empty();
}

std::string TrackAssets::seasonName(const std::string& fileName) const {
    if (season_.size() != 2) return std::string();
    size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string extension = lowerCase(fileName.substr(dot + 1));
    char        letter = 0;
    if (extension == "3do")
        letter = '3';
    else if (extension == "mip")
        letter = 'm';
    else if (extension == "srb")
        letter = 's';
    else
        return std::string();
    return fileName.substr(0, dot + 1) + letter + season_;
}

bool TrackAssets::readLoose(const std::string& fileName, std::vector<uint8_t>& out) const {
    FILE* file = openRead(directory_ + kSeparator + fileName);
    if (!file) return false;
    uint64_t size = fileSize(file);
    out.resize(static_cast<size_t>(size));
    bool ok = seekTo(file, 0) && (size == 0 || std::fread(out.data(), 1, out.size(), file) ==
                                                   out.size());
    std::fclose(file);
    return ok;
}

bool TrackAssets::read(const std::string& fileName, std::vector<uint8_t>& out,
                       std::string* where) const {
    std::string seasonal = seasonName(fileName);
    if (!seasonal.empty() && readLoose(seasonal, out)) {
        if (where) *where = seasonal;
        return true;
    }
    if (readLoose(fileName, out)) {
        if (where) *where = fileName;
        return true;
    }
    uint32_t offset = 0;
    uint32_t size = 0;
    if (archive_.isOpen() && archive_.find(fileName, offset, size) &&
        archive_.read(offset, size, out)) {
        if (where) *where = name_ + ".dat!" + fileName;
        return true;
    }
    return false;
}

bool TrackAssets::inArchive(const std::string& fileName) const {
    uint32_t offset = 0;
    uint32_t size = 0;
    return archive_.isOpen() && archive_.find(fileName, offset, size);
}

bool TrackAssets::probe3do(const std::string& fileName, Model3doSizes& sizes,
                           std::string* where) const {
    std::string candidates[2] = {seasonName(fileName), fileName};
    for (const std::string& candidate : candidates) {
        if (candidate.empty()) continue;
        std::string path = directory_ + kSeparator + candidate;
        FILE*       file = openRead(path);
        if (!file) continue;
        uint64_t size = fileSize(file);
        std::fclose(file);
        LooseContext context{&path};
        if (probe3doSizes(&readLooseAt, &context, 0, size, sizes)) {
            if (where) *where = candidate;
            return true;
        }
        return false;
    }
    uint32_t offset = 0;
    uint32_t size = 0;
    if (archive_.isOpen() && archive_.find(fileName, offset, size) &&
        probe3doSizes(&readArchiveAt, const_cast<void*>(static_cast<const void*>(&archive_)),
                      offset, size, sizes)) {
        if (where) *where = name_ + ".dat!" + fileName;
        return true;
    }
    return false;
}

}  // namespace baked
