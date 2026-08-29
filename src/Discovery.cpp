#include "Discovery.h"

#include "Log.h"

#include <algorithm>
#include <cstring>

namespace gplvr {
namespace {

// Instruction patterns are only meaningful inside executable sections; matching
// them in data would produce false call sites.
std::vector<const uint8_t*> findAllCode(const ModuleImage& img, const void* pattern,
                                        size_t length) {
    std::vector<const uint8_t*> hits;
    for (const Section& section : img.sections) {
        if (!section.executable || !section.hasRawData) continue;
        auto found = findAll(section.begin, section.end, pattern, length);
        hits.insert(hits.end(), found.begin(), found.end());
    }
    return hits;
}

// Returns the section containing `p`, or nullptr.
const Section* sectionOf(const ModuleImage& img, const uint8_t* p) {
    for (const Section& section : img.sections)
        if (p >= section.begin && p < section.end) return &section;
    return nullptr;
}

// The loader resolves each entry with GetProcAddress and stores the result in a
// global. Find `push <nameString>` and take the first `mov [imm32], eax` after it
// that names a writable location.
void** findRasterGlobal(const ModuleImage& img, const char* name) {
    const char* text = findString(img, name);
    if (!text) return nullptr;

    uint8_t push[5] = {0x68};
    auto address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(text));
    std::memcpy(push + 1, &address, sizeof(address));

    for (const uint8_t* site : findAllCode(img, push, sizeof(push))) {
        const Section* section = sectionOf(img, site);
        if (!section) continue;
        const uint8_t* limit = site + 64;
        if (limit > section->end - 5) limit = section->end - 5;
        for (const uint8_t* p = site + 5; p < limit; ++p) {
            if (*p != 0xA3) continue;  // mov [imm32], eax
            uint32_t global = 0;
            std::memcpy(&global, p + 1, sizeof(global));
            auto candidate = reinterpret_cast<void**>(static_cast<uintptr_t>(global));
            if (isWritable(img, candidate)) return candidate;
        }
    }
    return nullptr;
}

// Target of the first near call inside [begin, end) whose destination is a known
// function entry. Requiring the destination to be in `entries` is what makes this safe:
// a stray 0xE8 byte inside some other instruction's operand almost never decodes to a
// real function.
uintptr_t firstCallTarget(const ModuleImage& img, const std::vector<uintptr_t>& entries,
                          uintptr_t begin, uintptr_t end) {
    auto p = reinterpret_cast<const uint8_t*>(begin);
    auto limit = reinterpret_cast<const uint8_t*>(end);
    if (limit > img.textEnd) limit = img.textEnd;
    if (limit < p + 5) return 0;

    for (; p <= limit - 5; ++p) {
        if (*p != 0xE8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        const uintptr_t target = reinterpret_cast<uintptr_t>(p) + 5 + rel;
        if (std::binary_search(entries.begin(), entries.end(), target)) return target;
    }
    return 0;
}

std::vector<uintptr_t> findIndirectCallSites(const ModuleImage& img, void** global) {
    uint8_t call[6] = {0xFF, 0x15};
    auto address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(global));
    std::memcpy(call + 2, &address, sizeof(address));

    std::vector<uintptr_t> sites;
    for (const uint8_t* p : findAllCode(img, call, sizeof(call)))
        sites.push_back(reinterpret_cast<uintptr_t>(p));
    return sites;
}


}  // namespace

bool discoverRasterTable(const ModuleImage& image, RasterTable& out) {
    out.screenDisplay = findRasterGlobal(image, "screenDisplay");
    out.screenGetBackBuffer = findRasterGlobal(image, "screenGetBackBuffer");
    out.polySetDest = findRasterGlobal(image, "polySetDest");
    out.polyDraw = findRasterGlobal(image, "polyDraw");
    out.bitmapInstance = findRasterGlobal(image, "bitmapInstance");
    out.bitmapWidth = findRasterGlobal(image, "bitmapWidth");
    out.bitmapHeight = findRasterGlobal(image, "bitmapHeight");
    out.bitmapInstanceAlloc = findRasterGlobal(image, "bitmapInstanceAlloc");
    out.bitmapInstanceFree = findRasterGlobal(image, "bitmapInstanceFree");

    // The globals exist from startup but hold null until a rasterizer DLL has been
    // loaded and its exports resolved. Treat "populated" as "ready".
    return out.screenDisplay && out.polySetDest && *out.screenDisplay && *out.polySetDest;
}

