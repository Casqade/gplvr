#include "Letterbox.h"

#include "Hooks.h"
#include "DataRebase.h"
#include "Log.h"
#include "Pe.h"
#include "Window.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gplvr {
namespace {

// EVERY ADDRESS HERE IS A gplc67.exe ADDRESS, AND EVERY ONE OF THEM IS CHECKED BEFORE IT
// IS USED. The exe has no relocations and loads at 0x400000, which is what makes a
// constant meaningful at all; the guards are what make a constant safe. Nothing is
// written unless the three layout slots still hold their stock targets, and no peer is
// touched unless its vtable is the class this expects.
//
// Verified against the installed gplc67.exe on 2026-09-11, and every offset below was
// read back out of the instructions rather than taken from a decompiler's struct guess:
//
//   0x4FFF50  add esi, 0x329         809-byte records, terminated by a zero first byte
//   0x41AABA  mov ecx, [esi+0x12]    the UI element's peer
//   0x41AAC7  push 0; push 0; call [edx+0x1C]     SetPosition(x, y)
//   0x41AADC  push edi; push eax; call [edx+0x24] SetSize(w, h)

// The `+0x6C` "lay this screen out" virtual, in three vtables: driving, replay, and the
// base whose implementation the other two call.
constexpr uintptr_t kLayoutSlot[3] = {0x0053747C, 0x00537534, 0x005375F4};
constexpr uintptr_t kStockLayout[3] = {0x004172E0, 0x0041A120, 0x0041AA50};

// The driving screen is 0x005B7418 and the replay screen 0x005B76F0, both statically
// allocated globals. Only their vtables are needed here: the layout thunk is handed the
// screen it was called on, so nothing has to reach them by address.
constexpr uintptr_t kScreenVtbl[2] = {0x00537410, 0x005374C8};

// The middle panel -- the picture band -- is resolved by name into a global, unlike the
// two bars which live in the screen.
// A reference-link address, placed at the read. The pointer it yields is validated by its
// vtable in peerOf, so a wrong one degrades rather than misbehaves -- but on an image where
// it cannot be placed the right answer is no middle panel at all, not a vtable gamble.
constexpr uintptr_t kRetailMiddleElement = 0x005B79D4;

// The widget list is TWO hops from the screen, and the first hop is the correction that
// cost a run: the name lookups are member functions of the object at `screen + 4`, not of
// the screen, and it is that object which carries the list.
//
//   0x41AE49  mov esi, ecx        esi = the screen
//   0x41AE51  mov edi, [esi+4]    edi = screen->field_4
//   0x41AE59  mov ecx, edi        ... which is the `this` sub_4F0A00 receives
//   0x4F08B4  mov ecx, [ecx+0x29] ... and dereferences to reach the list
constexpr unsigned kScreenUiRoot = 0x04;
constexpr unsigned kUiRootWidgetList = 0x29;
constexpr unsigned kScreenTopElement = 0x30;
constexpr unsigned kScreenBottomElement = 0x34;

constexpr uintptr_t kPeerVtblRectSprite = 0x0053D330;   // rectTop, rectBottom
constexpr uintptr_t kPeerVtblTrackSprite = 0x0053CF98;  // gfxFullHires

// The three peer slots this calls, and the addresses they must hold. They are the BASE
// implementations, inherited unchanged by every widget class in the game -- which is
// exactly why they are called and never patched.
constexpr unsigned kSlotHide = 0x14 / 4;
constexpr unsigned kSlotSetPosition = 0x1C / 4;
constexpr unsigned kSlotSetSize = 0x24 / 4;
constexpr uintptr_t kPeerHideImpl = 0x004FC3D0;
constexpr uintptr_t kPeerSetPositionImpl = 0x004FC250;
constexpr uintptr_t kPeerSetSizeImpl = 0x004FC310;

// A widget record. The rect is PARENT-RELATIVE, not absolute design space: a label inside
// a group carries its offset within the group, which is why `lblESC` reads 0,12 and every
// panel reads 0,0 10x10.
constexpr size_t   kRecordStride = 0x329;
constexpr unsigned kRecordY = 264;
constexpr unsigned kRecordH = 272;
constexpr unsigned kRecordX = 260;
constexpr unsigned kRecordW = 268;
constexpr unsigned kRecordType = 256;
constexpr unsigned kElementPeer = 0x12;

// A screen with more widgets than this is not a screen. The walk is over game memory and
// its terminator is a zero byte, so it gets a ceiling as well as a validity check.
constexpr int kWidgetLimit = 2048;

typedef void(__thiscall* LayoutFn)(void*);
typedef void(__thiscall* PeerHideFn)(void*);
typedef void(__thiscall* PeerPairFn)(void*, int, int);

LayoutFn g_original[3] = {};
bool     g_installed = false;

// Same shape as Window.cpp's: these are game pointers, and a mode transition can leave
// any of them stale for a frame.
bool readable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto begin = static_cast<const uint8_t*>(info.BaseAddress);
    return static_cast<const uint8_t*>(address) + bytes <= begin + info.RegionSize;
}

