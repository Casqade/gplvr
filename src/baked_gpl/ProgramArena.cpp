#include "baked_gpl/ProgramArena.h"

#include "Log.h"

#include <windows.h>

#include <atomic>
#include <new>

namespace gplvr {
namespace {

// kBudgetBytes (CarProgram.cpp): ~700 programs of the 2020 grid's (~340-380 KiB each, report 105
// section 4.55), the mirror lists by glass at a full-field start. A 2 GiB process gets half: the
// track's bake is budgeted at half the address space free when it starts (TrackBake.cpp), and the
// section 4.55 run had 920 MiB free after the bake there, 418 in one piece after a reload.
constexpr size_t   kArenaLargeBytes = size_t{256} << 20;
constexpr size_t   kArenaSmallBytes = size_t{128} << 20;
constexpr uint32_t kMaxPages = static_cast<uint32_t>(kArenaLargeBytes / kArenaPageBytes);
constexpr uint32_t kWords = kMaxPages / 32;

// The region's bounds are written once, by programArenaInstall on the render thread before the
// car build worker starts (CarBuildWorker.h), and only read after: programArenaFree may compare
// against them from any thread. The page map and its counts are the render thread's alone.
char*         g_base = nullptr;
size_t        g_bytes = 0;
uint32_t      g_pages = 0;
uint32_t      g_used = 0;
uint32_t      g_committed = 0;
bool          g_tried = false;
unsigned long g_commitError = 0;
std::atomic<uint32_t> g_spills{0};
// One bit a page: a run holds it; it is committed. The words past g_pages read as held.
uint32_t      g_held[kWords] = {};
uint32_t      g_commit[kWords] = {};
// The open fill (ProgramArenaFill) of this thread: a fill redirects only the allocations of the
// thread that opened it, so the worker's heap scratch never lands in a run the render thread is
// filling.
thread_local char* g_at = nullptr;
thread_local char* g_end = nullptr;

bool bit(const uint32_t* map, uint32_t page) {
    return ((map[page >> 5] >> (page & 31)) & 1u) != 0;
}

void setBits(uint32_t* map, uint32_t first, uint32_t count, bool on) {
    for (uint32_t page = first; page < first + count; ++page) {
        if (on)
            map[page >> 5] |= 1u << (page & 31);
        else
            map[page >> 5] &= ~(1u << (page & 31));
    }
}

// The first run of `wanted` free pages.
bool findRun(uint32_t wanted, uint32_t& first) {
    uint32_t start = 0;
    uint32_t length = 0;
    for (uint32_t w = 0; w < kWords; ++w) {
        uint32_t bits = g_held[w];
        if (bits == 0xFFFFFFFFu) {
            length = 0;
            continue;
        }
        if (bits == 0) {
            if (!length) start = w * 32;
            length += 32;
            if (length >= wanted) {
                first = start;
                return true;
            }
            continue;
        }
        for (uint32_t b = 0; b < 32; ++b) {
            if ((bits >> b) & 1u) {
                length = 0;
                continue;
            }
            if (!length) start = w * 32 + b;
            if (++length >= wanted) {
                first = start;
                return true;
            }
        }
    }
    return false;
}

uint32_t largestFreeRun() {
    uint32_t largest = 0;
    uint32_t length = 0;
    for (uint32_t page = 0; page < g_pages; ++page) {
        length = bit(g_held, page) ? 0 : length + 1;
        if (length > largest) largest = length;
    }
    return largest;
}

double mib(uint64_t bytes) { return static_cast<double>(bytes) / 1048576.0; }

}  // namespace

bool programArenaInstall(uint64_t keepFree) {
    if (g_tried) return g_base != nullptr;
    g_tried = true;
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    bool   known = GlobalMemoryStatusEx(&status) != 0;
    size_t bytes = known && status.ullTotalVirtual > (uint64_t{2} << 30) ? kArenaLargeBytes
                                                                        : kArenaSmallBytes;
    const char*   why = nullptr;
    unsigned long error = 0;
    if (!known) {
        why = "GlobalMemoryStatusEx failed";
    } else if (status.ullAvailVirtual < bytes + keepFree) {
        why = "it would leave less than the floor of address space free";
    } else {
        void* base = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (!base) {
            error = GetLastError();
            why = "no free range that long";
        } else {
            g_base = static_cast<char*>(base);
            g_bytes = bytes;
            g_pages = static_cast<uint32_t>(bytes / kArenaPageBytes);
            setBits(g_held, g_pages, kMaxPages - g_pages, true);
        }
    }
    MEMORYSTATUSEX after = {};
    after.dwLength = sizeof(after);
    if (!GlobalMemoryStatusEx(&after)) after.ullAvailVirtual = 0;
    if (g_base)
        logf("baked cars: kept lists in a reserved region (report 105 section 4.59) -- %.0f MiB at "
             "%08X, %u pages of %zu KiB, committed as runs take them; the process has %.0f MiB of "
             "address space, %.0f MiB free before the reservation and %.0f after",
             mib(g_bytes), static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_base)), g_pages,
             kArenaPageBytes >> 10, mib(status.ullTotalVirtual), mib(status.ullAvailVirtual),
             mib(after.ullAvailVirtual));
    else
        logf("baked cars: kept lists on the heap -- a region of %.0f MiB (%zu KiB pages) was NOT "
             "reserved: %s (VirtualAlloc error %lu); the process has %.0f MiB of address space, "
             "%.0f MiB free, the floor is %.0f MiB",
             mib(bytes), kArenaPageBytes >> 10, why, error, mib(status.ullTotalVirtual),
             mib(after.ullAvailVirtual), mib(keepFree));
    return g_base != nullptr;
}

