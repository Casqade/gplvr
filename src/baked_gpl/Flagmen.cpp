#include "baked_gpl/Flagmen.h"

#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"

#include "DataRebase.h"
#include "Hooks.h"
#include "Log.h"
#include "Pe.h"

#include "baked/CarScene.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe; report 105 sections 4.11 and 4.13, IDA 2026-10-25) -------------

// gpl_flagmanBindModel(kind, colour index, frame, MATERIAL, side), called for every flagman by
// gpl_flagmanBindAllModels 0x481830 from the per-frame snapshot gpl_simFrameSetup 0x48A9A0. It
// sets the 16 slots of the MATERIAL's colormap with `call [colormapSetColor]` (FF 15 imm32) at
// these five sites -- slots 0..11 from the kind's sample colormap, 12..15 blended with the flag
// colour, or all 16 from the chequered one for kind 6 -- and then the MATERIAL's word 1.
constexpr uintptr_t     kBindModel = 0x00481910;
constexpr unsigned char kBindModelStart[] = {0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x24};
constexpr uintptr_t     kSetColourSites[] = {0x00481969, 0x004819C1, 0x004819F5, 0x00481A38,
                                             0x00481A8C};
constexpr size_t        kSiteCount = sizeof(kSetColourSites) / sizeof(kSetColourSites[0]);
// roglv2's colormapSetColor(handle, slot, colour), which stores the colour word as it is
// (0x20002730). gpl_rasterResolveEntryPoints 0x4E21B0 writes the slot again on every mode change,
// so the wrapper reads it on every call.
constexpr uintptr_t kColormapSetColor = 0x00866DD8;
// The flagman table, which lies past `.data` where rebase() places nothing: it is named by
// gpl_flagmanBindAllModels' own operands. The count by `mov eax, [count]` (A1 imm32) at 0x48183C
// and 0x481896; the array of flagman pointers by `mov edi, offset` (BF) at 0x48184B and
// `mov esi, offset` (BE) at 0x4818AE. Both pairs must agree. A flagman holds its side at +16, its
// kind at +20, its frame at +21 and its MATERIAL at +22 (unaligned).
constexpr uintptr_t     kCountLoads[] = {0x0048183C, 0x00481896};
constexpr uintptr_t     kObjectsLoads[] = {0x0048184B, 0x004818AE};
constexpr unsigned char kObjectsOpcodes[] = {0xBF, 0xBE};
constexpr unsigned      kFlagmanKind = 20;
constexpr unsigned      kFlagmanMaterial = 22;
// The 26-byte flagman block before the pointer array has room for 127.
constexpr int kMaxFlagmen = 127;
// `mov dword ptr [ebx+4], offset gpl_nullNode` (C7 43 04 imm32), kind 0's frame: drawn as
// nothing (gpl_nodeMaterialRender 0x4CE6A0 dispatches no child whose type word is negative).
constexpr uintptr_t     kNullNodeStore = 0x00481AAB;
constexpr unsigned char kNullNodeOpcode[] = {0xC7, 0x43, 0x04};

// Reference-link data, placed through rebase().
constexpr uintptr_t kModelViewSlot = 0x0056FE28;   // -> the current 4x4 model-view, row-major
constexpr uintptr_t kProjectionSlot = 0x0056FE2C;  // -> the 4x4 projection

constexpr uint8_t kNodeMaterial = 0x05;
constexpr uint8_t kNodeBillboard = 0x03;

// One list per flagman call; a pass reaches a handful.
constexpr uint32_t kMaxListsPerPass = 128;
// Colormaps recorded; GPL makes one per flagman at each track load. A full table starts again.
constexpr long kMaxPalettes = 512;

using SetColourFn = int(__cdecl*)(int handle, int slot, uint32_t colour);

// -- the colormaps, as GPL sets them ---------------------------------------------------------------

struct Palette {
    volatile uint32_t handle;
    volatile uint32_t mask;  // the slots set since the entry was made
    volatile uint32_t colours[16];
};

Palette           g_palettes[kMaxPalettes];
volatile long     g_paletteCount = 0;
uintptr_t         g_setColourSlot = 0;

