#include "SimRing.h"

#include "Hooks.h"
#include "Log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe; reports 44, 45 and 72) ----------------------------------------

constexpr uintptr_t kLiveCars = 0x006BF080;  // gpl_carLiveGet(0)
constexpr uintptr_t kReplayCars = 0x00790848;
constexpr uintptr_t kCarSize = 1544;
constexpr int       kCarsPerArray = 20;
constexpr int       kCarSlots = 2 * kCarsPerArray;

// The sim thread's loop body ends here: gpl_bgTask 0x452AD0 calls bgSleep 0x4E8300 and blocks
// on the task gate until the timer grants the next tick (report 72 section 4.2).
constexpr uintptr_t     kBgSleepSite = 0x00452B74;
constexpr unsigned char kBgSleepStock[5] = {0xE8, 0x87, 0x57, 0x09, 0x00};

// gpl_simFrameSetup 0x48A9A0 latches the two loops over the cars and then the viewed car.
struct StockCall {
    uintptr_t     site;
    unsigned char bytes[5];
};
constexpr StockCall kCarLatchCalls[3] = {{0x0048AE95, {0xE8, 0x96, 0x21, 0xFE, 0xFF}},
                                         {0x0048AF7A, {0xE8, 0xB1, 0x20, 0xFE, 0xFF}},
                                         {0x0048B024, {0xE8, 0x07, 0x20, 0xFE, 0xFF}}};

// The two lookups gpl_carLatchRenderRecord makes from car[196]: the driver data and the model
// handle. Both __cdecl, both fg-side.
constexpr uintptr_t kDriverDataOf = 0x004A5A30;
constexpr uintptr_t kModelHandleOf = 0x0046F050;

// gpl_taskBgGrantsDeferred: the timer thread increments it whenever a tick grant finds
// gpl_taskFgBgLock taken, which is the render thread holding the latch bracket.
constexpr uintptr_t kGrantsDeferred = 0x00581ED0;

// -- the slot ----------------------------------------------------------------------------------

// The physics block car[195], in the order gpl_carLatchRenderRecord reads it: position,
// distance along the lap (+60), distance across (+64), the segment hint (+80) and the 3x3
// matrix (+84, 36 bytes).
constexpr int kPhysWords = 15;

constexpr int kCarFirstWord = 271;  // car[271..317] inclusive
constexpr int kCarWordCount = 47;

struct CarSource {
    uint32_t present;  // 0 when the car had no readable physics block this tick
    uint32_t driverIndex;
    uint32_t word40;
    uint32_t words[kCarWordCount];
    uint32_t phys[kPhysWords];
};

// The record gpl_carLatchRenderRecord writes, car[320..385].
constexpr int kRecordFirst = 320;
constexpr int kRecordWords = 66;

struct Slot {
    std::atomic<unsigned> sequence{0};  // even: stable. odd: being written
    unsigned              publish = 0;
    int                   bgTick = 0;
    long long             qpc = 0;
    CarSource             cars[kCarSlots] = {};
};

constexpr int kSlots = 4;

using BgSleepFn = void(__cdecl*)();
using CarLatchFn = void*(__cdecl*)(void* car);
using LookupFn = int(__cdecl*)(int);
using CountFn = int(__cdecl*)();

struct State {
    bool installed = false;

    Slot                slots[kSlots];
    std::atomic<int>    newest{-1};
    std::atomic<int>    pinned[2] = {};
    std::atomic<unsigned> publishes{0};

    BgSleepFn  bgSleep = nullptr;
    CarLatchFn carLatch = nullptr;
    LookupFn   driverDataOf = nullptr;
    LookupFn   modelHandleOf = nullptr;
    CountFn    getBGCount = nullptr;

    // The window since the last report.
    unsigned windowLatches = 0;    // car latches the render thread ran
    unsigned windowVerified = 0;   // of those, compared against a slot of the same tick
    unsigned windowSkipped = 0;    // slot was from another tick, or the car was not in it
    unsigned windowMismatchCars = 0;
    unsigned windowMismatchWords = 0;
    unsigned windowMaterialised = 0;
    unsigned windowRepins = 0;     // the slot changed under the pin and was taken again
    int      grantsAtReport = 0;

    // Totals.
    unsigned totalPublishes = 0;
    unsigned totalMismatchCars = 0;
    unsigned totalGrantsDeferred = 0;
    int      firstMismatchWord = -1;
    int      firstMismatchCar = -1;

    long long frequency = 0;