int32_t readInt(const void* base, unsigned offset) {
    int32_t value = 0;
    memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
    return value;
}

void* readPointer(const void* base, unsigned offset) {
    void* value = nullptr;
    memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
    return value;
}

// The peer is the window object that owns a rectangle and knows how to paint. The UI
// element in front of it is the named resource; the two are separate objects.
void* peerOf(void* element, uintptr_t wantVtbl) {
    if (!element || !readable(element, kElementPeer + sizeof(void*))) return nullptr;
    void* peer = readPointer(element, kElementPeer);
    if (!peer || !readable(peer, 0x30)) return nullptr;
    if (readInt(peer, 0) != static_cast<int32_t>(wantVtbl)) return nullptr;
    return peer;
}

void* const* vtableOf(void* peer) {
    auto vtbl = *reinterpret_cast<void* const* const*>(peer);
    if (!vtbl || !readable(vtbl, 0x30)) return nullptr;
    return vtbl;
}

bool setPeerRect(void* peer, int x, int y, int w, int h) {
    void* const* vtbl = vtableOf(peer);
    if (!vtbl) return false;
    if (reinterpret_cast<uintptr_t>(vtbl[kSlotSetPosition]) != kPeerSetPositionImpl)
        return false;
    if (reinterpret_cast<uintptr_t>(vtbl[kSlotSetSize]) != kPeerSetSizeImpl) return false;

    reinterpret_cast<PeerPairFn>(vtbl[kSlotSetPosition])(peer, x, y);
    reinterpret_cast<PeerPairFn>(vtbl[kSlotSetSize])(peer, w, h);
    return true;
}

// Hide, not SetSize(w, 0). The rect is inclusive -- `bottom = y + h - 1` -- so a zero
// height would leave bottom above top, and the dirty-rect submission that reads it has
// no reason to expect that. Hide invalidates first, so the area is repainted once.
bool hidePeer(void* peer) {
    void* const* vtbl = vtableOf(peer);
    if (!vtbl) return false;
    if (reinterpret_cast<uintptr_t>(vtbl[kSlotHide]) != kPeerHideImpl) return false;
    reinterpret_cast<PeerHideFn>(vtbl[kSlotHide])(peer);
    return true;
}

bool isKnownScreen(void* screen) {
    if (!screen || !readable(screen, 0x40)) return false;
    int32_t vtbl = readInt(screen, 0);
    return vtbl == static_cast<int32_t>(kScreenVtbl[0]) ||
           vtbl == static_cast<int32_t>(kScreenVtbl[1]);
}

// Every widget the screen declares, in the order the resource file lists them. An
// INVENTORY, once per screen -- it hides nothing.
//
// It used to hide every widget whose declared rect fell wholly inside a bar band, which
// is report 33's step 2. The first run that reached it printed the list and killed the
// idea twice over:
//
//   gfxFullHires  type 11  rect 0,0 10x10      <- the MIDDLE PANEL, rect 0,0 10x10, so
//   rectTop       type 6   rect 0,0 10x10         y+h = 10 is "inside the top bar" and
//   rectBottom    type 6   rect 0,0 10x10         the picture band hid itself one line
//                                                 after being grown to the full mode
//
// and the rest of the list is `grpHelpDialog` and its children -- `lblESC rect 0,12 0x0`,
// `RightEdge rect 298,2 0x0` -- whose rects are PARENT-RELATIVE, so a band test in design
// space is not a test of anything. The driving screen declares the three panels and the
// F1 help dialog and nothing else: **there is no bar text on this screen to hide.**
//
// Kept as a log because it is how that was established, and because a screen whose
// inventory ever does contain something in a bar would show up here first.
int reportWidgets(void* screen) {
    void* uiRoot = readPointer(screen, kScreenUiRoot);
    if (!uiRoot || !readable(uiRoot, kUiRootWidgetList + sizeof(void*))) {
        logf("letterbox: the screen has no UI root at +4 -- no widget list");
        return 0;
    }
    auto record = static_cast<uint8_t*>(readPointer(uiRoot, kUiRootWidgetList));

    // ONLY THE WIDGETS WITH A REAL RECT ARE LISTED. The driving screen declares 105, and about
    // a hundred of them are the help dialog's children at 0x0 -- parent-relative, which the
    // comment above explains is not a band test of anything. Listing them all cost 105 log
    // lines per screen for three useful ones. A widget that ever does sit in a bar has a size,
    // so it still shows up here, which is the whole reason this report exists.
    int seen = 0;
    int listed = 0;
    for (; record && seen < kWidgetLimit && readable(record, kRecordStride) &&
           record[0] != 0;
         record += kRecordStride, ++seen) {
        int w = readInt(record, kRecordW);
        int h = readInt(record, kRecordH);
        if (w <= 0 || h <= 0) continue;

        char name[64] = {};
        for (size_t i = 0; i + 1 < sizeof(name) && record[i]; ++i)
            name[i] = static_cast<char>(record[i]);

        ++listed;
        logf("letterbox:   %-28s type %-3d rect %d,%d %dx%d", name,
             readInt(record, kRecordType), readInt(record, kRecordX),
             readInt(record, kRecordY), w, h);
    }

    logf("letterbox: %d widgets declared on this screen, %d with a rect of their own (the rest "
         "are 0x0 and parent-relative), none of them touched",
         seen, listed);
    return seen;
}