// Replaces the five `call [colormapSetColor]` of gpl_flagmanBindModel (cdecl, the caller pops).
// Runs on the thread that latches the frame, before the walk that draws it reads the colours.
int __cdecl setColourWrapper(int handle, int slot, uint32_t colour) {
    if (slot >= 0 && slot < 16) {
        long     count = g_paletteCount;
        Palette* entry = nullptr;
        for (long i = 0; i < count && !entry; ++i)
            if (g_palettes[i].handle == static_cast<uint32_t>(handle)) entry = &g_palettes[i];
        if (!entry) {
            if (count >= kMaxPalettes) count = 0;
            entry = &g_palettes[count];
            entry->mask = 0;
            entry->handle = static_cast<uint32_t>(handle);
            g_paletteCount = count + 1;
        }
        entry->colours[slot] = colour;
        entry->mask |= 1u << slot;
    }
    SetColourFn original = *reinterpret_cast<SetColourFn const*>(g_setColourSlot);
    return original ? original(handle, slot, colour) : 0;
}

// The 16 colours of `handle`, when GPL has set every slot of it.
bool paletteOf(uint32_t handle, uint32_t colours[16]) {
    long count = g_paletteCount;
    for (long i = 0; i < count; ++i) {
        const Palette& entry = g_palettes[i];
        if (entry.handle != handle) continue;
        if (entry.mask != 0xFFFFu) return false;
        for (int k = 0; k < 16; ++k) colours[k] = entry.colours[k];
        return true;
    }
    return false;
}

// -- state -----------------------------------------------------------------------------------------

// Why a flagman was left to GPL. Each is logged the first time it happens.
enum LeftToGpl : int {
    kLeftCountBad,      // the flagman count is out of range
    kLeftNotMaterial,   // the XFORM's child is not a MATERIAL
    kLeftNoPalette,     // the MATERIAL has no colormap word
    kLeftReadFault,     // a read faulted
    kLeftNotBillboard,  // the frame is not a BILLBOARD
    kLeftNoObject,      // the frame's object is not baked
    kLeftNotLeaf,       // the frame is not a baked sprite leaf
    kLeftNoColours,     // GPL has not set all 16 colours of the colormap
    kLeftNoRenderer,    // the car renderer is not up for this pass
    kLeftArenaFull,     // past kMaxListsPerPass lists this pass
    kLeftKinds
};

struct State {
    bool      installTried = false;
    bool      on = false;
    uintptr_t countAddress = 0;
    uintptr_t objectsAddress = 0;
    uintptr_t nullNode = 0;
    uintptr_t modelViewSlot = 0;
    uintptr_t projectionSlot = 0;

    std::vector<baked::CarDrawList> lists;
    uint32_t                        count = 0;
    std::vector<uint32_t>           seen;  // the XFORMs reached this pass
    bool                            leftSaid[kLeftKinds] = {};
    bool                            firstLogged = false;
};

State g_flag;

template <typename T>
T at(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// A flagman left to GPL: false, for flagmanDispatch to return, logged the first time each reason
// occurs.
bool leaveToGpl(State& s, LeftToGpl why) {
    if (!s.leftSaid[why]) {
        s.leftSaid[why] = true;
        static const char* const kWhy[kLeftKinds] = {
            "the flagman count is out of range", "the XFORM's child is not a MATERIAL",
            "the MATERIAL has no colormap word", "a read faulted",
            "the frame is not a BILLBOARD", "the frame's object is not baked",
            "the frame is not a baked sprite leaf", "GPL has not set all 16 colours of its colormap",
            "the car renderer is not up for this pass", "past the lists a pass holds"};
        logf("baked flagmen: a flagman left to GPL (first time) -- %s", kWhy[why]);
    }
    return false;
}

// gpl_matrixTranslateEuler(t, r): T(t) * Rz * Ry * Rx (CarWalk.cpp, poseMatrix).
void poseMatrix(const float w[6], float m[12]) {
    float cz = std::cos(w[3]), sz = std::sin(w[3]);
    float cy = std::cos(w[4]), sy = std::sin(w[4]);
    float cx = std::cos(w[5]), sx = std::sin(w[5]);
    float out[12] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, w[0],
                     sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, w[1],
                     -sy,     cy * sx,                cy * cx,                w[2]};
    std::memcpy(m, out, sizeof(out));
}

// -- reading GPL's memory --------------------------------------------------------------------------
// Everything a dispatch reads, in one guarded function that holds no C++ objects (so __try is
// legal): a track reload frees the MATERIALs, and a fault there is a refusal.

enum ReadStatus {
    kReadNotFlagman = 0,
    kReadFlagman,
    kReadCountBad,
    kReadNotMaterial,
    kReadNoPalette,
    kReadFault,
};

