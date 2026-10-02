#include "DataRebase.h"

#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// The bands, in reference-link (retail gplc67.exe) addresses. `first`/`last` are inclusive and
// span only bytes that an operand in one of the two known links actually names; the unnamed bytes
// between one band's last and the next band's first are gaps, and rebase() refuses them.
//
// kZeroBand reaches from the image base up to the lowest global that has ever moved. Its delta is
// derived from an anchor like any other and is expected to come out 0, which is the check that
// this band map is being applied to an image it describes at all.
enum Band { kZeroBand, kBandA, kBandB, kBandC, kBandCount };

struct BandRange {
    uintptr_t   first;
    uintptr_t   last;
    const char* name;
};

constexpr BandRange kBands[kBandCount] = {
    {0x00400000, 0x0054C5B7, "below"},
    {0x0054C5C0, 0x00586627, "A"},
    {0x00586630, 0x005B1203, "B"},
    {0x005B1208, 0x00971267, "C"},  // 0x00971267 is the last byte of `.data`
};

// The eight bytes that relocate rather than slide: 0x0054C5B8 and 0x0054C5BC in the reference
// link are 0x005B11F8 and 0x005B11FC in the plain one.
constexpr uintptr_t kMovedFirst = 0x0054C5B8;
constexpr uintptr_t kMovedLast = 0x0054C5BF;

// Each anchor is an instruction in vanilla `.text` carrying an imm32 that names `reference`. The
// opcode ahead of the operand is compared, so an anchor whose instruction has been patched
// declines rather than yielding a delta read out of someone else's bytes.
//
// Two anchors per band, naming different globals. One anchor can confirm a delta but cannot
// reveal that the band is cut somewhere this map does not know about; two that disagree can.
struct Anchor {
    uintptr_t     site;
    unsigned char opcode[2];
    unsigned      opcodeLength;
    unsigned      operandOffset;
    uintptr_t     reference;
    Band          band;
    const char*   what;
};

constexpr Anchor kAnchors[] = {
    // C7 05 <dest> <imm32>: mov gpl_brakingHelpBinding_22, offset off_536ED8. One verified opcode
    // yields a band B delta from the destination and the zero control from the immediate, so the
    // control is as well guarded as the band it sits next to.
    {0x0040E9D8, {0xC7, 0x05}, 2, 2, 0x005B11A8, kBandB, "a brakingHelp binding field"},
    {0x0040E9D8, {0xC7, 0x05}, 2, 6, 0x00536ED8, kZeroBand, "a vtable below the window"},

    {0x0040E9BF, {0xA0, 0x00}, 1, 1, 0x0054C5C0, kBandA, "gpl_optBrakingHelp"},
    {0x004E8F3D, {0x3B, 0x05}, 2, 2, 0x00582114, kBandA, "gpl_fgLastLatchedBgCount"},

    {0x0040E9B5, {0xB9, 0x00}, 1, 1, 0x005B11A8, kBandB, "a brakingHelp binding field"},
    {0x00406490, {0xB8, 0x00}, 1, 1, 0x005A1928, kBandB, "gpl_carsetDirName"},

    {0x004CABD9, {0x8B, 0x15}, 2, 2, 0x00870140, kBandC, "gpl_drawnMarkCount"},
    {0x00460B40, {0x8B, 0x15}, 2, 2, 0x006B7FD8, kBandC, "gpl_segmentArray"},
};

constexpr unsigned kAnchorCount = sizeof(kAnchors) / sizeof(kAnchors[0]);

// A delta larger than this is not a link difference. The two known links differ by 16 bytes; the
// bound is loose enough for a third that moved a whole object and tight enough that an operand
// misread as an address -- a float, a string, a count -- fails it.
constexpr long kDeltaLimit = 0x10000;

struct BandState {
    long delta = 0;
    bool resolved = false;
    bool conflicted = false;
};

ModuleImage g_image;
BandState   g_band[kBandCount];
bool        g_resolved = false;
bool        g_usable = false;
char        g_summary[96] = "unresolved";

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto start = static_cast<const uint8_t*>(info.BaseAddress);
    return static_cast<const uint8_t*>(address) + bytes <= start + info.RegionSize;
}

bool inImage(const ModuleImage& image, uintptr_t address) {
    auto p = reinterpret_cast<const uint8_t*>(address);
    return p >= image.base && p < image.base + image.size;
}

// Read one anchor's operand and return the delta it implies, or false when the anchor cannot
// speak for this image.
bool anchorDelta(const Anchor& anchor, long& delta) {
    auto site = reinterpret_cast<const uint8_t*>(anchor.site);
    size_t span = anchor.operandOffset + sizeof(uint32_t);
    if (!g_image.inText(site) || !g_image.inText(site + span - 1)) return false;
    if (!readable(site, span)) return false;
    if (std::memcmp(site, anchor.opcode, anchor.opcodeLength) != 0) return false;

    uint32_t operand;
    std::memcpy(&operand, site + anchor.operandOffset, sizeof(operand));
    long found = static_cast<long>(operand) - static_cast<long>(anchor.reference);
    if (found > kDeltaLimit || found < -kDeltaLimit) return false;

    // The operand has to name something in this image. A plausible-looking delta read out of an
    // instruction that merely starts with the same opcode fails here.
    if (!inImage(g_image, operand)) return false;

    delta = found;
    return true;
}

