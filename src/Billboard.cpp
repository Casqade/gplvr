#include "Billboard.h"

#include "Hooks.h"
#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// The call site inside sub_4D6490, and what has to be there before we touch it.
//
// `call sub_4C7380` -- the one-line accessor that copies modelview row 0 out to the
// caller's buffer. Its rel32 encodes 0x4C7380 - (0x4D64CB + 5) = 0xFFFF0EB0.
const uintptr_t kCallSite = 0x004D64CB;
const unsigned char kStockCall[5] = {0xE8, 0xB0, 0x0E, 0xFF, 0xFF};

// The eye position in the CURRENT LOCAL frame -- the same frame the sprite's anchor
// vertex is in, which is what makes the subtraction meaningful. The matrix stack
// maintains it, and sub_4C7880's plane-side test reads it the same way. Its value is
// still the object's own frame at the call site: sub_4D6490 does not push until 0x4D6994.
float* const kEyeLocal = reinterpret_cast<float*>(0x008676A0);

// Top of the modelview stack, for the degenerate fallback.
float** const kModelviewTop = reinterpret_cast<float**>(0x0056FE28);

}  // namespace

// out <- the horizontal direction from the sprite to the eye.
//
// anchorRecord is the caller's arg_0: a cached vertex whose first dword is the transform
// cache's back-pointer, so the coordinates start one float in.
//
// External linkage and a C name: its only caller is the inline asm below, and a static in
// an anonymous namespace could be discarded by a link-time optimiser that cannot see that
// reference.
extern "C" void __cdecl gplvrBillboardAxis(float* out, const float* anchorRecord) {
    const float* anchor = anchorRecord + 1;

    float dx = anchor[0] - kEyeLocal[0];
    float dy = anchor[1] - kEyeLocal[1];

    // The eye directly above or below the sprite leaves the horizontal direction
    // undefined, and cross((0,0,1), v) would collapse to zero. Stock behaviour is a
    // reasonable answer there and the sprite is edge-on anyway.
    if (dx * dx + dy * dy < 1e-8f) {
        const float* row0 = *kModelviewTop;
        out[0] = row0[0];
        out[1] = row0[1];
        out[2] = row0[2];
        return;
    }

    out[0] = dx;
    out[1] = dy;
    out[2] = 0.0f;
}

namespace {

// Entry esp holds the return address, [esp+4] is the out pointer, and the caller's arg_0
// sits at [esp+0x104]: sub_4D6490 enters with esp = E, subtracts 0xEC, pushes four
// registers, and the call pushes the return address, so arg_0 at E+4 is 0x104 above us.
// Naked so that offset is exactly what it says rather than whatever a prologue chose.
//
// __cdecl, so the caller cleans -- plain `ret`, matching the `add esp,4` at 0x4D64D0.
__declspec(naked) void billboardAxisThunk() {
    __asm {
        push ebp
        mov  ebp, esp
        push eax
        push ecx
        push edx
        mov  eax, [ebp+0x108]       // arg_0, the anchor vertex
        mov  ecx, [ebp+8]           // the out pointer
        push eax
        push ecx
        call gplvrBillboardAxis
        add  esp, 8
        pop  edx
        pop  ecx
        pop  eax
        pop  ebp
        ret
    }
}

}  // namespace

void billboardInstall(const Binding&) {
    if (!g_options.billboardFaceEye) return;

    unsigned char* site = reinterpret_cast<unsigned char*>(kCallSite);
    if (memcmp(site, kStockCall, sizeof(kStockCall)) != 0) {
        logf("billboard: %08X does not hold the stock `call sub_4C7380` "
             "(%02X %02X %02X %02X %02X) -- not patching",
             static_cast<unsigned>(kCallSite), site[0], site[1], site[2], site[3],
             site[4]);
        return;
    }

    int32_t rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&billboardAxisThunk) - (kCallSite + 5));
    if (!patchBytes(site + 1, &rel, sizeof(rel))) {
        logf("billboard: could not write the call at %08X",
             static_cast<unsigned>(kCallSite));
        return;
    }
    logf("billboard: sub_4D6490's camera-row read at %08X now returns the horizontal "
         "direction to the eye -- vertical-axis billboards face the viewer instead of "
         "the view plane",
         static_cast<unsigned>(kCallSite));
}

}  // namespace gplvr
