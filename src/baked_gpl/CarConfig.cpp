#include "baked_gpl/CarConfig.h"

#include "baked_gpl/CarBake.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

constexpr unsigned kObjectRoot = 56;  // Obj3d+0x38, the root node a SUBOBJECT dispatches
constexpr int      kMaxDepth = 512;   // CarWalk's
constexpr int32_t  kMaxSelectorCases = 4096;
constexpr uint32_t kMaxGroup = 65536;
// The register scan's memo, by control node, and the learned gates: open addressing, linear
// probing, at most half full. The memo holds every control node of every model the walks asked
// about: 2020's cars18 have 2,900-28,600 a team (~120,000 for the ten), stock cars67 a few
// thousand. It starts at 2^17 entries and doubles before a scan whenever it is a quarter full, so
// one scan (at most one model's nodes) never fills it; past 2^20 a scan that fills it leaves the
// gate undecided (counted), never decided.
constexpr unsigned kScanMinBits = 17;
constexpr unsigned kScanMaxBits = 20;
constexpr unsigned kLearnBits = 13;
constexpr uint32_t kLearnSize = 1u << kLearnBits;

enum ScanState : uint8_t {
    kUnknown = 0,
    kBusy,  // on the scan's path: a cycle reads as no write
    kNo,
    kYes,
};

std::vector<uintptr_t> g_scanKeys;
std::vector<uint8_t>   g_scanStates;
unsigned               g_scanBits = 0;
uintptr_t              g_learned[kLearnSize];
uint32_t               g_generation = 0;
bool                   g_haveGeneration = false;
ConfigCounts           g_counts;

template <typename T>
T at(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

uint32_t word(uintptr_t node, unsigned index) { return at<uint32_t>(node + 4u * index); }

uint32_t slotOf(uintptr_t node, unsigned bits) {
    return (static_cast<uint32_t>(node) * 2654435761u) >> (32 - bits);
}

// A new car set names other nodes: both tables start again.
void follow() {
    uint32_t generation = carBakeGeneration();
    if (g_haveGeneration && generation == g_generation) return;
    g_haveGeneration = true;
    g_generation = generation;
    g_scanBits = kScanMinBits;
    g_scanKeys.assign(size_t{1} << g_scanBits, 0);
    g_scanStates.assign(size_t{1} << g_scanBits, kUnknown);
    std::memset(g_learned, 0, sizeof(g_learned));
    g_counts.scanned = 0;
    g_counts.writers = 0;
    g_counts.nodes = 0;
    g_counts.learned = 0;
    g_counts.tableKiB = 0;
    ++g_counts.generations;
}

// Doubles the memo while it is a quarter full or more, between scans only (a scan holds slot
// pointers).
void roomForScan() {
    while (g_scanBits < kScanMaxBits && g_counts.nodes >= (1u << g_scanBits) / 4) {
        std::vector<uintptr_t> keys(size_t{1} << (g_scanBits + 1), 0);
        std::vector<uint8_t>   states(keys.size(), kUnknown);
        uint32_t               mask = static_cast<uint32_t>(keys.size() - 1);
        for (size_t j = 0; j < g_scanKeys.size(); ++j) {
            if (!g_scanKeys[j]) continue;
            uint32_t i = slotOf(g_scanKeys[j], g_scanBits + 1);
            while (keys[i]) i = (i + 1) & mask;
            keys[i] = g_scanKeys[j];
            states[i] = g_scanStates[j] == kBusy ? kUnknown : g_scanStates[j];
        }
        g_scanKeys.swap(keys);
        g_scanStates.swap(states);
        ++g_scanBits;
    }
    g_counts.tableKiB = static_cast<uint32_t>(
        (g_scanKeys.size() * (sizeof(uintptr_t) + sizeof(uint8_t))) >> 10);
}

// The memo slot of a control node, made when new; null when the table is half full.
uint8_t* stateOf(uintptr_t node) {
    uint32_t mask = static_cast<uint32_t>(g_scanKeys.size() - 1);
    for (uint32_t i = slotOf(node, g_scanBits);; i = (i + 1) & mask) {
        if (g_scanKeys[i] == node) return &g_scanStates[i];
        if (g_scanKeys[i]) continue;
        if (g_counts.nodes >= g_scanKeys.size() / 2) return nullptr;
        g_scanKeys[i] = node;
        g_scanStates[i] = kUnknown;
        ++g_counts.nodes;
        return &g_scanStates[i];
    }
}

struct Scan {
    bool deep = false;
    bool full = false;
};

bool writes(uintptr_t node, int depth, Scan& scan);

bool anyOf(uintptr_t node, const unsigned* words, unsigned count, int depth, Scan& scan) {
    for (unsigned i = 0; i < count; ++i)
        if (writes(word(node, words[i]), depth + 1, scan)) return true;
    return false;
}

// The children CarWalk's visit takes, every selector case and both sides of every gate.
bool childrenWrite(uintptr_t node, uint8_t type, int depth, Scan& scan) {
    static const unsigned kOne[] = {1};
    static const unsigned kTwo[] = {2};
    static const unsigned kTwoThree[] = {2, 3};
    static const unsigned kTwoToFour[] = {2, 3, 4};
    static const unsigned kTwoToFive[] = {2, 3, 4, 5};
    static const unsigned kThree[] = {3};
    static const unsigned kEight[] = {8};
    static const unsigned kNine[] = {9};
    switch (type) {
    case 0x04: {  // GROUP
        uint32_t count = word(node, 1);
        if (count > kMaxGroup) {
            scan.deep = true;
            return true;
        }
        for (uint32_t i = 0; i < count; ++i)
            if (writes(word(node, 2 + i), depth + 1, scan)) return true;
        return false;
    }
    case 0x05:  // MATERIAL
    case 0x0C:  // ALTSET
        return anyOf(node, kOne, 1, depth, scan);
    case 0x06: return anyOf(node, kTwo, 1, depth, scan);
    case 0x07:
    case 0x0B: return anyOf(node, kTwoThree, 2, depth, scan);
    case 0x08: return anyOf(node, kTwoToFive, 4, depth, scan);
    case 0x09:
    case 0x0A: return anyOf(node, kTwoToFour, 3, depth, scan);
    case 0x0D:
    case 0x13:
    case 0x16: return anyOf(node, kEight, 1, depth, scan);
    case 0x0E: return writes(at<uint32_t>(word(node, 1) + kObjectRoot), depth + 1, scan);
    case 0x0F: {  // SORTGROUP: the static child, then the elements
        if (writes(word(node, 10), depth + 1, scan)) return true;
        uint32_t count = word(node, 15);
        if (count > kMaxGroup) {
            scan.deep = true;
            return true;
        }
        for (uint32_t i = 0; i < count; ++i)
            if (writes(word(node, 19 + 4 * i), depth + 1, scan)) return true;
        return false;
    }
    case 0x11: {  // SELECTOR: every case
        auto count = static_cast<int32_t>(word(node, 4));
        if (count <= 0 || count > kMaxSelectorCases) return false;
        for (int32_t i = 0; i < count; ++i)
            if (writes(word(node, 6 + 2 * static_cast<unsigned>(i)), depth + 1, scan)) return true;
        return false;
    }
    case 0x18: return anyOf(node, kThree, 1, depth, scan);
    case 0x19: return anyOf(node, kNine, 1, depth, scan);
    default: return false;
    }
}

bool writes(uintptr_t node, int depth, Scan& scan) {
    if (depth > kMaxDepth) {
        scan.deep = true;
        return true;
    }
    auto typeWord = at<int32_t>(node);
    if (typeWord < 0) return false;  // gpl_nullNode and every skipped node
    auto type = static_cast<uint8_t>(typeWord);
    switch (type) {
    case 0x14: return true;  // SETREG
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x11: case 0x13: case 0x16: case 0x18:
    case 0x19:
        break;
    default: return false;  // leaves, lines, PRETRANSFORM, the null handlers
    }
    uint8_t* state = stateOf(node);
    if (!state) {
        scan.full = true;
        return true;
    }
    if (*state == kYes) return true;
    if (*state == kNo || *state == kBusy) return false;
    *state = kBusy;
    bool result = childrenWrite(node, type, depth, scan);
    // A scan that could not finish (`deep`, `full`) answers nothing: its path stays unknown.
    *state = scan.deep || scan.full ? kUnknown : result ? kYes : kNo;
    return result;
}

// A fault leaves the scan's path marked busy; carConfigDecides clears it.
bool scanGuarded(uintptr_t node, Scan& scan, bool& faulted) {
    __try {
        return writes(node, 0, scan);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return true;
    }
}

bool learned(uintptr_t node) {
    for (uint32_t i = slotOf(node, kLearnBits);; i = (i + 1) & (kLearnSize - 1)) {
        if (g_learned[i] == node) return true;
        if (!g_learned[i]) return false;
    }
}

}  // namespace

