#include "NoSignal.h"

#include "Log.h"
#include "Pe.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// gplc67.exe addresses. The exe has no relocations and the slot is in .rdata, below the range
// the US/UK link moves; nothing is written unless the slot holds the stock target.
//
//   0x537604  the driving view widget's draw slot  -> 0x423100 gpl_drivingViewWidgetDraw
//   0x42311C  cmp [esi+8], bl    the render branch clears the flag at 0x423130
//   0x423165  mov byte ptr [esi+8], 1   the static branch sets it
//   0x423145  retn 4             __thiscall, one stack argument
//   0x4C7BE0  gpl_cameraGetBoundCurrent: mov eax, [gpl_cameraBoundCurrent]; retn
//             the static branch draws over *camera, that camera's surface handle
constexpr uintptr_t kDrawSlot = 0x00537604;
constexpr uintptr_t kStockDraw = 0x00423100;
constexpr unsigned  kStaticFlag = 8;
constexpr uintptr_t kCameraGetBoundCurrent = 0x004C7BE0;

typedef char(__thiscall* DrawFn)(void*, int);

DrawFn g_original = nullptr;
bool   g_drawnThisFrame = false;
// gpl_cameraBoundCurrent, read out of the getter's own instruction; null when the getter's
// bytes are not the expected two instructions, and then the surface is unknown.
void* const* g_boundCamera = nullptr;
void*        g_camera = nullptr;
void*        g_surface = nullptr;

bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto start = static_cast<const uint8_t*>(info.BaseAddress);
    return static_cast<const uint8_t*>(address) + bytes <= start + info.RegionSize;
}

char __fastcall drawThunk(void* self, void*, int argument) {
    char result = g_original(self, argument);
    auto flag = static_cast<const uint8_t*>(self) + kStaticFlag;
    if (!readable(flag, 1) || !*flag) return result;
    g_drawnThisFrame = true;
    // The camera the static was drawn through is still the bound one: the branch binds none.
    void* camera = g_boundCamera && readable(g_boundCamera, sizeof(void*)) ? *g_boundCamera
                                                                         : nullptr;
    g_camera = camera;
    g_surface = camera && readable(camera, sizeof(void*)) ? *static_cast<void**>(camera)
                                                          : nullptr;
    return result;
}

}  // namespace

void noSignalInstall() {
    auto slot = reinterpret_cast<void**>(kDrawSlot);
    if (!readable(slot, sizeof(void*))) {
        logf("no signal: the driving view's draw slot at %#010x is not readable -- GPL's static "
             "cannot be told from a world frame, and goes to the headset as a pair",
             static_cast<unsigned>(kDrawSlot));
        return;
    }
    if (reinterpret_cast<uintptr_t>(*slot) != kStockDraw) {
        logf("no signal: the draw slot at %#010x holds %p, not the expected %#010x -- this is "
             "not the gplc67.exe it was read from, so nothing is written and GPL's static goes "
             "to the headset as a pair",
             static_cast<unsigned>(kDrawSlot), *slot, static_cast<unsigned>(kStockDraw));
        return;
    }
    g_original = reinterpret_cast<DrawFn>(*slot);
    void* thunk = reinterpret_cast<void*>(&drawThunk);
    if (!patchBytes(slot, &thunk, sizeof(void*))) {
        g_original = nullptr;
        logf("no signal: the draw slot at %#010x could not be written -- GPL's static goes to "
             "the headset as a pair",
             static_cast<unsigned>(kDrawSlot));
        return;
    }
    auto getter = reinterpret_cast<const uint8_t*>(kCameraGetBoundCurrent);
    if (readable(getter, 6) && getter[0] == 0xA1 && getter[5] == 0xC3) {
        void* const* global = nullptr;
        memcpy(&global, getter + 1, sizeof(global));
        g_boundCamera = global;
    }
    logf("no signal: the driving view's draw is thunked, so a frame GPL fills with its static "
         "is known as one%s",
         g_boundCamera ? ", along with the surface it was drawn on"
                       : ". gpl_cameraGetBoundCurrent at 0x004C7BE0 is not `mov eax, [global]; "
                         "retn`, so the surface it was drawn on is not known");
}

bool noSignalDrawnThisFrame() { return g_drawnThisFrame; }

void* noSignalSurface() { return g_surface; }

void* noSignalCamera() { return g_camera; }

void noSignalEndFrame() {
    g_drawnThisFrame = false;
    g_camera = nullptr;
    g_surface = nullptr;
}

}  // namespace gplvr