    // The physics block a car slot had last tick, so the publish costs one VirtualQuery per
    // car only when the pointer changes rather than 5,760 a second.
    const uint32_t* lastPhysics[kCarSlots] = {};
    bool            lastPhysicsOk[kCarSlots] = {};
};

State g_state;

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto start = reinterpret_cast<uintptr_t>(address);
    auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + bytes <= end;
}

long long now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

uintptr_t callTarget(uintptr_t site) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const uint8_t*>(site) + 1, sizeof(rel));
    return site + 5 + rel;
}

// The sim thread runs the bgSleep call site 144 times a second while this is installed, so the
// displacement is written as ONE 32-bit store rather than through memcpy: the four bytes lie
// inside a single cache line, which is what makes the store indivisible to another core running
// the instruction. The page is opened and closed around it exactly as patchBytes does.
bool redirect(uintptr_t site, uintptr_t to) {
    auto    displacement = reinterpret_cast<volatile int32_t*>(site + 1);
    int32_t rel = static_cast<int32_t>(to - (site + 5));
    DWORD   previous = 0;
    if (!VirtualProtect(const_cast<int32_t*>(displacement), sizeof(rel), PAGE_EXECUTE_READWRITE,
                        &previous))
        return false;
    *displacement = rel;
    DWORD ignored = 0;
    VirtualProtect(const_cast<int32_t*>(displacement), sizeof(rel), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), const_cast<int32_t*>(displacement), sizeof(rel));
    return true;
}

uint32_t* carWords(int slot) {
    uintptr_t array = slot < kCarsPerArray ? kLiveCars : kReplayCars;
    return reinterpret_cast<uint32_t*>(array + (slot % kCarsPerArray) * kCarSize);
}

int carSlotOf(uintptr_t car) {
    uintptr_t span = kCarSize * kCarsPerArray;
    if (car >= kLiveCars && car < kLiveCars + span && (car - kLiveCars) % kCarSize == 0)
        return static_cast<int>((car - kLiveCars) / kCarSize);
    if (car >= kReplayCars && car < kReplayCars + span && (car - kReplayCars) % kCarSize == 0)
        return kCarsPerArray + static_cast<int>((car - kReplayCars) / kCarSize);
    return -1;
}

// -- publish, on the sim thread ------------------------------------------------------------

void captureCar(CarSource& out, const uint32_t* car, int slot) {
    State& s = g_state;
    out.present = 0;
    auto physics = reinterpret_cast<const uint32_t*>(car[195]);
    if (!physics) return;
    if (physics != s.lastPhysics[slot]) {
        s.lastPhysics[slot] = physics;
        s.lastPhysicsOk[slot] = readable(physics, 120);
    }
    if (!s.lastPhysicsOk[slot]) return;

    out.driverIndex = car[196];
    out.word40 = car[40];
    std::memcpy(out.words, car + kCarFirstWord, sizeof(out.words));
    out.phys[0] = physics[0];
    out.phys[1] = physics[1];
    out.phys[2] = physics[2];
    out.phys[3] = physics[15];  // +60, distance along the lap
    out.phys[4] = physics[16];  // +64, distance across
    out.phys[5] = physics[20];  // +80, the segment hint
    std::memcpy(out.phys + 6, physics + 21, 36);  // +84, the 3x3 matrix
    out.present = 1;
}

// The lowest slot that is neither the newest nor pinned. With four slots, two pins and one
// newest there is always one, so the sim never blocks here.
int freeSlot() {
    State& s = g_state;
    int newest = s.newest.load(std::memory_order_acquire);
    int pin0 = s.pinned[0].load(std::memory_order_acquire);
    int pin1 = s.pinned[1].load(std::memory_order_acquire);
    for (int i = 0; i < kSlots; ++i) {
        if (i == newest || i == pin0 || i == pin1) continue;
        return i;
    }
    return (newest + 1) % kSlots;
}

void publishTick() {
    State& s = g_state;
    int    index = freeSlot();
    Slot&  slot = s.slots[index];

    slot.sequence.fetch_add(1, std::memory_order_release);  // odd: being written
    for (int i = 0; i < kCarSlots; ++i) captureCar(slot.cars[i], carWords(i), i);
    slot.bgTick = s.getBGCount ? s.getBGCount() : 0;
    slot.qpc = now();
    slot.publish = s.publishes.fetch_add(1, std::memory_order_relaxed) + 1;
    slot.sequence.fetch_add(1, std::memory_order_release);  // even: stable

    s.newest.store(index, std::memory_order_release);
}