void describe() {
    int written = 0;
    for (int band = 0; band < kBandCount; ++band) {
        int room = static_cast<int>(sizeof(g_summary)) - written - 1;
        if (room <= 0) break;
        int n;
        if (g_band[band].resolved) {
            n = _snprintf_s(g_summary + written, room, _TRUNCATE, "%s%+ld", band ? "/" : "",
                            g_band[band].delta);
        } else {
            n = _snprintf_s(g_summary + written, room, _TRUNCATE, "%s%s", band ? "/" : "",
                            g_band[band].conflicted ? "conflict" : "refused");
        }
        if (n < 0) break;
        written += n;
    }
    g_summary[sizeof(g_summary) - 1] = '\0';
}

}  // namespace

void dataRebaseResolve() {
    if (g_resolved) return;
    g_resolved = true;

    if (!getModuleImage(GetModuleHandle(nullptr), g_image)) {
        logf("data rebase: the running image could not be walked, so no global can be placed and "
             "every feature that needs one declines");
        return;
    }

    unsigned spoke = 0;
    for (const Anchor& anchor : kAnchors) {
        long delta = 0;
        if (!anchorDelta(anchor, delta)) {
            logf("data rebase: the anchor at %08X (%s) does not hold the instruction it should, so "
                 "it says nothing about band %s",
                 static_cast<unsigned>(anchor.site), anchor.what, kBands[anchor.band].name);
            continue;
        }
        ++spoke;

        BandState& band = g_band[anchor.band];
        if (band.conflicted) continue;
        if (!band.resolved) {
            band.delta = delta;
            band.resolved = true;
            continue;
        }
        if (band.delta == delta) continue;

        logf("data rebase: band %s is contradicted -- the anchor at %08X (%s) puts it at %+ld "
             "while another put it at %+ld, so this image's data is cut somewhere the band map "
             "does not describe and the whole band is refused",
             kBands[anchor.band].name, static_cast<unsigned>(anchor.site), anchor.what, delta,
             band.delta);
        band.resolved = false;
        band.conflicted = true;
    }

    describe();

    // The control first. Every other delta is read against a picture of the image that this one
    // anchor is the only evidence for.
    if (!g_band[kZeroBand].resolved || g_band[kZeroBand].delta != 0) {
        logf("data rebase: REFUSING EVERY GLOBAL -- the control below the window did not come out "
             "at zero (below/A/B/C = %s, %u of %u anchors spoke). No known link moves anything "
             "there, so an image that disagrees is not one these addresses describe",
             g_summary, spoke, kAnchorCount);
        return;
    }

    g_usable = true;
    bool moved = g_band[kBandA].delta || g_band[kBandB].delta || g_band[kBandC].delta;
    logf("data rebase: deltas below/A/B/C = %s, from %u of %u anchors -- %s", g_summary, spoke,
         kAnchorCount,
         moved ? "src/'s addresses are moved onto this image before use"
               : "this image keeps its globals where src/ names them");
}

uintptr_t rebase(uintptr_t referenceVa) {
    if (!g_usable) return 0;
    if (referenceVa >= kMovedFirst && referenceVa <= kMovedLast) return 0;

    for (int band = 0; band < kBandCount; ++band) {
        if (referenceVa < kBands[band].first || referenceVa > kBands[band].last) continue;
        if (!g_band[band].resolved) return 0;
        auto moved = static_cast<uintptr_t>(static_cast<long>(referenceVa) + g_band[band].delta);
        return inImage(g_image, moved) ? moved : 0;
    }

    // Past the end of `.data`, or in a gap between two bands that neither link names.
    return referenceVa > kBands[kBandC].last && inImage(g_image, referenceVa) ? referenceVa : 0;
}

const char* rebaseSummary() { return g_summary; }

unsigned rebaseOperands(unsigned char* bytes, size_t length, uintptr_t from, uintptr_t to) {
    unsigned char want[4];
    unsigned char give[4];
    auto          fromValue = static_cast<uint32_t>(from);
    auto          toValue = static_cast<uint32_t>(to);
    std::memcpy(want, &fromValue, sizeof(want));
    std::memcpy(give, &toValue, sizeof(give));

    unsigned hits = 0;
    for (size_t i = 0; i + sizeof(want) <= length;) {
        if (std::memcmp(bytes + i, want, sizeof(want)) != 0) {
            ++i;
            continue;
        }
        std::memcpy(bytes + i, give, sizeof(give));
        ++hits;
        i += sizeof(want);
    }
    return hits;
}

}  // namespace gplvr
