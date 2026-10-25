#include "FlagmanReplay.h"

#include "DataRebase.h"
#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// gplc67.exe, read in IDA on 2026-10-25. Code, the same in both links; the data operands differ,
// so the stock bytes are completed with this image's addresses before they are compared.
constexpr uintptr_t kHandler = 0x00482160;      // sub_482160, replay block 9's handler
constexpr uintptr_t kCountLoad = 0x00481890;    // mov ebp, [states] (8B 2D imm32)
constexpr uintptr_t kEntriesLoad = 0x004818A7;  // mov edi, states + 2 (BF imm32)

// Reference-link data, placed through rebase().
constexpr uintptr_t kStates = 0x0074F368;      // the count word, then one word per flagman
constexpr uintptr_t kStatesSize = 0x0074F52C;  // the last block's size; nonzero while one is held
// From the count word to the waving.3do frame table at 0x74F3D8: the count and 55 flagmen.
constexpr unsigned kStockCapacity = 0x70;

// sub_482160 whole: mov eax, [esp+8]; push esi; mov esi, [esp+10h]; mov ecx, eax; mov edx, ecx;
// push edi; mov edi, states; shr ecx, 2; rep movsd; mov ecx, edx; and ecx, 3; rep movsb; pop edi;
// mov [statesSize], eax; pop esi; retn. The two zeroed operands are filled in at install.
constexpr unsigned char kHandlerStock[39] = {
    0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x8B, 0xC8, 0x8B, 0xD1,
    0x57, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xC1, 0xE9, 0x02, 0xF3, 0xA5, 0x8B, 0xCA,
    0x83, 0xE1, 0x03, 0xF3, 0xA4, 0x5F, 0xA3, 0x00, 0x00, 0x00, 0x00, 0x5E, 0xC3};
constexpr unsigned kHandlerStatesOperand = 15;
constexpr unsigned kHandlerSizeOperand = 33;

// The live flagman block holds at most 127 (Flagmen.cpp), and gpl_flagmanBindAllModels reads the
// entries only when the count word equals the live count; room for twice that.
constexpr size_t kStateWords = 1 + 256;

alignas(4) uint16_t g_states[kStateWords];
uint32_t*           g_statesSize = nullptr;
volatile LONG       g_blocks = 0;
volatile LONG       g_overStock = 0;  // blocks longer than GPL's own buffer
volatile LONG       g_clamped = 0;    // blocks longer than this one, cut to it
volatile LONG       g_logged = 0;

// Entered by a jmp from 0x482160 with GPL's cdecl call frame intact; returns to its caller as the
// stock handler does, with the block's size in eax.
int __cdecl replayStatesHandler(int, unsigned size, const void* data) {
    InterlockedIncrement(&g_blocks);
    size_t copied = size;
    if (copied > sizeof(g_states)) {
        copied = sizeof(g_states);
        InterlockedIncrement(&g_clamped);
    }
    std::memcpy(g_states, data, copied);
    *g_statesSize = size;
    if (size > kStockCapacity) {
        InterlockedIncrement(&g_overStock);
        if (InterlockedExchange(&g_logged, 1) == 0)
            logf("flagman replay: a %u-byte block of flagman states (%u flagmen) played back -- GPL's "
                 "own buffer holds %u bytes and would have written the rest over the waving.3do "
                 "frame table; kept in gplvr's (%u bytes)",
                 size, static_cast<unsigned>(g_states[0]), kStockCapacity,
                 static_cast<unsigned>(sizeof(g_states)));
    }
    return static_cast<int>(size);
}

}  // namespace

void flagmanReplayInstall() {
    uintptr_t states = rebase(kStates);
    uintptr_t statesSize = rebase(kStatesSize);
    if (!states || !statesSize) {
        logf("flagman replay: rebase refused the state buffer or its size word (%s) -- not "
             "patching; a replay on a track with more than 55 flagmen still overruns it",
             rebaseSummary());
        return;
    }
    auto states32 = static_cast<uint32_t>(states);
    auto size32 = static_cast<uint32_t>(statesSize);
    auto entries32 = states32 + 2;

    unsigned char handler[sizeof(kHandlerStock)];
    std::memcpy(handler, kHandlerStock, sizeof(handler));
    std::memcpy(handler + kHandlerStatesOperand, &states32, 4);
    std::memcpy(handler + kHandlerSizeOperand, &size32, 4);
    unsigned char countLoad[6] = {0x8B, 0x2D};
    std::memcpy(countLoad + 2, &states32, 4);
    unsigned char entriesLoad[5] = {0xBF};
    std::memcpy(entriesLoad + 1, &entries32, 4);

    struct Check {
        uintptr_t            address;
        const unsigned char* stock;
        size_t               length;
        const char*          what;
    };
    const Check checks[] = {
        {kHandler, handler, sizeof(handler), "sub_482160, replay block 9's handler"},
        {kCountLoad, countLoad, sizeof(countLoad), "gpl_flagmanBindAllModels' count read"},
        {kEntriesLoad, entriesLoad, sizeof(entriesLoad), "gpl_flagmanBindAllModels' entries"},
    };
    for (const Check& c : checks)
        if (std::memcmp(reinterpret_cast<const void*>(c.address), c.stock, c.length) != 0) {
            logf("flagman replay: %08X (%s) does not hold its stock bytes for state buffer %08X -- "
                 "not patching",
                 static_cast<unsigned>(c.address), c.what, states32);
            return;
        }

    g_statesSize = reinterpret_cast<uint32_t*>(statesSize);
    auto ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_states[0]));
    auto oursEntries = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_states[1]));
    unsigned char jump[5] = {0xE9};
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&replayStatesHandler) -
                                    (kHandler + 5));
    std::memcpy(jump + 1, &rel, 4);

    // The two reads first, then the handler, each undone if a later write fails, so the old buffer
    // and ours are never read and written in a mix.
    bool count = patchBytes(reinterpret_cast<void*>(kCountLoad + 2), &ours, 4);
    bool entries = count && patchBytes(reinterpret_cast<void*>(kEntriesLoad + 1), &oursEntries, 4);
    bool jumped = entries && patchBytes(reinterpret_cast<void*>(kHandler), jump, sizeof(jump));
    if (!jumped) {
        if (entries)
            patchBytes(reinterpret_cast<void*>(kEntriesLoad), entriesLoad, sizeof(entriesLoad));
        if (count) patchBytes(reinterpret_cast<void*>(kCountLoad), countLoad, sizeof(countLoad));
        logf("flagman replay: could not write %s -- put back, not patching",
             !count ? "the count read" : !entries ? "the entries read" : "the handler's jump");
        return;
    }
    logf("flagman replay: patched -- replay block 9 (the flagmen's states) is kept in gplvr's "
         "%u-byte buffer instead of GPL's %u bytes at %08X, which a track with more than 55 "
         "flagmen overran into the waving.3do frame table (Isle of Man has 79)",
         static_cast<unsigned>(sizeof(g_states)), kStockCapacity, states32);
}

void flagmanReplayLogSummary() {
    if (g_blocks > 0)
        logf("flagman replay: %ld blocks of flagman states played back this session, %ld of them "
             "longer than GPL's own buffer, %ld cut to gplvr's",
             g_blocks, g_overStock, g_clamped);
}

}  // namespace gplvr