void __cdecl bgSleepHook() {
    State& s = g_state;
    if (g_options.simRing > 0) publishTick();
    s.bgSleep();
}

// -- consume, on the render thread ----------------------------------------------------------

// The newest complete slot, pinned. Returns -1 when nothing has been published, or when the
// slot changed under the pin more times than a bounded retry allows -- publishes happen at most
// once a sim tick, so one retry is already generous.
int pinNewest(unsigned& sequence) {
    State& s = g_state;
    for (int attempt = 0; attempt < 4; ++attempt) {
        int index = s.newest.load(std::memory_order_acquire);
        if (index < 0) return -1;
        s.pinned[0].store(index, std::memory_order_release);
        unsigned before = s.slots[index].sequence.load(std::memory_order_acquire);
        if ((before & 1u) == 0 && s.newest.load(std::memory_order_acquire) == index) {
            sequence = before;
            return index;
        }
        ++s.windowRepins;
    }
    s.pinned[0].store(-1, std::memory_order_release);
    return -1;
}

void unpin() { g_state.pinned[0].store(-1, std::memory_order_release); }

// gpl_carLatchRenderRecord 0x46D030, written out of the slot. `r` is the 66-word record --
// car[320..385] itself, or a scratch copy for the verify -- and `car` its owner; the two lookups
// and the back-pointer stay the render thread's.
void materialise(uint32_t* car, const CarSource& source, uint32_t* r) {
    State& s = g_state;
    const uint32_t* w = source.words;  // w[k] is car[271 + k]
    auto word = [&](int index) { return w[index - kCarFirstWord]; };

    r[0] = 0;                                        // [320]
    r[1] = reinterpret_cast<uint32_t>(car);          // [321]
    r[2] = source.phys[3];                           // [322]
    r[3] = source.phys[0];                           // [323]
    r[4] = source.phys[1];                           // [324]
    r[5] = source.phys[2];                           // [325]
    r[6] = 0x40400000u;                              // [326] 3.0f
    r[8] = source.phys[4];                           // [328]
    r[9] = source.phys[5];                           // [329]
    r[10] = static_cast<uint32_t>(s.driverDataOf(static_cast<int>(source.driverIndex)));
    r[12] = static_cast<uint32_t>(s.modelHandleOf(static_cast<int>(source.driverIndex)));
    std::memcpy(r + 13, source.phys + 6, 36);        // [333..341]
    r[22] = (word(275) >> 3) & 3;                    // [342]
    r[23] = source.word40;                           // [343]
    r[24] = word(312);                               // [344]
    r[25] = word(313);
    r[26] = word(314);
    r[27] = word(311);
    r[28] = word(315);
    r[29] = word(316);
    r[30] = word(317);
    r[31] = word(309);
    r[32] = word(310);
    r[33] = word(287);                               // [353]
    r[34] = word(288);
    r[35] = word(289);
    for (int k = 0; k < 4; ++k) {
        r[36 + k] = word(297 + k);                   // [356..359]
        r[40 + k] = word(301 + k);                   // [360..363]
        r[44 + k] = word(305 + k);                   // [364..367]
    }
    r[48] = word(296);                               // [368]
    r[49] = word(290);
    r[50] = word(291);
    r[51] = word(292);
    r[52] = word(293);
    r[53] = word(294);
    r[54] = word(295);
    r[55] = word(277);                               // [375]
    r[56] = word(281);
    r[57] = word(282);
    r[58] = word(283);
    r[59] = word(284);
    r[60] = word(285);
    r[61] = word(286);
    r[62] = word(271);                               // [382..385]
    r[63] = word(272);
    r[64] = word(273);
    r[65] = word(274);
}

// The words of the record this file does not write from the slot: the back-pointer [321], the
// two lookups [330] and [332], and [327] and [331], which gpl_carLatchRenderRecord leaves to
// its caller.
bool comparedWord(int index) {
    switch (index) {
        case 1:
        case 7:
        case 10:
        case 11:
        case 12: return false;
        default: return true;
    }
}

