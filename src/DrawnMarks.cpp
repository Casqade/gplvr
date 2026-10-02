#include "DrawnMarks.h"

#include "DataRebase.h"
#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// Reference-link addresses, which DataRebase maps onto the running image; g_count and
// g_stockList below hold the result. Both are in the band above 0x005B1208, where the plain link
// of 1.2.0.2 keeps them 8 lower. They also appear as imm32 operands inside the two instruction
// templates, which is why the templates are built rather than compared as written.
constexpr uintptr_t kRetailCount = 0x00870140;  // gpl_drawnMarkCount
constexpr uintptr_t kRetailList = 0x00870144;   // gpl_drawnMarkList, up to gpl_backgroundTable's block
constexpr uint32_t  kStockSlots = 2048;

// A node is listed at most once between clears, because a marked node is skipped, so the count
// is bounded by the distinct object-list nodes one pass draws. Report 53 saw 2073 in the pass
// that crashed and a process-wide high-water mark of 775 in another run. 32768 slots is 16x the
// crash and costs 128 kB.
constexpr uint32_t kSlots = 32768;

// gpl_nodeDrawAndMark after the draw has set the mark, esi = the node:
//
//   0x4CABD9  8B 15 40 01 87 00     mov  edx, [gpl_drawnMarkCount]
//   0x4CABDF  89 34 95 44 01 87 00  mov  [gpl_drawnMarkList+edx*4], esi
//   0x4CABE6  A1 40 01 87 00        mov  eax, [gpl_drawnMarkCount]
//   0x4CABEB  40                    inc  eax
//   0x4CABEC  A3 40 01 87 00        mov  [gpl_drawnMarkCount], eax
//   0x4CABF1  5E                    pop  esi       where the thunk resumes
//   0x4CABF2  C3                    retn
//
// The only branch into the function body targets 0x4CABF1, so the first six bytes become
// jmp rel32 + NOP and the rest is left in place, unreachable.
//
// The three site addresses are code and are the same in both links; the imm32s inside the
// bytes here and in kClearStock are the two globals and are not, so stockBytes() rewrites them
// for the running image before the comparison.
constexpr uintptr_t     kAppendSite = 0x004CABD9;
constexpr uintptr_t     kAppendResume = 0x004CABF1;
constexpr unsigned char kAppendStock[26] = {
    0x8B, 0x15, 0x40, 0x01, 0x87, 0x00, 0x89, 0x34, 0x95, 0x44, 0x01, 0x87, 0x00,
    0xA1, 0x40, 0x01, 0x87, 0x00, 0x40, 0xA3, 0x40, 0x01, 0x87, 0x00, 0x5E, 0xC3};

// gpl_renderClearDrawnMarks, all of it. Called from gpl_sceneRender at 0x487A78 (after the
// mirror walk) and 0x487FBC (end of the pass), which do not read its eax. Its list references
// are the imm32s at 0x4CAFD7 and 0x4CAFDF; the jmp at its entry makes both unreachable, so they
// are verified here and not rewritten.
constexpr uintptr_t     kClearSite = 0x004CAFD0;
constexpr unsigned char kClearStock[63] = {
    0x8B, 0x0D, 0x40, 0x01, 0x87, 0x00, 0xB8, 0x44, 0x01, 0x87, 0x00, 0x56, 0x8D, 0x14, 0x8D, 0x44,
    0x01, 0x87, 0x00, 0x3B, 0xD0, 0x74, 0x13, 0x8B, 0x08, 0x83, 0xC0, 0x04, 0x8B, 0x31, 0x81, 0xE6,
    0xFF, 0xFF, 0xFF, 0x7F, 0x3B, 0xC2, 0x89, 0x31, 0x75, 0xED, 0xA1, 0x40, 0x01, 0x87, 0x00, 0x85,
    0xC0, 0x74, 0x0A, 0xC7, 0x05, 0x40, 0x01, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5E, 0xC3};

uint32_t g_buffer[kSlots];

// The two globals as the running image keeps them, resolved in drawnMarksInstall(). Zero until
// then, and the install is what refuses when they cannot be resolved, so nothing below ever
// sees a half-mapped pair.
uintptr_t g_count = 0;
uintptr_t g_stockList = 0;

// The list the thunk appends to and the clear walks. Both start on the stock list, and the clear
// moves them to g_buffer once the append thunk is in, at a moment the list is empty. Appends and
// clears both run on fg, so the two never see a half-made move.
uintptr_t     g_list = 0;
uint32_t      g_capacity = kStockSlots;
volatile LONG g_appendInstalled = 0;

