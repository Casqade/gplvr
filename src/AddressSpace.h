#pragma once

// The game process's address space, for pricing large GPU allocations.
//
// gplc67.exe is 32-bit: 2 GiB, or 4 GiB when it is /LARGEADDRESSAWARE. A render target or a
// shared texture lives in video memory, but the driver may also map some or all of it into
// the process, and that share -- not the video memory -- is what decides whether the next one
// can be allocated.

#include <cstdint>

namespace gplvr {

struct AddressSpace {
    uint64_t freeBytes = 0;
    uint64_t largestFreeBytes = 0;  // what one contiguous allocation must fit in
};

AddressSpace addressSpaceNow();

// The largest free region alone, for callers that need nothing else.
uint64_t addressSpaceLargestFree();

}  // namespace gplvr