struct FlagmanRead {
    uint32_t material = 0;
    uint32_t frame = 0;      // the MATERIAL's word 1, live
    int32_t  frameType = -1;  // its type word; negative draws nothing
    uint32_t colour = 0;     // a BILLBOARD frame's colour word
    uint32_t palette = 0;    // the colormap handle, the MATERIAL's word 3
    uint8_t  kind = 0;
    float    pose[6] = {};   // the XFORM's words 1..6
    float    modelView[16] = {};
    float    projection[16] = {};
};

int readFlagman(const State& s, uintptr_t xform, FlagmanRead& out) {
    __try {
        uint32_t child = *reinterpret_cast<const uint32_t*>(xform + 32);
        int      count = *reinterpret_cast<const int*>(s.countAddress);
        if (count < 0 || count > kMaxFlagmen) return kReadCountBad;
        auto     objects = reinterpret_cast<const uint32_t*>(s.objectsAddress);
        uint32_t flagman = 0;
        for (int i = 0; i < count && !flagman && child; ++i) {
            uint32_t object = objects[i];
            uint32_t material = 0;
            if (object)
                std::memcpy(&material, reinterpret_cast<const void*>(object + kFlagmanMaterial),
                            sizeof(material));
            if (object && material == child) flagman = object;
        }
        if (!flagman) return kReadNotFlagman;
        out.material = child;
        out.kind = *reinterpret_cast<const uint8_t*>(flagman + kFlagmanKind);
        auto m = reinterpret_cast<const uint32_t*>(child);
        if ((m[0] & 0xFF) != kNodeMaterial) return kReadNotMaterial;
        // gpl_materialNewWithPalette(&m, 0, 0, 1) makes flags 2: no texture, list or slot word
        // ahead of the colormap, which is then word 3 (gpl_materialTextureWordsBytes 0x482650).
        if ((m[2] & 0x33) != 2) return kReadNoPalette;
        out.palette = m[3];
        out.frame = m[1];
        out.frameType = -1;
        if (out.frame && out.frame != s.nullNode) {
            out.frameType = *reinterpret_cast<const int32_t*>(out.frame);
            if (out.frameType >= 0 && (out.frameType & 0xFF) == kNodeBillboard)
                out.colour = reinterpret_cast<const uint32_t*>(out.frame)[2];
        }
        std::memcpy(out.pose, reinterpret_cast<const void*>(xform + 4), sizeof(out.pose));
        std::memcpy(out.modelView,
                    *reinterpret_cast<const float* const*>(s.modelViewSlot), sizeof(out.modelView));
        std::memcpy(out.projection,
                    *reinterpret_cast<const float* const*>(s.projectionSlot),
                    sizeof(out.projection));
        return kReadFlagman;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kReadFault;
    }
}

// The operand at `site + 1` when the byte at `site` is `opcode`, else 0.
uintptr_t operandAfter(uintptr_t site, unsigned char opcode) {
    if (at<uint8_t>(site) != opcode) return 0;
    return at<uint32_t>(site + 1);
}

void logFirst(const FlagmanRead& read, const baked::CarObject& object, uint32_t frameOffset,
              const uint32_t colours[16]) {
    char   palette[16 * 9 + 1] = {};
    size_t used = 0;
    for (int k = 0; k < 16; ++k)
        used += static_cast<size_t>(std::snprintf(palette + used, sizeof(palette) - used, "%s%08X",
                                                  k ? " " : "", colours[k]));
    logf("baked flagmen: first flagman drawn by gplvr -- kind %u, MATERIAL %08X, frame BILLBOARD "
         "%08X (PRIM +0x%X of %s), colour word %08X, at (%.1f, %.1f, %.1f) yaw %.3f; colormap "
         "%08X as GPL set it, slots 0..15 ARGB: %s",
         read.kind, read.material, read.frame, frameOffset,
         object.name.empty() ? "?" : object.name.c_str(), read.colour, read.pose[0], read.pose[1],
         read.pose[2], read.pose[3], read.palette, palette);
}

}  // namespace

