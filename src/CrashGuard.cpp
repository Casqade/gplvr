#include "CrashGuard.h"

#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// sub_470B90 (slot +8 of the vtables at 0x539B78, 0x539B90 and 0x539BA8): a contact sphere
// against a surface's ring of planes. With no remembered plane (*a3 == NULL) it keeps the best
// plane in ebp, starting from NULL with a best distance of -1e9 in [esp+10h]. Both loops reach
// the contact-found tail at 0x470FA1 (jz at 0x470EDD, jmp at 0x470F8B) with the x87 stack
// empty and ebx = a3. When no plane beat -1e9 -- a body diverged to ~1e14 m, or every distance
// NaN -- ebp is still NULL and 0x470FBA faults on fld [ebp+8] (report 50).
//
//   0x470FA1  D9 44 24 10   fld  dword ptr [esp+10h]   best distance
//   0x470FA5  8B 54 24 2C   mov  edx, [esp+2Ch]        a5, the depth out pointer
//   0x470FA9  8B 4C 24 28   mov  ecx, [esp+28h]        where the guard resumes
//
// Nothing branches into 0x470FA2..0x470FA8, so the eight bytes become jmp rel32 + 3 NOPs.
constexpr uintptr_t     kSite = 0x00470FA1;
constexpr uintptr_t     kResume = 0x00470FA9;
constexpr unsigned char kStock[8] = {0xD9, 0x44, 0x24, 0x10, 0x8B, 0x54, 0x24, 0x2C};

// The sim's body table, 668-byte rows indexed by car (sub_4A1D30, sub_4A40E0). Row 0 is the body
// every contact shape pointed to in report 50's dump; its position is at +0x94, the pointer
// sub_4A40E0 hands gpl_viewRecordSetPosition.
constexpr uintptr_t kBody = 0x006126DC;
constexpr uintptr_t kBodyPosition = kBody + 0x94;
constexpr uintptr_t kBodyRowBytes = 668;

constexpr LONG kLoggedFirings = 16;

volatile LONG g_firings = 0;
bool          g_bodyReadable = false;

// On the sim thread, from the thunk below.
void __stdcall noGroundPlane() {
    LONG count = InterlockedIncrement(&g_firings);
    if (count > kLoggedFirings) return;

    char body[160] = "body position not read: 006126DC is not in a writable section of the exe";
    if (g_bodyReadable) {
        float p[3];
        std::memcpy(p, reinterpret_cast<const void*>(kBodyPosition), sizeof(p));
        bool finite = std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        std::snprintf(body, sizeof(body), "body 0 (006126DC) position %g %g %g, %s", p[0], p[1],
                      p[2], finite ? "finite" : "NON-FINITE");
    }
    logf("crash guard: collision found no ground plane for a sphere (best distance stayed -1e9) "
         "-- returned no contact instead of reading NULL | #%ld this session | %s%s",
         count, body,
         count == kLoggedFirings ? " | further ones are counted, not logged" : "");
}

using NoGroundPlaneFn = void(__stdcall*)();
NoGroundPlaneFn g_noGroundPlaneFn = &noGroundPlane;
void*           g_resume = reinterpret_cast<void*>(kResume);

// Entered by a jmp from 0x470FA1, so esp is sub_470B90's own frame: [esp] edi, [esp+4] esi,
// [esp+8] ebp, [esp+0Ch] ebx, [esp+10h] best distance, then 8 more bytes of locals, the return
// address at [esp+1Ch] and the four arguments.
//
// ebp != 0: the two overwritten instructions, then back into the function at 0x470FA9.
// ebp == 0: the log call (it may use eax, ecx and edx, which the no-contact result does not
// need), then the epilog of the function's other no-contact exits (0x470EBB, 0x470F91): al = 0,
// the four callee-saved registers, the locals, and retn 10h for the four arguments. *a3 stays
// NULL and *a5 is not written; the caller sub_471950 tests al and, on 0, frees the child nodes
// and zeroes the depth itself. The x87 stack is empty and stays empty.
__declspec(naked) void contactFoundThunk() {
    __asm {
        test ebp, ebp
        jz   noPlane
        fld  dword ptr [esp+10h]
        mov  edx, [esp+2Ch]
        jmp  dword ptr [g_resume]
    noPlane:
        call dword ptr [g_noGroundPlaneFn]
        xor  al, al
        pop  edi
        pop  esi
        pop  ebp
        pop  ebx
        add  esp, 0Ch
        ret  10h
    }
}

}  // namespace

void crashGuardInstall() {
    ModuleImage image;
    g_bodyReadable = getModuleImage(GetModuleHandleA(nullptr), image) &&
                     isWritable(image, reinterpret_cast<const void*>(kBody)) &&
                     isWritable(image, reinterpret_cast<const void*>(kBody + kBodyRowBytes - 1));

    auto bytes = reinterpret_cast<const uint8_t*>(kSite);
    if (std::memcmp(bytes, kStock, sizeof(kStock)) != 0) {
        logf("crash guard: %08X does not hold sub_470B90's stock contact-found tail "
             "(%02X %02X %02X %02X %02X %02X %02X %02X) -- not patching; a sphere with no ground "
             "plane still crashes at 00470FBA",
             static_cast<unsigned>(kSite), bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
             bytes[5], bytes[6], bytes[7]);
        return;
    }

    unsigned char patch[8] = {0xE9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&contactFoundThunk) - (kSite + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    if (!patchBytes(reinterpret_cast<void*>(kSite), patch, sizeof(patch))) {
        logf("crash guard: could not write the jump at %08X -- not patching",
             static_cast<unsigned>(kSite));
        return;
    }
    logf("crash guard: sub_470B90's contact-found tail at %08X goes through gplvr -- a sphere that "
         "found no ground plane returns no contact instead of reading NULL at 00470FBA%s",
         static_cast<unsigned>(kSite),
         g_bodyReadable ? "" : " (the body position will not be logged: 006126DC is not writable)");
}

void crashGuardLogSummary() {
    LONG count = g_firings;
    if (count > 0)
        logf("crash guard: %ld contacts with no ground plane were returned as no contact this "
             "session",
             count);
}

}  // namespace gplvr