uintptr_t g_countSlot = 0;
void*     g_appendResume = reinterpret_cast<void*>(kAppendResume);

// Session totals, written on fg. The reporter, also on fg, takes window deltas.
uint32_t g_appendsSinceClear = 0;  // every attempt, overflows included
uint32_t g_overflows = 0;
uint32_t g_clears = 0;
uint32_t g_sessionPeak = 0;

// Window values, reset by the reporter.
uint32_t g_windowPeak = 0;
uint32_t g_windowMaxAppends = 0;
uint32_t g_windowClearsStart = 0;
uint32_t g_windowOverflowsStart = 0;

bool g_installed = false;

// Entered by a jmp from 0x4CABD9 with esi = the node, its type word already marked. eax, ecx and
// edx are free: the stock code overwrites eax and edx before the retn, and ecx held only the
// marked type word. Leaves eax = the count, as the stock append does.
//
// When the list is full the node's mark is taken back off, so the clear does not need to know
// about it; at worst it draws twice in the pass.
__declspec(naked) void appendThunk() {
    __asm {
        inc  dword ptr [g_appendsSinceClear]
        mov  ecx, dword ptr [g_countSlot]
        mov  edx, [ecx]
        cmp  edx, dword ptr [g_capacity]
        jae  full
        mov  eax, dword ptr [g_list]
        mov  [eax+edx*4], esi
        inc  edx
        mov  [ecx], edx
        mov  eax, edx
        jmp  dword ptr [g_appendResume]
    full:
        and  dword ptr [esi], 7FFFFFFFh
        inc  dword ptr [g_overflows]
        mov  eax, edx
        jmp  dword ptr [g_appendResume]
    }
}

// Replaces gpl_renderClearDrawnMarks through a jmp at its entry, so it returns to the caller of
// 0x4CAFD0 directly.
int __cdecl clearDrawnMarks() {
    auto count = reinterpret_cast<LONG*>(g_count);
    int  result = *count;
    auto held = static_cast<uint32_t>(result);
    // Only stock appends from before the install can have gone past the capacity.
    uint32_t walk = held < g_capacity ? held : g_capacity;
    auto     list = reinterpret_cast<uint32_t**>(g_list);
    for (uint32_t i = 0; i < walk; ++i) *list[i] &= 0x7FFFFFFFu;
    *count = 0;

    ++g_clears;
    if (held > g_windowPeak) g_windowPeak = held;
    if (held > g_sessionPeak) g_sessionPeak = held;
    if (g_appendsSinceClear > g_windowMaxAppends) g_windowMaxAppends = g_appendsSinceClear;
    g_appendsSinceClear = 0;

    if (g_list == g_stockList && g_appendInstalled) {
        g_list = reinterpret_cast<uintptr_t>(g_buffer);
        g_capacity = kSlots;
    }
    return result;
}

// One instruction template with its global operands moved onto the running image. Each imm32 is
// found by VALUE and the substitutions are counted, so a template whose operands do not add up --
// an edit that dropped one, or an address that appears a different number of times than expected
// -- is a refusal rather than a pattern that still names the reference link and matches nothing
// else.
//
// Where the running image keeps its globals exactly where src/ names them, every substitution
// writes what was already there; the tally still has to come out, which is what makes the check
// worth running on any image.
bool stockBytes(unsigned char* out, const unsigned char* reference, size_t length,
                unsigned countOperands, unsigned listOperands) {
    std::memcpy(out, reference, length);
    return rebaseOperands(out, length, kRetailCount, g_count) == countOperands &&
           rebaseOperands(out, length, kRetailList, g_stockList) == listOperands;
}

bool writeJump(uintptr_t site, void* target, size_t length) {
    unsigned char patch[6] = {0xE9, 0x00, 0x00, 0x00, 0x00, 0x90};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    return patchBytes(reinterpret_cast<void*>(site), patch, length);
}

}  // namespace

