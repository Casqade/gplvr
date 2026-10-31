#include "baked_gpl/CarWalkStats.h"

#include "Log.h"

#include "baked_gpl/CarBake.h"
#include "baked_gpl/CarProgram.h"
#include "baked_gpl/CarShare.h"

#include <windows.h>

#include <cstdint>

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
    if (t.draws) logSkin("GPL's draws: mirrors, cockpit, stages 1 and 2", t.skin);
    if (stage3) {
        carShareSecond();
        carProgramSecond();
    }
    t = Second{};
    windowStart = now.QuadPart;
    return true;
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
