#include "baked_gpl/CarWalkStats.h"

#include "Log.h"

#include "baked_gpl/BakedPass.h"
#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarProgram.h"
#include "baked_gpl/CarShare.h"
#include "baked_gpl/CockpitDraw.h"

#include <windows.h>

#include <intrin.h>

#include <cstdint>
#include <cstdio>

namespace gplvr {
namespace carwalk {
namespace {

// One line a second for every walk of the second: which leaves GPL's memo skinned, and every way
// the emulation could not follow it.
void logSkin(const char* what, const SkinCounts& k) {
    logf("baked cars: skinning (%s) -- %u leaves skinned (1B %u, 1C %u, 1D %u, 1E %u, 1F %u, 20 "
         "%u, 21 %u, 22 %u), %u corners and %u normals from another frame, %u corners and %u "
         "normals GPL memoised before the walk, %u dynamic vertices; memo full: vertices %u, "
         "normals %u; frames past %u %u; memo words not a live slot %u; polygons over %u corners "
         "%u; skinned polygons without baked corners %u",
         what, k.leaves, k.byType[0x1B], k.byType[0x1C], k.byType[0x1D], k.byType[0x1E],
         k.byType[0x1F], k.byType[0x20], k.byType[0x21], k.byType[0x22], k.corners, k.normals,
         k.liveCorners, k.liveNormals, k.dynamicVertices, k.vertexOverflow, k.normalOverflow,
         static_cast<unsigned>(kMaxWalkFrames), k.frameOverflow, k.staleWords,
         kMaxSkinCorners, k.tooManyCorners, k.noBakedCorners);
}

// GPL's own work in the baked passes, timed (report 105 section 4.44), in ms of CPU over the
// second: the cockpit GPL draws, GPL's car draw with the body skipped, and the node filter. The
// filter's TSC is calibrated against QueryPerformanceCounter over the second; the first second
// has no rate.
void logGplOwn(const Second& t, int64_t qpcNow, int64_t qpcFrequency) {
    static uint64_t lastTsc = 0;
    static int64_t  lastQpc = 0;
    uint64_t        tsc = __rdtsc();
    uint64_t        filterCalls[kBakedTimeKinds] = {}, filterTicks[kBakedTimeKinds] = {};
    bakedFilterTake(filterCalls, filterTicks);
    int64_t  windowTicks[2] = {};
    uint32_t windowPasses[2] = {};
    bakedPassWindowsTake(windowTicks, windowPasses);
    double tscPerMs = lastQpc && qpcNow > lastQpc
                          ? static_cast<double>(tsc - lastTsc) * 1000.0 /
                                (static_cast<double>(qpcNow - lastQpc) / qpcFrequency * 1e6)
                          : 0.0;
    lastTsc = tsc;
    lastQpc = qpcNow;
    if (tscPerMs <= 0.0) return;
    double ms = 1000.0 / static_cast<double>(qpcFrequency);
    const OursSecond& o = t.cars;
    const OursSecond& m = t.mirrorCars;
    uint64_t calls = 0;
    double   filterMs = 0.0;
    double   filterKind[kBakedTimeKinds];
    for (int kind = 0; kind < kBakedTimeKinds; ++kind) {
        filterKind[kind] = static_cast<double>(filterTicks[kind]) / tscPerMs;
        filterMs += filterKind[kind];
        calls += filterCalls[kind];
    }
    // Report 105 section 4.46: each walk window less the car calls and the filter inside it is
    // GPL's own walk (segments, cells, lists, plane nodes, box culls, selectors, sort groups).
    double window[2], carCalls[2], walk[2];
    for (int kind = 0; kind < 2; ++kind) {
        window[kind] = static_cast<double>(windowTicks[kind]) * ms;
        carCalls[kind] = static_cast<double>(t.callTicks[kind]) * ms;
        walk[kind] = window[kind] - carCalls[kind] - filterKind[kind];
    }
    logf("baked: GPL's walk windows (report 105 section 4.46), ms a second -- main passes %u: "
         "window %.2f = car calls %.2f (%u) + node filter %.2f + GPL's own walk %.2f; mirror "
         "passes %u: window %.2f = car calls %.2f (%u) + node filter %.2f + GPL's own walk %.2f; "
         "outside the windows: car calls %.2f (%u), node filter %.2f",
         windowPasses[0], window[0], carCalls[0], t.callCount[kBakedMain], filterKind[kBakedMain],
         walk[0], windowPasses[1], window[1], carCalls[1], t.callCount[kBakedMirror],
         filterKind[kBakedMirror], walk[1],
         static_cast<double>(t.callTicks[kBakedOutside]) * ms, t.callCount[kBakedOutside],
         filterKind[kBakedOutside]);
    logf("baked: GPL's own work timed (report 105 section 4.44), ms a second -- the cockpit GPL "
         "draws %.2f (%u calls, %.0f us each); GPL's car draw with the body skipped: main passes "
         "%.2f (%u calls, %.1f us each), mirror passes %.2f (%u calls, %.1f us each); the node "
         "filter ~%.2f (%llu calls, %.0f ns each, 1 in 16 timed); TSC %.0f MHz",
         static_cast<double>(t.cockpitTicks) * ms, t.cockpitCalls,
         t.cockpitCalls ? static_cast<double>(t.cockpitTicks) * ms * 1000.0 / t.cockpitCalls : 0.0,
         static_cast<double>(o.originalTicks) * ms, o.originalCalls,
         o.originalCalls ? static_cast<double>(o.originalTicks) * ms * 1000.0 / o.originalCalls
                         : 0.0,
         static_cast<double>(m.originalTicks) * ms, m.originalCalls,
         m.originalCalls ? static_cast<double>(m.originalTicks) * ms * 1000.0 / m.originalCalls
                         : 0.0,
         filterMs, static_cast<unsigned long long>(calls),
         calls ? filterMs * 1e6 / static_cast<double>(calls) : 0.0, tscPerMs / 1000.0);
}

// The cockpit gplvr draws while GPL draws its mirror glass (report 105 section 4.48): its lists,
// GPL's glass-only draws, and every call GPL drew whole, by why.
void logCockpit(const OursSecond& k, const CockpitSecond& c, int64_t qpcFrequency) {
    uint32_t whole = 0;
    for (uint32_t n : c.whole) whole += n;
    double us = 1e6 / static_cast<double>(qpcFrequency);
    logf("baked cars: the cockpit (report 105 section 4.48) -- %u calls: %u lists walked (%.0f us "
         "each), %u taken from the first eye and %u from its kept list (section 4.50), %u calls "
         "deduped; list contract MATCHED on %u of "
         "%u (leaves %llu + glass left to GPL %llu of the walk's %llu); GPL's glass-only draws %u "
         "(%.0f us each): glass leaves handed to GPL %llu, other leaves dropped %llu, lines "
         "handed to GPL %llu, GPL's polygons %llu (contract: the glass handed over, less any its "
         "clipper drops whole); glass nodes known %u; drawn WHOLE by GPL %u: not a baked main "
         "pass %u, not the main register file %u, node filter blind to the lit polygon types %u, "
         "mirror.mip not known %u, past %u lists %u, walk faulted %u, list refused %u (refused "
         "leaves: no object %u, not a baked leaf %u, type differs %u, overflow %u, slot with no "
         "list %u, glass past the set's room %u, step 2's glass without live corners %u)",
         c.calls, k.lists, k.lists ? us * static_cast<double>(k.walkTicks) / k.lists : 0.0,
         c.taken, c.kept, k.deduped, k.matched, k.lists, static_cast<unsigned long long>(k.leaves),
         static_cast<unsigned long long>(k.glass), static_cast<unsigned long long>(k.walked),
         c.glassCalls, c.glassCalls ? us * static_cast<double>(c.glassTicks) / c.glassCalls : 0.0,
         static_cast<unsigned long long>(c.glassPassed),
         static_cast<unsigned long long>(c.dropped), static_cast<unsigned long long>(c.lines),
         static_cast<unsigned long long>(c.gplPolygons), c.glassNodes, whole,
         c.whole[kCockpitNotBaked], c.whole[kCockpitNotMainFile], c.whole[kCockpitFilter],
         c.whole[kCockpitNoMirrorMip], kMaxListsPerPass, c.whole[kCockpitArenaFull],
         c.whole[kCockpitFaulted], c.whole[kCockpitRefused], k.emit.noObject, k.emit.notLeaf,
         k.emit.typeDiffers, k.emit.overflow, k.emit.slotNoList, k.emit.glassOverflow,
         k.emit.glassRefused);
    if (c.overlayLists || c.stepOne || c.overlayCalls)
        logf("baked cars: the cockpit, step 2 (report 105 section 4.49) -- lists made to be drawn "
             "over the eye image %u (walked, taken or kept; glass the walked ones listed with "
             "GPL's live UVs %u), lists for step 1 instead %u; "
             "GPL's draws handing it no leaf %u (%.0f us each): leaves dropped %llu, leaves "
             "handed to GPL %llu (must be 0), lines handed to GPL %llu, GPL's polygons %llu (must "
             "be 0)",
             c.overlayLists, k.emit.glassListed, c.stepOne, c.overlayCalls,
             c.overlayCalls ? us * static_cast<double>(c.overlayTicks) / c.overlayCalls : 0.0,
             static_cast<unsigned long long>(c.overlayDropped),
             static_cast<unsigned long long>(c.overlayPassed),
             static_cast<unsigned long long>(c.overlayLines),
             static_cast<unsigned long long>(c.overlayGplPolygons));
    if (k.differed)
        logf("baked cars: the cockpit, first list this second that did not cover its walk -- "
             "leaves and glass %u vs the walk's %u",
             k.firstLeaves, k.firstWalked);
    if (k.lists) logSkin("gplvr's cockpit", k.skin);
}

}  // namespace

bool reportSecond(Second& t, int64_t& windowStart, bool stage3, uint32_t bodySkipped,
                  uint32_t bodySkippedMirror) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (!windowStart) windowStart = now.QuadPart;
    if (now.QuadPart - windowStart < frequency.QuadPart) return false;
    if (t.draws) {
        double microseconds = 1e6 * static_cast<double>(t.walkTicks) / frequency.QuadPart / t.draws;
        const WalkCounts& c = t.sums;
        logf("baked cars: %u car draws (main %u, mirror %u, cockpit %u), walked in %.1f us each -- "
             "GPL's polygon count MATCHED on %u of %u (cockpit %u of %u), %u walks faulted, %u "
             "draws left out by debug view 6; "
             "polygons ours %llu vs GPL %llu, blob shadow %llu counted apart; per draw: %.0f "
             "control nodes, culled by box %.2f, by sphere %.2f, by hidden class %.2f; op-5 reads "
             "past the cases %u, clock selectors %u, unknown node types %u, depth limit %u, LINE "
             "nodes %u (not in GPL's count)",
             t.draws, t.mainDraws, t.mirrorDraws, t.cockpitDraws, microseconds, t.matched,
             t.draws - t.walkFailed - t.leftOut, t.cockpitMatched, t.cockpitDraws, t.walkFailed,
             t.leftOut,
             static_cast<unsigned long long>(t.ours), static_cast<unsigned long long>(t.gpl),
             static_cast<unsigned long long>(t.shadow), static_cast<double>(c.controls) / t.draws,
             static_cast<double>(c.boxCulled) / t.draws,
             static_cast<double>(c.sphereCulled) / t.draws,
             static_cast<double>(c.classHidden) / t.draws, c.indexOutOfRange, c.clockSelectors,
             c.unknownTypes, c.depthLimit, c.lines);
        if (t.differed) {
            const WalkCounts& f = t.first;
            logf("baked cars: first difference this second -- driver %d, instance %p, %s, %s pass, "
                 "user planes %02X: ours %u (sprites %u) vs GPL %u; that walk culled by box %u, by "
                 "sphere %u, by hidden class %u, op-5 past the cases %u",
                 t.driver, t.instance, t.cockpit ? "cockpit" : "car", t.mirror ? "mirror" : "main",
                 t.planeMask, f.polygons, f.sprites, t.firstGpl, f.boxCulled, f.sphereCulled,
                 f.classHidden, f.indexOutOfRange);
        }
    }
    const EmitSecond& e = t.emit;
    if (e.draws) {
        const EmitCounts&    c = e.counts;
        const CarBakeTotals& b = carBakeTotals();
        double               draws = static_cast<double>(e.draws);
        logf("baked cars: stage 2 -- %u draw lists, leaves %llu of the walk's %llu polygons: "
             "MATCHED on %u of %u lists; per list %.1f ranges, %.1f frames, %.1f materials, %.2f "
             "leaves without geometry; refused leaves: no object %u, not a baked leaf %u, type "
             "differs %u; frame or material overflow %u, slot with no list %u, textures "
             "unresolved %u",
             e.draws, static_cast<unsigned long long>(e.leaves),
             static_cast<unsigned long long>(e.walked), e.matched, e.draws, e.ranges / draws,
             e.frames / draws, e.materials / draws, e.emptyLeaves / draws, c.noObject, c.notLeaf,
             c.typeDiffers, c.overflow, c.slotNoList, c.unresolved);
        logf("baked cars: stage 2 bake so far -- %u objects (%u failed, %u rebaked) in %.1f ms, "
             "textures %u (%u missing), sprites %u (%u missing), files read and parsed in %.1f "
             "ms; folders by the only one holding the name %u, by load order %u, still "
             "ambiguous %u; resource cache scans %u",
             b.objects, b.objectsFailed, b.objectsRebaked, b.bakeMilliseconds, b.textures,
             b.texturesMissing, b.sprites, b.spritesMissing, b.fileMilliseconds, b.folderUnique,
             b.folderByLoadOrder, b.folderAmbiguous, b.cacheScans);
        if (e.differed) {
            const EmitCounts& f = e.first;
            logf("baked cars: stage 2 first difference this second -- driver %d, %s, %s pass: "
                 "leaves %u vs the walk's %u; refused there: no object %u, not a baked leaf %u, "
                 "type differs %u",
                 e.driver, e.cockpit ? "cockpit" : "car", e.mirror ? "mirror" : "main",
                 e.firstLeaves, e.firstWalked, f.noObject, f.notLeaf, f.typeDiffers);
        }
    }
    const OursSecond& o = t.cars;
    if (stage3 && o.passedThrough)
        logf("baked cars: stage 2b -- %u draws GPL makes itself handed over UNWALKED (cockpit %u, "
             "mirror %u, main-pass calls outside a baked pass %u): the per-draw comparison with "
             "GPL's polygon counter is OFF at bakedCars = 3 (levels 1 and 2 run it)",
             o.passedThrough, o.passedCockpit, o.passedMirror, o.passedOther);
    if (stage3 && o.profiled && o.ticks.total) {
        double total = static_cast<double>(o.ticks.total);
        logf("baked cars: stage 2b walk by parts, %u walks sampled (1 in %u) -- memo (corners, "
             "normals, control-node vertices) %.0f%%, leaf emission %.0f%% (of it building "
             "skinned leaves %.0f%%, finding the leaf's object %.0f%%), the rest (control nodes, "
             "MATERIALs, frames) %.0f%%",
             o.profiled, kProfileEvery, 100.0 * o.ticks.memo / total, 100.0 * o.ticks.emit / total,
             100.0 * o.ticks.skinned / total, 100.0 * o.ticks.object / total,
             100.0 * (total - o.ticks.memo - o.ticks.emit) / total);
    }
    if (stage3 && (o.mainInside || o.mainOutside || o.calls)) {
        const EmitCounts& c = o.emit;
        double lists = o.lists ? static_cast<double>(o.lists) : 1.0;
        logf("baked cars: stage 2b -- main-pass car calls inside a baked pass %u, OUTSIDE one %u "
             "(GPL drew those); gplvr's cars: %u calls -> %u lists (%u calls deduped: the same "
             "car in another cell; %u past %u lists a pass and %u faulted walks left to GPL, %u "
             "of those a car missing in one cell), "
             "walked in %.1f us a list, %.1f draws a list; list contract MATCHED on %u of %u "
             "(leaves %llu of the walk's %llu); GPL's body dispatch skipped %u times on %u calls, "
             "GPL polygons on gplvr's cars %llu (must be 0); %u calls left out by debug view 6; "
             "refused leaves: no object %u, not a baked leaf %u, type differs %u, overflow %u, "
             "slot with no list %u, textures unresolved %u",
             o.mainInside, o.mainOutside, o.calls, o.lists, o.deduped, o.arenaFull,
             kMaxListsPerPass, o.walkFailed, o.bodyLost,
             1e6 * static_cast<double>(o.walkTicks) / static_cast<double>(frequency.QuadPart) /
                 lists,
             static_cast<double>(o.draws) / lists, o.matched, o.lists,
             static_cast<unsigned long long>(o.leaves), static_cast<unsigned long long>(o.walked),
             bodySkipped, o.bodyCalls, static_cast<unsigned long long>(o.gplPolygons),
             o.leftOut, c.noObject, c.notLeaf, c.typeDiffers, c.overflow, c.slotNoList,
             c.unresolved);
        if (o.differed)
            logf("baked cars: stage 2b first list this second that did not cover its walk -- "
                 "driver %d: leaves %u vs the walk's %u",
                 o.driver, o.firstLeaves, o.firstWalked);
        if (o.lists) logSkin("gplvr's cars", o.skin);
    }
    const OursSecond& m = t.mirrorCars;
    if (stage3 && (m.mainInside || m.mainOutside || m.calls)) {
        double lists = m.lists ? static_cast<double>(m.lists) : 1.0;
        logf("baked cars: mirror passes -- mirror car calls inside a baked mirror pass %u, OUTSIDE "
             "one %u (GPL drew those: mirror detail 0, refused mirror walks, debug view 15); "
             "gplvr's mirror cars: %u calls -> %u lists (%u deduped, %u past %u lists a pass and "
             "%u faulted walks left to GPL, %u of those a car missing in one cell), walked in "
             "%.1f us a list, %.1f draws a list; list contract MATCHED on %u of %u (leaves %llu "
             "of the walk's %llu); GPL's body dispatch skipped %u times on %u calls, GPL "
             "polygons on gplvr's mirror cars %llu (must be 0); refused leaves: no object %u, not "
             "a baked leaf %u, type differs %u, overflow %u, slot with no list %u, textures "
             "unresolved %u",
             m.mainInside, m.mainOutside, m.calls, m.lists, m.deduped, m.arenaFull,
             kMaxListsPerPass, m.walkFailed, m.bodyLost,
             1e6 * static_cast<double>(m.walkTicks) / static_cast<double>(frequency.QuadPart) /
                 lists,
             static_cast<double>(m.draws) / lists, m.matched, m.lists,
             static_cast<unsigned long long>(m.leaves), static_cast<unsigned long long>(m.walked),
             bodySkippedMirror, m.bodyCalls, static_cast<unsigned long long>(m.gplPolygons),
             m.emit.noObject, m.emit.notLeaf, m.emit.typeDiffers, m.emit.overflow,
             m.emit.slotNoList, m.emit.unresolved);
        if (m.differed)
            logf("baked cars: mirror passes, first list this second that did not cover its walk "
                 "-- driver %d: leaves %u vs the walk's %u",
                 m.driver, m.firstLeaves, m.firstWalked);
        if (m.lists) logSkin("gplvr's mirror cars", m.skin);
    }
    if (stage3 && t.cockpitOurs.calls)
        logCockpit(t.cockpitList, t.cockpitOurs, frequency.QuadPart);
    if (stage3) cockpitGplDrawSecond();
    if (t.draws) logSkin("GPL's draws: mirrors, cockpit, stages 1 and 2", t.skin);
    if (stage3) {
        logGplOwn(t, now.QuadPart, frequency.QuadPart);
        carShareSecond();
        carProgramSecond();
    }
    t = Second{};
    windowStart = now.QuadPart;
    return true;
}

void logInstance(void* instance, int driver, bool cockpit, const WalkCounts& c,
                 const EmitCounts& emit, const SkinCounts& k, const int32_t* skinnedObject,
                 const uint32_t* skinnedLeaves, int skinnedObjects,
                 const baked::CarDrawList* list) {
    logf("baked cars: team instance %p first drawn for driver %d (%s) -- the walk met %u control "
         "nodes (%u gates, %u selectors, %u SETREG, %u transforms), %u polygons (1B %u, 1C %u, 1D "
         "%u, 1E %u, 1F %u, 20 %u, 21 %u, 22 %u), %u sprites, %u lines, %u MATERIALs; culled by "
         "box %u, by sphere %u, by hidden class %u; unknown node types %u",
         instance, driver, cockpit ? "cockpit" : "car", c.controls, c.gates, c.selectors,
         c.setRegisters, c.transforms, c.polygons - c.sprites, c.byType[0x1B], c.byType[0x1C],
         c.byType[0x1D], c.byType[0x1E], c.byType[0x1F], c.byType[0x20], c.byType[0x21],
         c.byType[0x22], c.sprites, c.lines, c.byType[0x05], c.boxCulled, c.sphereCulled,
         c.classHidden, c.unknownTypes);
    if (!list) return;
    uint32_t spriteRuns = 0;
    for (const baked::CarDraw& draw : list->draws)
        if (draw.flags & baked::kCarLeafSprite) ++spriteRuns;
    uint32_t textured = 0, unresolved = 0;
    for (const baked::CarMaterial& material : list->materials) {
        if (!material.textureHandle) continue;
        ++textured;
        if (material.texture < 0) ++unresolved;
    }
    logf("baked cars: team instance %p stage 2 -- its first draw list holds %u leaves of the "
         "walk's %u polygons (%u without geometry) in %zu ranges (%u of sprites), %zu frames, %zu "
         "materials (%u with a texture, %u of them unresolved); mirror glass left to GPL %u, "
         "listed with GPL's live UVs %u; leaves refused: no object %u, not a baked leaf %u, type "
         "differs %u",
         instance, list->polygons, c.polygons, list->emptyLeaves, list->draws.size(), spriteRuns,
         list->frames.size(), list->materials.size(), textured, unresolved, emit.glass,
         emit.glassListed, emit.noObject, emit.notLeaf, emit.typeDiffers);

    // Which objects GPL's vertex memo skins (report 105 section 4.4): in stock cars, the
    // suspension and the arms, whose 0x16 XFORMs leave their vertices memoised for the parent.
    char   objects[512];
    size_t used = 0;
    objects[0] = '\0';
    const baked::CarScene& scene = carBakeScene();
    for (int i = 0; i < skinnedObjects && used + 64 < sizeof(objects); ++i) {
        int32_t     index = skinnedObject[i];
        const char* name = index >= 0 && static_cast<size_t>(index) < scene.objects.size() &&
                                   !scene.objects[static_cast<size_t>(index)].name.empty()
                               ? scene.objects[static_cast<size_t>(index)].name.c_str()
                               : "?";
        used += static_cast<size_t>(std::snprintf(objects + used, sizeof(objects) - used,
                                                  " %.40s x %u", name, skinnedLeaves[i]));
    }
    logf("baked cars: team instance %p skinning -- %u of its %u leaves drawn from corners GPL's "
         "vertex memo holds from another frame (1B %u, 1C %u, 1D %u, 1E %u, 1F %u, 20 %u, 21 %u, "
         "22 %u; %u corners, %u normals lit in another frame, %u corners and %u normals GPL "
         "memoised before the walk), by object:%s",
         instance, k.leaves, list->polygons, k.byType[0x1B], k.byType[0x1C], k.byType[0x1D],
         k.byType[0x1E], k.byType[0x1F], k.byType[0x20], k.byType[0x21], k.byType[0x22],
         k.corners, k.normals, k.liveCorners, k.liveNormals, used ? objects : " none");
}

void addCounts(WalkCounts& sum, const WalkCounts& add) {
    sum.polygons += add.polygons;
    sum.sprites += add.sprites;
    sum.lines += add.lines;
    sum.controls += add.controls;
    sum.gates += add.gates;
    sum.selectors += add.selectors;
    sum.setRegisters += add.setRegisters;
    sum.transforms += add.transforms;
    sum.boxCulled += add.boxCulled;
    sum.sphereCulled += add.sphereCulled;
    sum.classHidden += add.classHidden;
    sum.indexOutOfRange += add.indexOutOfRange;
    sum.clockSelectors += add.clockSelectors;
    sum.unknownTypes += add.unknownTypes;
    sum.depthLimit += add.depthLimit;
    for (int i = 0; i < 0x24; ++i) sum.byType[i] += add.byType[i];
}

void addEmitCounts(EmitCounts& sum, const EmitCounts& add) {
    sum.leaves += add.leaves;
    sum.noObject += add.noObject;
    sum.notLeaf += add.notLeaf;
    sum.typeDiffers += add.typeDiffers;
    sum.overflow += add.overflow;
    sum.slotNoList += add.slotNoList;
    sum.unresolved += add.unresolved;
    sum.glass += add.glass;
    sum.glassOverflow += add.glassOverflow;
    sum.glassListed += add.glassListed;
    sum.glassRefused += add.glassRefused;
}

void addSkinCounts(SkinCounts& sum, const SkinCounts& add) {
    sum.leaves += add.leaves;
    for (int i = 0; i < 0x24; ++i) sum.byType[i] += add.byType[i];
    sum.corners += add.corners;
    sum.liveCorners += add.liveCorners;
    sum.normals += add.normals;
    sum.liveNormals += add.liveNormals;
    sum.dynamicVertices += add.dynamicVertices;
    sum.vertexOverflow += add.vertexOverflow;
    sum.normalOverflow += add.normalOverflow;
    sum.frameOverflow += add.frameOverflow;
    sum.staleWords += add.staleWords;
    sum.tooManyCorners += add.tooManyCorners;
    sum.noBakedCorners += add.noBakedCorners;
}

}  // namespace carwalk
}  // namespace gplvr