void drawnMarksInstall() {
    // The two globals first. The count is a read-modify-write and the list an indexed store, so
    // an address this image cannot be shown to keep them at would scribble over whatever it does
    // keep there, and the stock-byte check below cannot tell: the code is the same in every link
    // and matches on all of them.
    g_count = rebase(kRetailCount);
    g_stockList = rebase(kRetailList);
    if (!g_count || !g_stockList) {
        logf("drawn marks: not patching -- gpl_drawnMarkCount %08X and gpl_drawnMarkList %08X "
             "cannot be placed in this image (deltas %s); gpl_drawnMarkList stays 2048 unbounded "
             "slots",
             static_cast<unsigned>(kRetailCount), static_cast<unsigned>(kRetailList),
             rebaseSummary());
        return;
    }
    g_countSlot = g_count;
    g_list = g_stockList;

    unsigned char appendPattern[sizeof(kAppendStock)];
    unsigned char clearPattern[sizeof(kClearStock)];
    if (!stockBytes(appendPattern, kAppendStock, sizeof(appendPattern), 3, 1) ||
        !stockBytes(clearPattern, kClearStock, sizeof(clearPattern), 3, 2)) {
        logf("drawn marks: not patching -- the stock byte templates do not carry the six "
             "gpl_drawnMarkCount and three gpl_drawnMarkList operands they are expected to, so "
             "they cannot be moved onto this image (deltas %s)",
             rebaseSummary());
        return;
    }

    auto append = reinterpret_cast<const uint8_t*>(kAppendSite);
    auto clear = reinterpret_cast<const uint8_t*>(kClearSite);
    bool appendStock = std::memcmp(append, appendPattern, sizeof(appendPattern)) == 0;
    bool clearStock = std::memcmp(clear, clearPattern, sizeof(clearPattern)) == 0;
    if (!appendStock || !clearStock) {
        logf("drawn marks: not patching -- %s%s%s no longer hold their stock bytes "
             "(%08X: %02X %02X %02X %02X %02X %02X, %08X: %02X %02X %02X %02X %02X %02X); "
             "gpl_drawnMarkList stays 2048 unbounded slots",
             appendStock ? "" : "gpl_nodeDrawAndMark's append",
             !appendStock && !clearStock ? " and " : "",
             clearStock ? "" : "gpl_renderClearDrawnMarks", static_cast<unsigned>(kAppendSite),
             append[0], append[1], append[2], append[3], append[4], append[5],
             static_cast<unsigned>(kClearSite), clear[0], clear[1], clear[2], clear[3], clear[4],
             clear[5]);
        return;
    }

    // The clear first: it walks g_list, which is still the stock list, so it agrees with the
    // stock append until the append thunk is in and the next clear moves both.
    if (!writeJump(kClearSite, reinterpret_cast<void*>(&clearDrawnMarks), 5)) {
        logf("drawn marks: could not write the jump at %08X -- not patching",
             static_cast<unsigned>(kClearSite));
        return;
    }
    g_installed = true;
    if (!writeJump(kAppendSite, reinterpret_cast<void*>(&appendThunk), 6)) {
        logf("drawn marks: could not write the jump at %08X -- the clear goes through gplvr but "
             "appends stay stock, 2048 unbounded slots",
             static_cast<unsigned>(kAppendSite));
        return;
    }
    InterlockedExchange(&g_appendInstalled, 1);
    logf("drawn marks: gpl_nodeDrawAndMark's append at %08X and gpl_renderClearDrawnMarks at "
         "%08X go through gplvr, over gpl_drawnMarkCount %08X and gpl_drawnMarkList %08X -- the "
         "list moves to %u slots at the next clear, and a full list unmarks the node instead of "
         "overwriting the globals after it",
         static_cast<unsigned>(kAppendSite), static_cast<unsigned>(kClearSite),
         static_cast<unsigned>(g_count), static_cast<unsigned>(g_stockList), kSlots);
}

bool drawnMarksFormat(char* out, size_t size, unsigned frames) {
    if (!g_installed) return false;
    uint32_t clears = g_clears - g_windowClearsStart;
    uint32_t overflows = g_overflows - g_windowOverflowsStart;
    bool     report = clears > 0 || overflows > 0;
    if (report)
        std::snprintf(out, size,
                      "drawn marks: peak %u of %u (clears %u, overflows %u) | appends between "
                      "clears max %u | clears %.2f a frame | session peak %u, overflows %u",
                      g_windowPeak, g_capacity, clears, overflows, g_windowMaxAppends,
                      frames ? static_cast<double>(clears) / frames : 0.0, g_sessionPeak,
                      g_overflows);
    g_windowPeak = 0;
    g_windowMaxAppends = 0;
    g_windowClearsStart = g_clears;
    g_windowOverflowsStart = g_overflows;
    return report;
}

void drawnMarksLogSummary() {
    if (g_installed)
        logf("drawn marks: session peak %u of %u slots, %u appends dropped as overflows",
             g_sessionPeak, g_capacity, g_overflows);
}

}  // namespace gplvr