// What the slot would have written, against what GPL's own latch just wrote. Only meaningful
// while the two describe the same tick: the render thread holds gpl_taskFgBgLock here, so the
// sim is blocked at bgSleep and its last publish is the state GPL has just read -- unless a
// grant slipped in before the lock, which the tick numbers catch.
void verify(uint32_t* car, const CarSource& source, unsigned publish) {
    State&   s = g_state;
    uint32_t expected[kRecordWords];
    const uint32_t* written = car + kRecordFirst;

    materialise(car, source, expected);

    int differing = 0;
    for (int i = 0; i < kRecordWords; ++i) {
        if (!comparedWord(i) || expected[i] == written[i]) continue;
        ++differing;
        if (s.firstMismatchWord < 0) {
            s.firstMismatchWord = kRecordFirst + i;
            s.firstMismatchCar = carSlotOf(reinterpret_cast<uintptr_t>(car));
            logf("ring: car[%d] differs -- the slot says %08X, GPL's latch wrote %08X (car slot "
                 "%d, publish %u). Every later mismatch is counted, not logged.",
                 kRecordFirst + i, expected[i], written[i], s.firstMismatchCar, publish);
        }
    }
    if (differing) {
        ++s.windowMismatchCars;
        ++s.totalMismatchCars;
        s.windowMismatchWords += static_cast<unsigned>(differing);
    }
}

void* __cdecl carLatchHook(void* car) {
    State& s = g_state;
    int    mode = g_options.simRing;
    int    slot = carSlotOf(reinterpret_cast<uintptr_t>(car));
    ++s.windowLatches;

    if (mode <= 0 || slot < 0) return s.carLatch(car);

    unsigned sequence = 0;
    int      index = pinNewest(sequence);
    if (index < 0) {
        ++s.windowSkipped;
        return s.carLatch(car);
    }

    const Slot&      published = s.slots[index];
    const CarSource& source = published.cars[slot];
    bool             sameTick = s.getBGCount && published.bgTick == s.getBGCount();
    bool             usable = source.present != 0 && sameTick;

    if (mode >= 2 && usable) {
        materialise(reinterpret_cast<uint32_t*>(car), source,
                    reinterpret_cast<uint32_t*>(car) + kRecordFirst);
        ++s.windowMaterialised;
        // The pin is only good while the sequence has not moved; a slot overwritten under it
        // means the words above are a mix of two ticks, so the live latch puts it right.
        if (published.sequence.load(std::memory_order_acquire) != sequence) {
            unpin();
            ++s.windowRepins;
            return s.carLatch(car);
        }
        unpin();
        return reinterpret_cast<uint32_t*>(car) + kCarFirstWord;
    }

    void* result = s.carLatch(car);
    if (usable && published.sequence.load(std::memory_order_acquire) == sequence) {
        verify(reinterpret_cast<uint32_t*>(car), source, published.publish);
        ++s.windowVerified;
    } else {
        ++s.windowSkipped;
    }
    unpin();
    return result;
}

int deferredGrants() {
    if (!readable(reinterpret_cast<const void*>(kGrantsDeferred), sizeof(int))) return 0;
    return *reinterpret_cast<volatile int*>(kGrantsDeferred);
}

}  // namespace