void flagmenInstall() {
    State& s = g_flag;
    if (s.installTried) return;
    s.installTried = true;
    if (!carBakeBind()) {
        logf("baked flagmen: NOT on -- the car bake is not bound; GPL draws the flagmen");
        return;
    }
    s.modelViewSlot = rebase(kModelViewSlot);
    s.projectionSlot = rebase(kProjectionSlot);
    uintptr_t slot = rebase(kColormapSetColor);
    if (!s.modelViewSlot || !s.projectionSlot || !slot) {
        logf("baked flagmen: NOT on -- rebase refused one of the game's addresses (%s); GPL draws "
             "the flagmen",
             rebaseSummary());
        return;
    }

    // Is this the code report 105 section 4.11 read?
    if (std::memcmp(reinterpret_cast<const void*>(kBindModel), kBindModelStart,
                    sizeof(kBindModelStart)) != 0) {
        logf("baked flagmen: NOT on -- gpl_flagmanBindModel %08X does not start as read; GPL "
             "draws the flagmen",
             static_cast<unsigned>(kBindModel));
        return;
    }
    uintptr_t count[2] = {operandAfter(kCountLoads[0], 0xA1), operandAfter(kCountLoads[1], 0xA1)};
    uintptr_t objects[2] = {operandAfter(kObjectsLoads[0], kObjectsOpcodes[0]),
                            operandAfter(kObjectsLoads[1], kObjectsOpcodes[1])};
    if (!count[0] || count[0] != count[1] || !objects[0] || objects[0] != objects[1]) {
        logf("baked flagmen: NOT on -- gpl_flagmanBindAllModels names the flagman count %08X / "
             "%08X and the flagman array %08X / %08X (each pair must agree); GPL draws the "
             "flagmen",
             static_cast<unsigned>(count[0]), static_cast<unsigned>(count[1]),
             static_cast<unsigned>(objects[0]), static_cast<unsigned>(objects[1]));
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kNullNodeStore), kNullNodeOpcode,
                    sizeof(kNullNodeOpcode)) != 0) {
        logf("baked flagmen: NOT on -- kind 0's store of gpl_nullNode at %08X is not C7 43 04; "
             "GPL draws the flagmen",
             static_cast<unsigned>(kNullNodeStore));
        return;
    }
    s.countAddress = count[0];
    s.objectsAddress = objects[0];
    s.nullNode = at<uint32_t>(kNullNodeStore + sizeof(kNullNodeOpcode));

    // Each site must be `call [colormapSetColor]` naming this image's slot.
    unsigned char stock[6] = {0xFF, 0x15, 0, 0, 0, 0};
    auto          slotWord = static_cast<uint32_t>(slot);
    std::memcpy(stock + 2, &slotWord, sizeof(slotWord));
    for (uintptr_t site : kSetColourSites) {
        if (std::memcmp(reinterpret_cast<const void*>(site), stock, sizeof(stock)) == 0) continue;
        logf("baked flagmen: NOT on -- the colormapSetColor call at %08X is not FF 15 %08X; GPL "
             "draws the flagmen",
             static_cast<unsigned>(site), static_cast<unsigned>(slot));
        return;
    }
    g_setColourSlot = slot;
    size_t patched = 0;
    for (; patched < kSiteCount; ++patched) {
        uintptr_t     site = kSetColourSites[patched];
        unsigned char call[6] = {0xE8, 0, 0, 0, 0, 0x90};
        auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&setColourWrapper) - (site + 5));
        std::memcpy(call + 1, &rel, sizeof(rel));
        if (!patchBytes(reinterpret_cast<void*>(site), call, sizeof(call))) break;
    }
    if (patched != kSiteCount) {
        // Put back what was written, so GPL's five calls are all stock or all ours.
        for (size_t undo = 0; undo < patched; ++undo)
            patchBytes(reinterpret_cast<void*>(kSetColourSites[undo]), stock, sizeof(stock));
        logf("baked flagmen: NOT on -- could not write the colormapSetColor call at %08X; GPL "
             "draws the flagmen",
             static_cast<unsigned>(kSetColourSites[patched]));
        return;
    }
    s.lists.reserve(kMaxListsPerPass);
    s.seen.reserve(kMaxListsPerPass);
    s.on = true;
    logf("baked flagmen: on -- colormapSetColor (slot %08X) wrapped at the 5 calls of "
         "gpl_flagmanBindModel %08X; flagman count %08X, flagman array %08X, gpl_nullNode %08X. "
         "In a baked main pass gplvr draws each flagman's sprite with the 16 colours GPL set, at "
         "its place in GPL's walk; anything it cannot resolve stays GPL's",
         static_cast<unsigned>(slot), static_cast<unsigned>(kBindModel),
         static_cast<unsigned>(s.countAddress), static_cast<unsigned>(s.objectsAddress),
         static_cast<unsigned>(s.nullNode));
}

