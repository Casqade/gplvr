#include "AddressSpace.h"

#include <windows.h>

namespace gplvr {

uint64_t addressSpaceLargestFree() {
    SYSTEM_INFO system = {};
    GetSystemInfo(&system);
    auto     at = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    auto     end = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    uint64_t largest = 0;
    MEMORY_BASIC_INFORMATION info = {};
    while (at < end && VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info))) {
        if (info.State == MEM_FREE && info.RegionSize > largest) largest = info.RegionSize;
        uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= at) break;
        at = next;
    }
    return largest;
}

AddressSpace addressSpaceNow() {
    AddressSpace space;
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) space.freeBytes = status.ullAvailVirtual;
    space.largestFreeBytes = addressSpaceLargestFree();
    return space;
}

}  // namespace gplvr
