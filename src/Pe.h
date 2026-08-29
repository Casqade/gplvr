#pragma once

// Minimal PE walking / byte scanning. 32-bit, in-process, no dependencies.

#include <windows.h>

#include <cstdint>
#include <vector>

namespace gplvr {

struct Section {
    const uint8_t* begin = nullptr;
    const uint8_t* end = nullptr;
    bool           executable = false;
    bool           writable = false;
    bool           hasRawData = false;  // false for .bss-like sections
};

struct ModuleImage {
    const uint8_t*       base = nullptr;
    size_t               size = 0;
    std::vector<Section> sections;

    // The first executable section: the original .text. GEM appends its own code
    // sections (modtext) after it, and we only ever hook vanilla code.
    const uint8_t* textBegin = nullptr;
    const uint8_t* textEnd = nullptr;

    bool inText(const void* p) const { return p >= textBegin && p < textEnd; }
};

bool getModuleImage(HMODULE module, ModuleImage& out);

// True if `p` lies in a writable section -- used to sanity-check that a discovered
// global really is a variable and not a misparsed immediate.
bool isWritable(const ModuleImage& image, const void* p);

// All occurrences of an exact byte run within [begin, end).
std::vector<const uint8_t*> findAll(const uint8_t* begin, const uint8_t* end,
                                    const void* pattern, size_t length);

// As above, but across every section that has raw data -- gplc67 keeps the
// rasterizer name literals in modtext, which is executable, so neither code nor
// data sections can be excluded. Sections without raw data (14 MB of modbss) are.
std::vector<const uint8_t*> findAllInImage(const ModuleImage& image, const void* pattern,
                                           size_t length);

// First occurrence of a NUL-terminated ASCII literal anywhere in the image.
const char* findString(const ModuleImage& image, const char* text);

// Every target of a near call (E8 rel32) found in an executable section whose
// destination lands inside .text. Sorted and de-duplicated: our approximation of
// "the set of function entry points".
std::vector<uintptr_t> collectCallTargets(const ModuleImage& image);

// Greatest entry point <= address, i.e. the function `address` belongs to.
uintptr_t functionOf(const std::vector<uintptr_t>& entries, uintptr_t address);

// End of the function starting at `entry` (== the next entry point, or textEnd).
uintptr_t functionEnd(const std::vector<uintptr_t>& entries, const ModuleImage& image,
                      uintptr_t entry);

// Call sites (address of the E8 opcode) targeting `target`.
std::vector<uintptr_t> findCallSites(const ModuleImage& image, uintptr_t target);

// Overwrite `length` bytes at `address`, handling page protection.
bool patchBytes(void* address, const void* data, size_t length);

}  // namespace gplvr