// Run after the game's own layout, so the panels are set to whatever the stock code
// decided and then overwritten. The order matters: the stock layout is what fills in
// rectTop and rectBottom in the first place.
void afterLayout(void* screen) {
    if (g_options.enableLetterbox) return;
    if (!isKnownScreen(screen)) return;

    int width = 0;
    int height = 0;
    windowEngineModeSize(width, height);
    if (width <= 0 || height <= 0) return;

    auto  middleSlot = reinterpret_cast<void**>(rebase(kRetailMiddleElement));
    void* middle = middleSlot && readable(middleSlot, sizeof(void*))
                       ? peerOf(*middleSlot, kPeerVtblTrackSprite)
                       : nullptr;
    void* top = peerOf(readPointer(screen, kScreenTopElement), kPeerVtblRectSprite);
    void* bottom = peerOf(readPointer(screen, kScreenBottomElement), kPeerVtblRectSprite);

    bool grew = middle && setPeerRect(middle, 0, 0, width, height);
    bool hidTop = top && hidePeer(top);
    bool hidBottom = bottom && hidePeer(bottom);

    static int reported = 0;
    if (reported < 2) {
        ++reported;
        logf("letterbox: screen %p laid out -- middle panel %s to %dx%d, top bar %s, "
             "bottom bar %s",
             screen, grew ? "grown" : "NOT grown", width, height,
             hidTop ? "hidden" : "NOT hidden", hidBottom ? "hidden" : "NOT hidden");
        reportWidgets(screen);
    }
}

template <int Index>
void __fastcall layoutThunk(void* self, void*) {
    if (g_original[Index]) g_original[Index](self);
    afterLayout(self);
}

}  // namespace

void letterboxInstall() {
    if (g_options.enableLetterbox) {
        logf("letterbox: enableLetterbox = 1 -- the game keeps its own 4:3 band, so "
             "nothing is thunked");
        return;
    }

    void* thunks[3] = {reinterpret_cast<void*>(&layoutThunk<0>),
                       reinterpret_cast<void*>(&layoutThunk<1>),
                       reinterpret_cast<void*>(&layoutThunk<2>)};

    // Every slot is checked before ANY is written. A half-installed set would leave the
    // driving screen widened and the replay screen not, which is a worse thing to debug
    // than a mod that refused to start.
    for (int i = 0; i < 3; ++i) {
        auto slot = reinterpret_cast<void**>(kLayoutSlot[i]);
        if (!readable(slot, sizeof(void*))) {
            logf("letterbox: the screen layout slot at %#010x is not readable -- the bar "
                 "widgets stay",
                 static_cast<unsigned>(kLayoutSlot[i]));
            return;
        }
        if (reinterpret_cast<uintptr_t>(*slot) != kStockLayout[i]) {
            logf("letterbox: the screen layout slot at %#010x holds %p, not the expected "
                 "%#010x -- this is not the gplc67.exe these addresses were read from, so "
                 "nothing is written",
                 static_cast<unsigned>(kLayoutSlot[i]), *slot,
                 static_cast<unsigned>(kStockLayout[i]));
            return;
        }
    }

    for (int i = 0; i < 3; ++i) {
        auto slot = reinterpret_cast<void**>(kLayoutSlot[i]);
        g_original[i] = reinterpret_cast<LayoutFn>(*slot);
        if (!patchBytes(slot, &thunks[i], sizeof(void*))) {
            logf("letterbox: the screen layout slot at %#010x could not be written -- the "
                 "bar widgets stay",
                 static_cast<unsigned>(kLayoutSlot[i]));
            for (int j = 0; j < i; ++j) {
                auto undo = reinterpret_cast<void**>(kLayoutSlot[j]);
                void* stock = reinterpret_cast<void*>(kStockLayout[j]);
                patchBytes(undo, &stock, sizeof(void*));
                g_original[j] = nullptr;
            }
            g_original[i] = nullptr;
            return;
        }
    }

    g_installed = true;
    logf("letterbox: the three screen layout virtuals are thunked. "
         "The middle panel is grown to the full mode and the two bar panels are hidden. "
         "The screen's widget list is logged once and otherwise left alone.");
}

void letterboxFrame() {
    // Nothing per frame. The panels are set by the layout virtual, which is the only
    // thing that moves them, and the widget list turned out to hold nothing that needs
    // hiding -- see reportWidgets. Kept as an entry point because the frame hook calls
    // it and a screen that ever does need per-frame work will need it here.
}

}  // namespace gplvr