void flagmenPassBegin() {
    g_flag.count = 0;
    g_flag.seen.clear();
}

const baked::CarDrawList* flagmenPassLists(uint32_t& count) {
    count = g_flag.count;
    return count ? g_flag.lists.data() : nullptr;
}

bool flagmanDispatch(const unsigned char* node, void*) {
    State& s = g_flag;
    if (!s.on) return false;
    auto        xform = reinterpret_cast<uintptr_t>(node);
    FlagmanRead read;
    int         status = readFlagman(s, xform, read);
    if (status == kReadNotFlagman) return false;
    switch (status) {
    case kReadFlagman: break;
    case kReadCountBad: return leaveToGpl(s, kLeftCountBad);
    case kReadNotMaterial: return leaveToGpl(s, kLeftNotMaterial);
    case kReadNoPalette: return leaveToGpl(s, kLeftNoPalette);
    default: return leaveToGpl(s, kLeftReadFault);
    }
    // A flagman reached again in the same pass is drawn again, as GPL does.
    bool repeat = false;
    for (uint32_t seen : s.seen) repeat = repeat || seen == xform;
    if (!repeat && s.seen.size() < kMaxListsPerPass) s.seen.push_back(static_cast<uint32_t>(xform));

    // What gplvr draws of it: nothing for the null frame (kind 0) or another negative type word,
    // else the frame's baked sprite leaf in that flagman's colours.
    bool                   nothing = read.frameType < 0;
    int                    refusal = -1;
    int32_t                object = -1;
    uint32_t               frameOffset = 0;
    baked::CarLeaf         leaf;
    uint32_t               colours[16] = {};
    const baked::CarScene& scene = carBakeScene();
    if (!nothing) {
        uintptr_t prim = 0;
        uint32_t  primSize = 0;
        if ((read.frameType & 0xFF) != kNodeBillboard) {
            refusal = kLeftNotBillboard;
        } else if ((object = carBakeObjectOf(read.frame, prim, primSize)) < 0 ||
                   static_cast<size_t>(object) >= scene.objects.size()) {
            refusal = kLeftNoObject;
        } else {
            const baked::CarObject& frameObject = scene.objects[static_cast<size_t>(object)];
            frameOffset = static_cast<uint32_t>(read.frame - prim);
            uint32_t index = frameObject.leafAt(frameOffset);
            if (index != baked::kNoCarLeaf) leaf = frameObject.leaves[index];
            if (index == baked::kNoCarLeaf || !(leaf.flags & baked::kCarLeafSprite) ||
                (leaf.flags & baked::kCarLeafNoGeometry))
                refusal = kLeftNotLeaf;
            else if (!paletteOf(read.palette, colours))
                refusal = kLeftNoColours;
        }
    }

    if (refusal >= 0) return leaveToGpl(s, static_cast<LeftToGpl>(refusal));
    if (!bakedPassDrawsCars()) return leaveToGpl(s, kLeftNoRenderer);
    if (nothing) return true;
    if (s.count >= kMaxListsPerPass) return leaveToGpl(s, kLeftArenaFull);

    if (s.lists.size() <= s.count) s.lists.emplace_back();
    baked::CarDrawList& list = s.lists[s.count++];
    list.clear();
    list.driver = -1;
    list.cockpit = false;
    list.mirror = bakedPassMirror();
    std::memcpy(list.rootModelView, read.modelView, sizeof(list.rootModelView));
    std::memcpy(list.projection, read.projection, sizeof(list.projection));
    std::memset(list.light, 0, sizeof(list.light));
    list.walkSlot = bakedPassWalkPosition();
    list.flagman = true;
    std::memcpy(list.palette, colours, sizeof(list.palette));
    // One frame: the XFORM's pose below the frame GPL dispatched it in, whose model-view the
    // list holds. The sprite turns about its local +Z, as GPL's vertical-axis drawer does.
    baked::CarFrame frame{};
    poseMatrix(read.pose, frame.rootFromLocal);
    list.frames.push_back(frame);
    list.materials.push_back(baked::CarMaterial{});
    baked::appendCarLeaf(list, static_cast<uint32_t>(object), leaf, 0, 0, read.colour);
    if (!s.firstLogged) {
        s.firstLogged = true;
        logFirst(read, scene.objects[static_cast<size_t>(object)], frameOffset, colours);
    }
    return true;
}

}  // namespace gplvr