void simRingInstall() {
    State& s = g_state;
    if (s.installed) return;

    HMODULE exe = GetModuleHandleA(nullptr);
    s.getBGCount = exe ? reinterpret_cast<CountFn>(GetProcAddress(exe, "?getBGCount@@YAHXZ"))
                       : nullptr;
    if (!s.getBGCount) {
        logf("ring: the exe does not export getBGCount -- no sim ring");
        return;
    }

    auto bytes = reinterpret_cast<const uint8_t*>(kBgSleepSite);
    if (std::memcmp(bytes, kBgSleepStock, sizeof(kBgSleepStock)) != 0) {
        logf("ring: %08X does not hold the stock `call bgSleep` (%02X %02X %02X %02X %02X) -- no "
             "sim ring",
             static_cast<unsigned>(kBgSleepSite), bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4]);
        return;
    }
    for (const StockCall& call : kCarLatchCalls) {
        auto site = reinterpret_cast<const uint8_t*>(call.site);
        if (std::memcmp(site, call.bytes, sizeof(call.bytes)) != 0) {
            logf("ring: %08X does not hold its stock car latch call (%02X %02X %02X %02X %02X) "
                 "-- no sim ring",
                 static_cast<unsigned>(call.site), site[0], site[1], site[2], site[3], site[4]);
            return;
        }
    }

    s.bgSleep = reinterpret_cast<BgSleepFn>(callTarget(kBgSleepSite));
    s.carLatch = reinterpret_cast<CarLatchFn>(callTarget(kCarLatchCalls[0].site));
    s.driverDataOf = reinterpret_cast<LookupFn>(kDriverDataOf);
    s.modelHandleOf = reinterpret_cast<LookupFn>(kModelHandleOf);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    s.frequency = frequency.QuadPart;
    for (std::atomic<int>& pin : s.pinned) pin.store(-1, std::memory_order_relaxed);

    if (!redirect(kBgSleepSite, reinterpret_cast<uintptr_t>(&bgSleepHook))) {
        logf("ring: could not redirect the sim thread's bgSleep call at %08X -- no sim ring",
             static_cast<unsigned>(kBgSleepSite));
        return;
    }
    for (const StockCall& call : kCarLatchCalls) {
        if (redirect(call.site, reinterpret_cast<uintptr_t>(&carLatchHook))) continue;
        // Put the publish back: a ring nothing reads is a copy for nothing.
        int32_t stock = 0;
        std::memcpy(&stock, kBgSleepStock + 1, sizeof(stock));
        redirect(kBgSleepSite, kBgSleepSite + 5 + static_cast<uintptr_t>(stock));
        logf("ring: could not redirect the car latch call at %08X -- no sim ring",
             static_cast<unsigned>(call.site));
        return;
    }

    s.installed = true;
    s.grantsAtReport = deferredGrants();
    logf("ring: installed -- the sim thread publishes at the bgSleep call %08X into %d slots of "
         "%zu bytes (%zu KB in all), the render thread reads them at the car latch calls "
         "%08X/%08X/%08X. simRing=%d (%s)",
         static_cast<unsigned>(kBgSleepSite), kSlots, sizeof(Slot),
         (sizeof(Slot) * kSlots + 1023) / 1024, static_cast<unsigned>(kCarLatchCalls[0].site),
         static_cast<unsigned>(kCarLatchCalls[1].site),
         static_cast<unsigned>(kCarLatchCalls[2].site), g_options.simRing,
         g_options.simRing >= 2 ? "the car records are written from the ring"
         : g_options.simRing == 1
             ? "the ring is verified against GPL's own latch; nothing drawn changes"
             : "publishing is off until simRing is set");
}

bool simRingActive() { return g_state.installed && g_options.simRing > 0; }

uintptr_t simRingCarLatchHookAddress() {
    return g_state.installed ? reinterpret_cast<uintptr_t>(&carLatchHook) : 0;
}

bool simRingFormat(char* out, size_t size, unsigned frames) {
    State& s = g_state;
    if (!s.installed) return false;

    unsigned publishes = s.publishes.load(std::memory_order_relaxed);
    unsigned window = publishes - s.totalPublishes;
    s.totalPublishes = publishes;

    int grants = deferredGrants();
    int deferred = grants - s.grantsAtReport;
    s.grantsAtReport = grants;
    if (deferred > 0) s.totalGrantsDeferred += static_cast<unsigned>(deferred);

    if (!window && !s.windowLatches) return false;

    std::snprintf(out, size,
                  "ring: %u ticks published, %.1f a frame | car latches %u: %u verified, %u "
                  "materialised, %u skipped (another tick or no car) | %u cars differed, %u "
                  "words | %u re-pins | GPL deferred %d tick grants while the latch held the "
                  "lock",
                  window, frames ? static_cast<double>(window) / frames : 0.0, s.windowLatches,
                  s.windowVerified, s.windowMaterialised, s.windowSkipped, s.windowMismatchCars,
                  s.windowMismatchWords, s.windowRepins, deferred);

    s.windowLatches = 0;
    s.windowVerified = 0;
    s.windowSkipped = 0;
    s.windowMismatchCars = 0;
    s.windowMismatchWords = 0;
    s.windowMaterialised = 0;
    s.windowRepins = 0;
    return true;
}

void simRingLogSummary() {
    State& s = g_state;
    if (!s.installed) return;
    if (s.firstMismatchWord >= 0) {
        logf("ring: %u ticks published in the session; %u cars differed from GPL's latch, the "
             "first at car[%d] of car slot %d; GPL deferred %u tick grants in all",
             s.publishes.load(std::memory_order_relaxed), s.totalMismatchCars,
             s.firstMismatchWord, s.firstMismatchCar, s.totalGrantsDeferred);
        return;
    }
    logf("ring: %u ticks published in the session, no car ever differed from GPL's latch; GPL "
         "deferred %u tick grants in all",
         s.publishes.load(std::memory_order_relaxed), s.totalGrantsDeferred);
}

}  // namespace gplvr
