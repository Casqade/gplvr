#pragma once

// The kept car programs' storage (docs/audit-2026-09-18/105-baked-renderer-rewrite.md section
// 4.59): one range of address space reserved at install, while the 32-bit process's address space
// is still in one piece, and cut into pages. A kept program's vectors are copied into one run of
// pages, packed end to end, so programs never interleave with GPL's, roglv2's or gplvr's other
// allocations, the pages in use are an exact count of what the programs hold, and dropping a
// program frees its run in one step. Pages are committed when a run first takes them and stay
// committed while the region holds programs; programArenaTrim decommits the free ones.
//
// The vectors a kept program holds are ArenaVectors: std::vector with an allocator that bumps
// through the run a ProgramArenaFill has open, and takes the heap otherwise (the walk's tapes, a
// build's scratch, every program when no region could be reserved). A block is given back by its
// address: one inside the region is freed with its run, any other goes back to the heap.
//
// The page map (install, Take, Give, Trim, Use) is the render thread's only: programs are kept,
// taken and dropped in the baked passes. A ProgramArenaFill redirects the allocations of the thread
// that opened it alone, so ArenaVectors on the car build worker (CarBuildWorker.h) always take the
// heap; a block is given back correctly from any thread.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gplvr {

constexpr size_t kArenaPageBytes = size_t{16} << 10;
constexpr size_t kArenaAlign = 8;  // every block starts on it; no kept element needs more

// A run of pages of the region: `pages` 0 is none.
struct ArenaRun {
    uint32_t first = 0;
    uint32_t pages = 0;
};

enum ArenaTake : int {
    kArenaTaken,
    kArenaNoRoom,        // fewer pages free than the run needs
    kArenaNoRun,         // enough pages free, but not in one run
    kArenaTooBig,        // the run is longer than the region, or there is no region
    kArenaCommitFailed,  // VirtualAlloc could not commit the run (programArenaCommitError)
};

struct ArenaUse {
    size_t   bytes = 0;      // reserved; 0 without a region
    uint32_t pages = 0;
    uint32_t used = 0;
    uint32_t committed = 0;
    uint32_t largestFree = 0;  // the longest run of free pages
};

// From carProgramInstall, once: reserves the region (256 MiB in a process with more than 2 GiB of
// address space, else 128 MiB), unless less than `keepFree` bytes of address space would be left
// free. Logs one line either way. True when the region is in use.
bool programArenaInstall(uint64_t keepFree);
bool programArenaActive();

// The bytes a block of `bytes` takes in a run.
inline size_t programArenaBlockBytes(size_t bytes) {
    return (bytes + kArenaAlign - 1) & ~(kArenaAlign - 1);
}

// A run holding `bytes`, committed: first fit. An ArenaTake; `run` is set only when kArenaTaken.
int programArenaTake(size_t bytes, ArenaRun& run);
// Frees `run`'s pages (they stay committed) and empties it.
void programArenaGive(ArenaRun& run);
// Decommits every committed page no run holds.
void programArenaTrim();
// Its pages now; largestFree is found by a scan of the page map.
ArenaUse programArenaUse();
// GetLastError of the last commit that failed.
unsigned long programArenaCommitError();
// Blocks asked of an open fill that did not fit its run, and came from the heap.
uint32_t programArenaSpills();

// ArenaAllocator's two ends.
void* programArenaAllocate(size_t bytes);
void  programArenaFree(void* block) noexcept;

// While one is alive, ArenaVector blocks are taken from `run` in turn. A run of no pages takes
// every block from the heap.
class ProgramArenaFill {
public:
    explicit ProgramArenaFill(const ArenaRun& run);
    ~ProgramArenaFill();
    ProgramArenaFill(const ProgramArenaFill&) = delete;
    ProgramArenaFill& operator=(const ProgramArenaFill&) = delete;

private:
    char* at_;
    char* end_;
};

template <typename T>
struct ArenaAllocator {
    static_assert(alignof(T) <= kArenaAlign, "a run's blocks are aligned to kArenaAlign");
    using value_type = T;

    ArenaAllocator() noexcept = default;
    template <typename U>
    ArenaAllocator(const ArenaAllocator<U>&) noexcept {}

    T*   allocate(size_t count) { return static_cast<T*>(programArenaAllocate(count * sizeof(T))); }
    void deallocate(T* block, size_t) noexcept { programArenaFree(block); }
};

template <typename T, typename U>
bool operator==(const ArenaAllocator<T>&, const ArenaAllocator<U>&) noexcept {
    return true;
}
template <typename T, typename U>
bool operator!=(const ArenaAllocator<T>&, const ArenaAllocator<U>&) noexcept {
    return false;
}

template <typename T>
using ArenaVector = std::vector<T, ArenaAllocator<T>>;

}  // namespace gplvr
