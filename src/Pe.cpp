#include "Pe.h"

#include <algorithm>
#include <cstring>

namespace gplvr {
namespace {

const IMAGE_NT_HEADERS32* ntHeaders(const uint8_t* base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    return nt;
}

}  // namespace

bool getModuleImage(HMODULE module, ModuleImage& out) {
    auto base = reinterpret_cast<const uint8_t*>(module);
    if (!base) return false;
    const IMAGE_NT_HEADERS32* nt = ntHeaders(base);
    if (!nt) return false;

    out.base = base;
    out.size = nt->OptionalHeader.SizeOfImage;
    out.sections.clear();
    out.textBegin = nullptr;
    out.textEnd = nullptr;

    auto header = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++header) {
        Section section;
        section.begin = base + header->VirtualAddress;
        section.end = section.begin + header->Misc.VirtualSize;
        section.executable = (header->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        section.writable = (header->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        section.hasRawData = header->SizeOfRawData != 0;
        out.sections.push_back(section);

        if (section.executable && !out.textBegin) {
            out.textBegin = section.begin;
            out.textEnd = section.end;
        }
    }
    return out.textBegin != nullptr;
}

bool isWritable(const ModuleImage& image, const void* p) {
    auto address = reinterpret_cast<const uint8_t*>(p);
    for (const Section& section : image.sections)
        if (address >= section.begin && address < section.end) return section.writable;
    return false;
}

std::vector<const uint8_t*> findAll(const uint8_t* begin, const uint8_t* end,
                                    const void* pattern, size_t length) {
    std::vector<const uint8_t*> hits;
    if (!begin || !end || length == 0 || static_cast<size_t>(end - begin) < length)
        return hits;

    auto pat = reinterpret_cast<const uint8_t*>(pattern);
    const uint8_t* limit = end - length;
    for (const uint8_t* p = begin; p <= limit; ++p) {
        if (*p == pat[0] && std::memcmp(p, pat, length) == 0) hits.push_back(p);
    }
    return hits;
}

std::vector<const uint8_t*> findAllInImage(const ModuleImage& image, const void* pattern,
                                           size_t length) {
    std::vector<const uint8_t*> hits;
    for (const Section& section : image.sections) {
        if (!section.hasRawData) continue;
        auto found = findAll(section.begin, section.end, pattern, length);
        hits.insert(hits.end(), found.begin(), found.end());
    }
    return hits;
}

const char* findString(const ModuleImage& image, const char* text) {
    const size_t length = std::strlen(text) + 1;  // include the NUL
    auto hits = findAllInImage(image, text, length);
    return hits.empty() ? nullptr : reinterpret_cast<const char*>(hits.front());
}

std::vector<uintptr_t> collectCallTargets(const ModuleImage& image) {
    std::vector<uintptr_t> targets;
    for (const Section& section : image.sections) {
        if (!section.executable || !section.hasRawData) continue;
        if (static_cast<size_t>(section.end - section.begin) < 5) continue;

        const uint8_t* limit = section.end - 5;
        for (const uint8_t* p = section.begin; p <= limit; ++p) {
            if (*p != 0xE8) continue;
            int32_t rel;
            std::memcpy(&rel, p + 1, sizeof(rel));
            auto target = reinterpret_cast<uintptr_t>(p) + 5 + rel;
            if (image.inText(reinterpret_cast<const void*>(target))) targets.push_back(target);
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

uintptr_t functionOf(const std::vector<uintptr_t>& entries, uintptr_t address) {
    auto it = std::upper_bound(entries.begin(), entries.end(), address);
    if (it == entries.begin()) return 0;
    return *(it - 1);
}

uintptr_t functionEnd(const std::vector<uintptr_t>& entries, const ModuleImage& image,
                      uintptr_t entry) {
    auto it = std::upper_bound(entries.begin(), entries.end(), entry);
    if (it == entries.end()) return reinterpret_cast<uintptr_t>(image.textEnd);
    return *it;
}

std::vector<uintptr_t> findCallSites(const ModuleImage& image, uintptr_t target) {
    std::vector<uintptr_t> sites;
    for (const Section& section : image.sections) {
        if (!section.executable || !section.hasRawData) continue;
        if (static_cast<size_t>(section.end - section.begin) < 5) continue;

        const uint8_t* limit = section.end - 5;
        for (const uint8_t* p = section.begin; p <= limit; ++p) {
            if (*p != 0xE8) continue;
            int32_t rel;
            std::memcpy(&rel, p + 1, sizeof(rel));
            if (reinterpret_cast<uintptr_t>(p) + 5 + rel == target)
                sites.push_back(reinterpret_cast<uintptr_t>(p));
        }
    }
    std::sort(sites.begin(), sites.end());
    return sites;
}

bool patchBytes(void* address, const void* data, size_t length) {
    DWORD previous = 0;
    if (!VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(address, data, length);
    DWORD ignored = 0;
    VirtualProtect(address, length, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, length);
    return true;
}

}  // namespace gplvr
