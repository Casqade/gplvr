#include "Billboard.h"

#include "Hooks.h"
#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cmath>
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

// The two call sites inside sub_4D69F0, the fully screen-aligned sprite. It spans its quad
// along modelview row 1 (the camera's left axis in the sprite's local frame, read by
// sub_4C73B0) and row 2 (the camera's up axis, sub_4C73E0), so the quad lies in the view
// plane and turns and rolls with the head. The pit board is this kind of sprite:
// pitboard.3do is one type-3 node on pitboard.srb, whose SRH2 chunk sets the +32 byte that
// sends sub_4D6FA0 here.
namespace {
const uintptr_t kSpriteLeftSite = 0x004D6A40;
const unsigned char kStockLeftCall[5] = {0xE8, 0x6B, 0x09, 0xFF, 0xFF};
const uintptr_t kSpriteUpSite = 0x004D6A65;
const unsigned char kStockUpCall[5] = {0xE8, 0x76, 0x09, 0xFF, 0xFF};
}  // namespace

// out <- axis 1 (left) or 2 (up) of a basis whose forward runs from the eye to the sprite
// and whose up stays in the vertical plane through both, so the quad faces the eye
// position whatever the head's orientation. GPL axes: X forward, Y left, Z up, so
// left = cross(Z, forward) and up = cross(forward, left). With the eye straight ahead on
// the level, that is (0,1,0) and (0,0,1), the rows the stock read returns there.
extern "C" void __cdecl gplvrSpriteAxis(float* out, const float* anchorRecord, int axis) {
    const float* anchor = anchorRecord + 1;

    float fx = anchor[0] - kEyeLocal[0];
    float fy = anchor[1] - kEyeLocal[1];
    float fz = anchor[2] - kEyeLocal[2];
    float horizontal = fx * fx + fy * fy;

    // Directly above or below, left is undefined; the stock view-plane row is as good an
    // answer as any for a sprite seen edge-on.
    if (horizontal < 1e-8f) {
        const float* row = *kModelviewTop + axis * 4;
        out[0] = row[0];
        out[1] = row[1];
        out[2] = row[2];
        return;
    }

    float length = std::sqrt(horizontal + fz * fz);
    fx /= length;
    fy /= length;
    fz /= length;

    float h = std::sqrt(fx * fx + fy * fy);
    float lx = -fy / h;
    float ly = fx / h;

    if (axis == 1) {
        out[0] = lx;
        out[1] = ly;
        out[2] = 0.0f;
    } else {
        out[0] = -fz * ly;
        out[1] = fz * lx;
        out[2] = fx * ly - fy * lx;
    }
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

// sub_4D69F0 enters with esp = E, subtracts 0xE0 and pushes edi, esi, ebx and the out
// pointer, and the call pushes the return address: arg_0 at E+4 is 0xF8 above our entry,
// 0xFC above ebp once it is pushed. Both call sites sit at that depth -- the second follows
// an `add esp,4` and a `push eax`. __cdecl, plain `ret`, as the stock accessors.
__declspec(naked) void spriteLeftThunk() {
    __asm {
        push ebp
        mov  ebp, esp
        push eax
        push ecx
        push edx
        push 1
        push dword ptr [ebp+0xFC]   // arg_0, the anchor vertex
        push dword ptr [ebp+8]      // the out pointer
        call gplvrSpriteAxis
        add  esp, 12
        pop  edx
        pop  ecx
        pop  eax
        pop  ebp
        ret
    }
}

__declspec(naked) void spriteUpThunk() {
    __asm {
        push ebp
        mov  ebp, esp
        push eax
        push ecx
        push edx
        push 2
        push dword ptr [ebp+0xFC]
        push dword ptr [ebp+8]
        call gplvrSpriteAxis
        add  esp, 12
        pop  edx
        pop  ecx
        pop  eax
        pop  ebp
        ret
    }
}

// Point the call at site to target if it still holds the stock call. Logs a refusal.
bool redirectCall(uintptr_t site, const unsigned char stock[5], void (*target)(),
                  const char* what) {
    unsigned char* bytes = reinterpret_cast<unsigned char*>(site);
    if (memcmp(bytes, stock, 5) != 0) {
        logf("billboard: %08X (%s) does not hold the stock call "
             "(%02X %02X %02X %02X %02X) -- not patching",
             static_cast<unsigned>(site), what, bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4]);
        return false;
    }
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    if (!patchBytes(bytes + 1, &rel, sizeof(rel))) {
        logf("billboard: could not write the call at %08X (%s)",
             static_cast<unsigned>(site), what);
        return false;
    }
    return true;
}

}  // namespace

void billboardInstall(const Binding&) {
    // Both rows or neither: a quad spanned by one eye-facing axis and one view-plane axis
    // would shear. The up site is checked before the left one is written.
    if (memcmp(reinterpret_cast<unsigned char*>(kSpriteUpSite), kStockUpCall, 5) == 0 &&
        redirectCall(kSpriteLeftSite, kStockLeftCall, &spriteLeftThunk, "sprite left") &&
        redirectCall(kSpriteUpSite, kStockUpCall, &spriteUpThunk, "sprite up")) {
        logf("billboard: sub_4D69F0's camera-row reads at %08X/%08X now return a basis "
             "facing the eye -- screen-aligned sprites (the pit board) face the viewer "
             "and no longer turn or roll with the head",
             static_cast<unsigned>(kSpriteLeftSite), static_cast<unsigned>(kSpriteUpSite));
    } else {
        logf("billboard: screen-aligned sprites left stock (sub_4D69F0 not patched)");
    }

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
