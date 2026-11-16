#include "TextEntry.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// gpl_widgetMgrSingleton, band C.
constexpr uintptr_t kRetailWidgetMgr = 0x0094D77C;

constexpr uint32_t kWindowVtbl = 0x0053D6F0;
constexpr uint32_t kEditBoxVtbl = 0x0053CC50;
constexpr uint32_t kEditBox2Vtbl = 0x0053CCD8;
constexpr uint32_t kEditBoxOnEvent = 0x004F69C0;
constexpr uint32_t kEditBoxGainFocus = 0x004F6B40;
constexpr uint32_t kEditBoxLoseFocus = 0x004F6BE0;

// Offsets the walk reads, each one named by a template below.
constexpr size_t kMgrWindowList = 0x14;
constexpr size_t kWindowRegister = 0x04;
constexpr size_t kRegisterShown = 0x18;
constexpr size_t kWindowChildren = 0x39;
constexpr size_t kVectorData = 0x0C;
constexpr size_t kEditBoxFocused = 0x09;
constexpr size_t kEditBoxReadOnly = 0x149;

// Bounds on a walk through heap the vtable tests vouch for; far above anything GPL builds.
constexpr unsigned kMaxWindows = 64;
constexpr unsigned kMaxChildren = 512;

struct Template {
    const char*          name;
    uintptr_t            site;
    const unsigned char* bytes;
    size_t               length;
    unsigned             mgrOperands;
};

// gpl_widgetMgrGet: push ecx / mov eax, [gpl_widgetMgrSingleton] / push esi / test eax, eax / jnz.
constexpr unsigned char kMgrGetBytes[] = {0x51, 0xA1, 0x7C, 0xD7, 0x94, 0x00,
                                          0x56, 0x85, 0xC0, 0x75};
// gpl_widgetMgrAddWindow at 0x4F3D57: the node {window, next} pushed on the list at mgr+0x14.
constexpr unsigned char kAddWindowBytes[] = {0x8B, 0x4E, 0x14, 0x89, 0x18, 0x89,
                                             0x48, 0x04, 0x89, 0x46, 0x14};
// gpl_windowCtor: the base ctor, then the vtable store and the focus child at +0x42 cleared.
constexpr unsigned char kWindowCtorBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x50,
                                              0xE8, 0x63, 0xEE, 0xFE, 0xFF, 0x33, 0xC0, 0xC7,
                                              0x06, 0xF0, 0xD6, 0x53, 0x00};
// gpl_windowOnEvent at 0x5013DA: the register at +4 and its shown byte at +0x18.
constexpr unsigned char kWindowShownBytes[] = {0x8B, 0x45, 0x04, 0x8A, 0x48, 0x18, 0x84, 0xC9};
// gpl_windowOnEvent at 0x5014FA: the child vector at +0x39, its data at +0x0C and count at +0.
constexpr unsigned char kWindowChildrenBytes[] = {0x8B, 0x45, 0x39, 0x8B, 0x70, 0x0C, 0x8B, 0x08};
// gpl_editBoxOnEvent: unhandled while the read-only dword at +0x149 is set, then event 0xB0B (a
// character) branches to 0x4F6A88.
constexpr unsigned char kEditBoxGateBytes[] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x49, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x0F,
    0x85, 0x21, 0x01, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x08, 0x81,
    0xE9, 0x0B, 0x0B, 0x00, 0x00, 0x0F, 0x84, 0xA5, 0x00, 0x00, 0x00};
// gpl_editBoxOnEvent at 0x4F6A88: the character is refused unless the focus byte +9 is set.
constexpr unsigned char kEditBoxCharBytes[] = {0x8A, 0x4E, 0x09, 0x84, 0xC9, 0x74, 0x63};
// gpl_widgetGainFocus: +9 is set only for a widget whose +0x17 is set.
constexpr unsigned char kGainFocusBytes[] = {0x8A, 0x41, 0x17, 0x84, 0xC0, 0x74,
                                             0x04, 0xC6, 0x41, 0x09, 0x01};

const Template kTemplates[] = {
    {"gpl_widgetMgrGet", 0x004F3070, kMgrGetBytes, sizeof(kMgrGetBytes), 1},
    {"gpl_widgetMgrAddWindow's list push", 0x004F3D57, kAddWindowBytes, sizeof(kAddWindowBytes), 0},
    {"gpl_windowCtor", 0x00501240, kWindowCtorBytes, sizeof(kWindowCtorBytes), 0},
    {"gpl_windowOnEvent's shown gate", 0x005013DA, kWindowShownBytes, sizeof(kWindowShownBytes),
     0},
    {"gpl_windowOnEvent's child walk", 0x005014FA, kWindowChildrenBytes,
     sizeof(kWindowChildrenBytes), 0},
    {"gpl_editBoxOnEvent's read-only gate", kEditBoxOnEvent, kEditBoxGateBytes,
     sizeof(kEditBoxGateBytes), 0},
    {"gpl_editBoxOnEvent's focus gate", 0x004F6A88, kEditBoxCharBytes, sizeof(kEditBoxCharBytes),
     0},
    {"gpl_widgetGainFocus", 0x004EC3C0, kGainFocusBytes, sizeof(kGainFocusBytes), 0},
};

