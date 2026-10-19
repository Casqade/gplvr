#include "SkidFlood.h"

#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// gplc67.exe, read in IDA on 2026-10-17. Code, the same in both links; no data operands.
struct Site {
    uintptr_t     address;
    unsigned char stock[7];
    unsigned      length;
    unsigned      immediate;  // offset of the 80000000h inside the instruction
    const char*   what;
};
constexpr Site kSites[3] = {
    {0x00483576, {0xF7, 0x03, 0x00, 0x00, 0x00, 0x80}, 6, 2,
     "sub_483550's test dword ptr [ebx], 80000000h"},
    {0x00483582, {0xC7, 0x45, 0xF8, 0x00, 0x00, 0x00, 0x80}, 7, 3,
     "sub_483550's mov [ebp-8], 80000000h (the lock or's operand)"},
    {0x004834ED, {0xBF, 0x00, 0x00, 0x00, 0x80}, 5, 1,
     "sub_4834E0's mov edi, 80000000h (its test, and with not the clear's mask)"},
};
constexpr uint32_t kSkidBit = 0x10000000;

}  // namespace

void skidFloodInstall() {
    for (const Site& s : kSites)
        if (std::memcmp(reinterpret_cast<const void*>(s.address), s.stock, s.length) != 0) {
            logf("skid flood: %08X (%s) does not hold its stock bytes -- not patching",
                 static_cast<unsigned>(s.address), s.what);
            return;
        }
    bool ok = true;
    for (const Site& s : kSites)
        if (ok) ok = patchBytes(reinterpret_cast<void*>(s.address + s.immediate), &kSkidBit, 4);
    logf("skid flood: %s -- the skid-mark flood (sub_483550/sub_4834E0, sim thread) marks cells with "
         "bit 28 instead of bit 31, the render walk's drawn mark, so a skid mark laid during an eye's "
         "walk no longer hides the cells around the car from that eye",
         ok ? "patched" : "A WRITE FAILED PART WAY -- the flood may now mark and clear different bits");
}

}  // namespace gplvr