bool discover(Binding& out) {
    if (!getModuleImage(GetModuleHandleA(nullptr), out.image)) {
        logf("discover: could not read the main module's headers");
        return false;
    }
    logf("discover: image %p..%p, .text %p..%p", out.image.base,
         out.image.base + out.image.size, out.image.textBegin, out.image.textEnd);

    if (!discoverRasterTable(out.image, out.raster)) {
        logf("discover: rasterizer table not resolved yet");
        return false;
    }
    logf("discover: screenDisplay global %p -> %p", out.raster.screenDisplay,
         *out.raster.screenDisplay);
    logf("discover: polySetDest   global %p -> %p", out.raster.polySetDest,
         *out.raster.polySetDest);
    logf("discover: polyDraw      global %p", out.raster.polyDraw);
    logf("discover: backBuffer    global %p", out.raster.screenGetBackBuffer);
    logf("discover: bitmapInstance global %p", out.raster.bitmapInstance);
    logf("discover: bitmapWidth    global %p, bitmapHeight %p", out.raster.bitmapWidth,
         out.raster.bitmapHeight);
    logf("discover: instanceAlloc  global %p, instanceFree %p",
         out.raster.bitmapInstanceAlloc, out.raster.bitmapInstanceFree);

    // -- cameraBind and sceneRender ------------------------------------------
    // polySetDest is reached through a small wrapper, so cameraBind is NOT the
    // function containing the call -- it is the function that CALLS that wrapper and
    // also computes tan(fov * 0.5). Two independent signals, intersected:
    //
    //   A. functions containing D9 F2 (fptan)
    //   B. functions that are, or call, the containing function of a polySetDest call
    //
    // and then disambiguated by C: exactly one function calls the survivor twice --
    // sceneRender, doing the mirror pass and then the main pass.
    std::vector<uintptr_t> polySetDestSites =
        findIndirectCallSites(out.image, out.raster.polySetDest);
    logf("discover: polySetDest call sites: %zu", polySetDestSites.size());
    if (polySetDestSites.empty()) {
        logf("discover: FAILED -- no polySetDest call sites");
        return false;
    }

    const std::vector<uintptr_t> entries = collectCallTargets(out.image);
    logf("discover: %zu candidate function entry points in .text", entries.size());

    // A -- every function containing fptan
    std::vector<uintptr_t> fptanFunctions;
    {
        const uint8_t fptan[2] = {0xD9, 0xF2};
        for (const uint8_t* p : findAll(out.image.textBegin, out.image.textEnd, fptan,
                                        sizeof(fptan))) {
            const uintptr_t owner = functionOf(entries, reinterpret_cast<uintptr_t>(p));
            if (owner && std::find(fptanFunctions.begin(), fptanFunctions.end(), owner) ==
                             fptanFunctions.end())
                fptanFunctions.push_back(owner);
        }
    }
    logf("discover: %zu functions contain fptan", fptanFunctions.size());

    // B -- functions related to a polySetDest call
    std::vector<uintptr_t> related;
    for (uintptr_t site : polySetDestSites) {
        const uintptr_t owner = functionOf(entries, site);
        if (!owner) continue;
        logf("discover:   polySetDest site %08X lies in function %08X..%08X",
             static_cast<unsigned>(site), static_cast<unsigned>(owner),
             static_cast<unsigned>(functionEnd(entries, out.image, owner)));
        related.push_back(owner);
        for (uintptr_t callSite : findCallSites(out.image, owner)) {
            const uintptr_t caller = functionOf(entries, callSite);
            if (caller) related.push_back(caller);
        }
    }

    // A n B, then C
    for (uintptr_t candidate : fptanFunctions) {
        if (std::find(related.begin(), related.end(), candidate) == related.end()) continue;

        std::vector<uintptr_t> bindSites = findCallSites(out.image, candidate);
        uintptr_t twiceCaller = 0;
        int twiceCount = 0;
        for (uintptr_t entry : entries) {
            const uintptr_t end = functionEnd(entries, out.image, entry);
            int calls = 0;
            for (uintptr_t site : bindSites)
                if (site >= entry && site < end) ++calls;
            if (calls >= 2) {
                twiceCaller = entry;
                ++twiceCount;
            }
        }
        logf("discover:   fptan function %08X is polySetDest-related, %zu callers, "
             "%d of them call it twice",
             static_cast<unsigned>(candidate), bindSites.size(), twiceCount);
        if (twiceCount != 1) continue;

        if (out.cameraBind) {
            logf("discover: FAILED -- more than one function satisfies all three tests");
            return false;
        }
        out.cameraBind = candidate;
        out.sceneRender = twiceCaller;
    }

    if (!out.cameraBind) {
        logf("discover: FAILED -- could not identify cameraBind");
        return false;
    }
    logf("discover: cameraBind  = %08X", static_cast<unsigned>(out.cameraBind));
    logf("discover: sceneRender = %08X", static_cast<unsigned>(out.sceneRender));

    // -- cameraCreate --------------------------------------------------------
    // cameraBind's very first call is to rebuildProjection, guarded by the dirty byte
    // at camera+0x3C. rebuildProjection is called from exactly two places in the whole
    // image: cameraBind, which we already have, and cameraCreate, which we want.
    //
    // Non-fatal on purpose. Stereo is the only thing that needs cameraCreate; if this
    // does not come out cleanly we would rather run without stereo than guess.
    {
        const uintptr_t bindEnd = functionEnd(entries, out.image, out.cameraBind);
        out.cameraRebuildProjection =
            firstCallTarget(out.image, entries, out.cameraBind, bindEnd);
        if (!out.cameraRebuildProjection) {
            logf("discover: no call found inside cameraBind -- stereo unavailable");
        } else {
            logf("discover: cameraRebuildProjection = %08X",
                 static_cast<unsigned>(out.cameraRebuildProjection));

            std::vector<uintptr_t> callers;
            for (uintptr_t site : findCallSites(out.image, out.cameraRebuildProjection)) {
                const uintptr_t owner = functionOf(entries, site);
                if (owner && std::find(callers.begin(), callers.end(), owner) ==
                                 callers.end())
                    callers.push_back(owner);
            }
            logf("discover: rebuildProjection has %zu distinct callers", callers.size());

            if (callers.size() != 2) {
                logf("discover: expected exactly 2 (cameraBind and cameraCreate) -- "
                     "stereo unavailable");
            } else if (std::find(callers.begin(), callers.end(), out.cameraBind) ==
                       callers.end()) {
                logf("discover: cameraBind is not among them -- stereo unavailable");
            } else {
                out.cameraCreate =
                    (callers[0] == out.cameraBind) ? callers[1] : callers[0];
                logf("discover: cameraCreate = %08X",
                     static_cast<unsigned>(out.cameraCreate));
            }
        }
    }

    // -- call sites and the caller ------------------------------------------
    out.sceneRenderCallSites = findCallSites(out.image, out.sceneRender);
    logf("discover: sceneRender call sites: %zu", out.sceneRenderCallSites.size());
    if (out.sceneRenderCallSites.empty()) {
        logf("discover: FAILED -- sceneRender is never called directly");
        return false;
    }
    for (uintptr_t site : out.sceneRenderCallSites) {
        logf("discover:   call at %08X", static_cast<unsigned>(site));
        const uintptr_t owner = functionOf(entries, site);
        if (!out.simFrameRender) out.simFrameRender = owner;
        else if (out.simFrameRender != owner)
            logf("discover:   note -- sceneRender has more than one calling function");
    }
    logf("discover: simFrameRender = %08X", static_cast<unsigned>(out.simFrameRender));

    // The cameraBind calls inside sceneRender. Exactly two -- the mirror pass and the
    // main pass -- which is the same cardinality that identified sceneRender in the
    // first place, so anything else means the shape assumed here no longer holds.
    if (out.sceneRender && out.cameraBind) {
        const uintptr_t sceneEnd = functionEnd(entries, out.image, out.sceneRender);
        for (uintptr_t site : findCallSites(out.image, out.cameraBind))
            if (site >= out.sceneRender && site < sceneEnd)
                out.cameraBindCallSitesInScene.push_back(site);

        logf("discover: cameraBind call sites inside sceneRender: %zu",
             out.cameraBindCallSitesInScene.size());
        for (uintptr_t site : out.cameraBindCallSitesInScene)
            logf("discover:   bind at %08X", static_cast<unsigned>(site));

        if (out.cameraBindCallSitesInScene.size() != 2) {
            logf("discover: WARNING -- expected 2, so the eye frustum fix stays OFF and "
                 "the eyes will render the game's own field of view");
            out.cameraBindCallSitesInScene.clear();
        }
    }

    logf("discover: OK");
    return true;
}

}  // namespace gplvr