bool programArenaActive() { return g_base != nullptr; }

int programArenaTake(size_t bytes, ArenaRun& run) {
    if (!g_base || bytes > g_bytes) return kArenaTooBig;
    size_t wanted = (bytes + kArenaPageBytes - 1) / kArenaPageBytes;
    if (wanted == 0) wanted = 1;
    if (wanted > g_pages) return kArenaTooBig;
    auto pages = static_cast<uint32_t>(wanted);
    if (g_pages - g_used < pages) return kArenaNoRoom;
    uint32_t first = 0;
    if (!findRun(pages, first)) return kArenaNoRun;
    if (!VirtualAlloc(g_base + first * kArenaPageBytes, pages * kArenaPageBytes, MEM_COMMIT,
                      PAGE_READWRITE)) {
        g_commitError = GetLastError();
        return kArenaCommitFailed;
    }
    for (uint32_t page = first; page < first + pages; ++page)
        if (!bit(g_commit, page)) ++g_committed;
    setBits(g_commit, first, pages, true);
    setBits(g_held, first, pages, true);
    g_used += pages;
    run.first = first;
    run.pages = pages;
    return kArenaTaken;
}

void programArenaGive(ArenaRun& run) {
    if (run.pages && g_base && run.first + run.pages <= g_pages) {
        setBits(g_held, run.first, run.pages, false);
        g_used -= run.pages;
    }
    run = ArenaRun{};
}

void programArenaTrim() {
    if (!g_base) return;
    uint32_t page = 0;
    while (page < g_pages) {
        if (bit(g_held, page) || !bit(g_commit, page)) {
            ++page;
            continue;
        }
        uint32_t first = page;
        while (page < g_pages && !bit(g_held, page) && bit(g_commit, page)) ++page;
        if (VirtualFree(g_base + first * kArenaPageBytes, (page - first) * kArenaPageBytes,
                        MEM_DECOMMIT)) {
            setBits(g_commit, first, page - first, false);
            g_committed -= page - first;
        }
    }
}

ArenaUse programArenaUse() {
    ArenaUse use;
    use.bytes = g_bytes;
    use.pages = g_pages;
    use.used = g_used;
    use.committed = g_committed;
    use.largestFree = largestFreeRun();
    return use;
}

unsigned long programArenaCommitError() { return g_commitError; }

uint32_t programArenaSpills() { return g_spills.load(std::memory_order_relaxed); }

void* programArenaAllocate(size_t bytes) {
    if (g_at) {
        size_t block = programArenaBlockBytes(bytes);
        if (block >= bytes && block <= static_cast<size_t>(g_end - g_at)) {
            void* at = g_at;
            g_at += block;
            return at;
        }
        g_spills.fetch_add(1, std::memory_order_relaxed);
    }
    return ::operator new(bytes);
}

void programArenaFree(void* block) noexcept {
    if (!block) return;
    if (reinterpret_cast<uintptr_t>(block) - reinterpret_cast<uintptr_t>(g_base) < g_bytes) return;
    ::operator delete(block);
}

ProgramArenaFill::ProgramArenaFill(const ArenaRun& run) : at_(g_at), end_(g_end) {
    if (g_base && run.pages && run.first + run.pages <= g_pages) {
        g_at = g_base + run.first * kArenaPageBytes;
        g_end = g_at + run.pages * kArenaPageBytes;
    } else {
        g_at = nullptr;
        g_end = nullptr;
    }
}

ProgramArenaFill::~ProgramArenaFill() {
    g_at = at_;
    g_end = end_;
}

}  // namespace gplvr