bool carConfigDecides(uintptr_t node) {
    follow();
    if (!node) return false;
    if (learned(node)) return true;
    uint8_t* state = stateOf(node);
    if (state && (*state == kYes || *state == kNo)) return *state == kYes;
    // Unanswered (new, or an earlier scan could not finish): scan, with room for a whole model.
    roomForScan();
    Scan scan;
    bool faulted = false;
    bool result = scanGuarded(node, scan, faulted);
    if (faulted) {
        for (uint8_t& s : g_scanStates)
            if (s == kBusy) s = kUnknown;
    }
    // A scan that could not answer leaves the gate to the both-ways walk of section 4.23; a
    // hazard it causes can still name it.
    if (faulted || scan.deep || scan.full) {
        if (faulted) ++g_counts.scanFaults;
        if (scan.deep) ++g_counts.scanDeep;
        if (scan.full) ++g_counts.scanFull;
        ++g_counts.unanswered;
        return false;
    }
    ++g_counts.scanned;
    if (result) ++g_counts.writers;
    return result;
}

int carConfigLearn(uintptr_t node) {
    follow();
    if (!node) return -1;
    for (uint32_t i = slotOf(node, kLearnBits);; i = (i + 1) & (kLearnSize - 1)) {
        if (g_learned[i] == node) return 0;
        if (g_learned[i]) continue;
        if (g_counts.learned >= kLearnSize / 2) {
            ++g_counts.learnFull;
            return -1;
        }
        g_learned[i] = node;
        ++g_counts.learned;
        ++g_counts.learnedNow;
        return 1;
    }
}

ConfigCounts carConfigCounts() { return g_counts; }

void carConfigSecond() {
    g_counts.learnedNow = 0;
    g_counts.scanFaults = 0;
    g_counts.scanDeep = 0;
    g_counts.scanFull = 0;
    g_counts.unanswered = 0;
    g_counts.learnFull = 0;
}

}  // namespace gplvr
