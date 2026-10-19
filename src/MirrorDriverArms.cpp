#include "MirrorDriverArms.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// gplc67.exe, reference link. Read in IDA on 2026-10-15:
//
//   0x54BB20  gpl_graphicsOptionUser, 2 x 15 bytes: [0][i] forward, [1][i] mirror. Written only by
//             the options dialog's OK (sub_42A660 via sub_40DFE0), which copies back what its
//             init (sub_429780) read, all 30 of them; no ini key reaches [1][12..14].
//   0x54BB40  gpl_graphicsOptionAllowed, same shape. gpl_main's sub_40DE10 sets all 30 to 1 when
//             the rasterizer can texture, so only the user flag is 0.
//   0x40DFA0  mov al, gpl_graphicsOptionUser[eax+ecx]   8A 84 08 <0x54BB20>
//   0x40E460  gpl_applyGraphicsOptions: mask |= gpl_graphicsOptionSurfaceClassMask[i] when allowed
//             and user; index 13 (driver arms) is 0x20000, class 17. Run on a player load
//             (sub_40FA90) and on the dialog's OK.
//   0x559FC8  gpl_surfaceClassOptionMaskMirror, the textured mask the mirror pass applies:
//   0x487760  mov edx, gpl_surfaceClassOptionMaskMirror   8B 15 <0x559FC8>, in gpl_sceneRender
constexpr uintptr_t kUserFlags = 0x0054BB20;
constexpr unsigned  kMirrorDriverArms = 15 + 13;
constexpr uintptr_t kUserFlagRead = 0x0040DFA0;
constexpr uintptr_t kMaskMirror = 0x00559FC8;
constexpr uintptr_t kMaskMirrorRead = 0x00487760;
constexpr uint32_t  kClassDriverArms = 0x20000;

volatile uint32_t* g_maskMirror = nullptr;
bool               g_loggedEnsure = false;

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto start = static_cast<const uint8_t*>(info.BaseAddress);
    return static_cast<const uint8_t*>(address) + bytes <= start + info.RegionSize;
}

// Whether `site` holds `opcode` followed by an imm32 naming `global` -- the code read here, in
// this image's link.
bool siteNames(uintptr_t site, const uint8_t* opcode, size_t opcodeBytes, uintptr_t global) {
    auto code = reinterpret_cast<const uint8_t*>(site);
    if (!readable(code, opcodeBytes + 4)) return false;
    if (std::memcmp(code, opcode, opcodeBytes) != 0) return false;
    uint32_t operand = 0;
    std::memcpy(&operand, code + opcodeBytes, sizeof(operand));
    return operand == static_cast<uint32_t>(global);
}

}  // namespace

void mirrorDriverArmsInstall() {
    if (!g_options.mirrorDriverArms) {
        logf("mirror arms: mirrorDriverArms = 0 -- the mirror keeps GPL's default, so stock cars "
             "show their steering wheel's low-detail quad as a yellow square");
        return;
    }

    uintptr_t flags = rebase(kUserFlags);
    uintptr_t mask = rebase(kMaskMirror);
    if (!flags || !mask) {
        logf("mirror arms: the option table (%08X) or the mirror mask (%08X) cannot be placed in "
             "this image -- nothing is written",
             static_cast<unsigned>(flags), static_cast<unsigned>(mask));
        return;
    }
    static const uint8_t kUserRead[] = {0x8A, 0x84, 0x08};
    static const uint8_t kMaskRead[] = {0x8B, 0x15};
    if (!siteNames(kUserFlagRead, kUserRead, sizeof(kUserRead), flags) ||
        !siteNames(kMaskMirrorRead, kMaskRead, sizeof(kMaskRead), mask)) {
        logf("mirror arms: %08X or %08X is not the instruction it was read from (the option "
             "table's reader, the mirror pass's mask read) -- nothing is written",
             static_cast<unsigned>(kUserFlagRead), static_cast<unsigned>(kMaskMirrorRead));
        return;
    }

    auto flag = reinterpret_cast<uint8_t*>(flags + kMirrorDriverArms);
    uint8_t before = *flag;
    uint8_t on = 1;
    if (before != on && !patchBytes(flag, &on, sizeof(on))) {
        logf("mirror arms: %08X could not be written -- the mirror keeps GPL's default",
             static_cast<unsigned>(flags + kMirrorDriverArms));
        return;
    }
    g_maskMirror = reinterpret_cast<volatile uint32_t*>(mask);
    logf("mirror arms: GPL's mirror driver-arms option %08X %u -> 1, so the mirror textures class "
         "17 as the forward view does and a stock car's low-detail steering wheel is no longer a "
         "yellow square. The mirror mask is %08X now; the next options apply includes it, and a "
         "mirror pass before then adds it itself.",
         static_cast<unsigned>(flags + kMirrorDriverArms), before,
         static_cast<unsigned>(*g_maskMirror));
}

void mirrorDriverArmsEnsure() {
    if (!g_maskMirror) return;
    uint32_t mask = *g_maskMirror;
    if (mask & kClassDriverArms) return;
    *g_maskMirror = mask | kClassDriverArms;
    if (!g_loggedEnsure) {
        g_loggedEnsure = true;
        logf("mirror arms: the mirror mask was %08X, without class 17 -- GPL applied its options "
             "before the install -- so it is %08X from this mirror pass on",
             static_cast<unsigned>(mask), static_cast<unsigned>(mask | kClassDriverArms));
    }
}

}  // namespace gplvr