uintptr_t g_mgrSlot = 0;
bool      g_ready = false;
unsigned  g_faults = 0;
unsigned  g_inhibited = 0;

template <typename T>
T readAt(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

bool templateMatches(const Template& t) {
    unsigned char expected[32];
    if (t.length > sizeof(expected)) return false;
    std::memcpy(expected, t.bytes, t.length);
    if (rebaseOperands(expected, t.length, kRetailWidgetMgr, g_mgrSlot) != t.mgrOperands) {
        logf("text entry: %s's template names %08X a different number of times than %u -- "
             "refused",
             t.name, static_cast<unsigned>(kRetailWidgetMgr), t.mgrOperands);
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(t.site), expected, t.length) == 0) return true;
    logf("text entry: %s at %08X does not hold the code that was studied -- refused", t.name,
         static_cast<unsigned>(t.site));
    return false;
}

// Both edit-box vtables must dispatch the event and the focus slots to the edit box's own code.
bool vtableMatches(uint32_t vtable) {
    auto slots = reinterpret_cast<const uint32_t*>(vtable);
    if (slots[2] == kEditBoxOnEvent && slots[5] == kEditBoxGainFocus &&
        slots[6] == kEditBoxLoseFocus)
        return true;
    logf("text entry: the vtable at %08X does not dispatch to gpl_editBoxOnEvent and its focus "
         "slots -- refused",
         static_cast<unsigned>(vtable));
    return false;
}

bool walk() {
    auto mgr = readAt<uintptr_t>(g_mgrSlot);
    if (!mgr) return false;
    auto     node = readAt<uintptr_t>(mgr + kMgrWindowList);
    unsigned windows = 0;
    for (; node && windows < kMaxWindows; node = readAt<uintptr_t>(node + 4), ++windows) {
        auto window = readAt<uintptr_t>(node);
        if (!window || readAt<uint32_t>(window) != kWindowVtbl) continue;
        auto shown = readAt<uintptr_t>(window + kWindowRegister);
        if (!shown || !readAt<uint8_t>(shown + kRegisterShown)) continue;
        auto vector = readAt<uintptr_t>(window + kWindowChildren);
        if (!vector) continue;
        auto count = readAt<uint32_t>(vector);
        auto data = readAt<uintptr_t>(vector + kVectorData);
        if (!data) continue;
        if (count > kMaxChildren) count = kMaxChildren;
        for (uint32_t i = 0; i < count; ++i) {
            auto child = readAt<uintptr_t>(data + i * 4);
            if (!child) continue;
            auto vtable = readAt<uint32_t>(child);
            if (vtable != kEditBoxVtbl && vtable != kEditBox2Vtbl) continue;
            if (readAt<uint8_t>(child + kEditBoxFocused) &&
                readAt<uint32_t>(child + kEditBoxReadOnly) == 0)
                return true;
        }
    }
    return false;
}

// Holds no C++ objects, so __try can guard the walk.
bool guardedWalk(bool& faulted) {
    __try {
        return walk();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return true;
    }
}

}  // namespace

void textEntryInstall() {
    g_mgrSlot = rebase(kRetailWidgetMgr);
    if (!g_mgrSlot) {
        logf("text entry: gpl_widgetMgrSingleton %08X cannot be placed in this image (deltas %s) "
             "-- hotkeys are not held back while a text field has the keyboard",
             static_cast<unsigned>(kRetailWidgetMgr), rebaseSummary());
    } else {
        bool ok = true;
        for (const Template& t : kTemplates) ok = templateMatches(t) && ok;
        ok = vtableMatches(kEditBoxVtbl) && ok;
        ok = vtableMatches(kEditBox2Vtbl) && ok;
        g_ready = ok;
        logf("text entry: %s",
             ok ? "GPL's edit boxes are checked on every hotkey press; a press while one has the "
                  "keyboard is left to the text"
                : "refused -- hotkeys are not held back while a text field has the keyboard");
    }
    if (!g_ready && !g_options.debugKeys && g_options.vrRecentreKey) {
        g_options.vrRecentreKey = 0;
        logf("options: vrRecentreKey=0 -- without the text field check a digit typed into a name "
             "would recentre; set debugKeys=1 to poll it anyway");
    }
}

bool textEntryReady() { return g_ready; }

bool gplTextEntryActive() {
    if (!g_ready) return false;
    bool faulted = false;
    bool active = guardedWalk(faulted);
    if (faulted && ++g_faults <= 4)
        logf("text entry: the widget walk faulted (%u so far); the press was held back", g_faults);
    return active;
}

bool hotkeyPressAllowed(int virtualKey, const char* name) {
    if (!gplTextEntryActive()) return true;
    ++g_inhibited;
    if (g_inhibited <= 8 || g_inhibited % 64 == 0)
        logf("hotkey: %s (0x%02X) left to GPL's text field (%u held back so far)", name,
             static_cast<unsigned>(virtualKey), g_inhibited);
    return false;
}

}  // namespace gplvr
