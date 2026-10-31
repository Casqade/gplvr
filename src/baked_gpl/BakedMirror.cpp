#include "baked_gpl/BakedMirror.h"

#include "baked_gpl/BakedPass.h"
#include "baked_gpl/ResidualDepth.h"

#include "DataRebase.h"
#include "GlInterop.h"
#include "Hooks.h"
#include "Log.h"
#include "MenuScale.h"
#include "Pe.h"
#include "Stereo.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// -- game addresses (gplc67.exe; report 105 section 4.18, IDA 2026-10-26) ------------------------

// gpl_sceneRender's mirror walk: `call gpl_worldRenderSegmentRange`, eight __cdecl arguments
// cleaned by the caller (`add esp, 20h` at 0x487A58); its result is never read. Code, the same in
// both links.
typedef void(__cdecl* WorldWalkFn)(int, int, int, int, int, int, int, int);
constexpr uintptr_t     kMirrorWalkSite = 0x00487A4C;
constexpr unsigned char kMirrorWalkStock[5] = {0xE8, 0x0F, 0xDF, 0xFF, 0xFF};
constexpr uintptr_t     kWorldWalk = 0x00485960;

// gpl_statGfxPolysDrawn, bumped on entry to the clipper for every polygon GPL emits.
constexpr uintptr_t kPolygonsDrawn = 0x0086A0E4;
// gpl_mirrorDetailLevel, read by `cmp gpl_mirrorDetailLevel, ebx` (39 1D imm32) at 0x487876: 0 is
// the car loop without a walk, 1 the walk skipping objects with +4 set, 2 the walk with all.
constexpr uintptr_t kDetailCompare = 0x00487876;
constexpr uintptr_t kMirrorDetailLevel = 0x00559FD4;
// gpl_userClipPlaneMask: the user clip planes every transformed vertex is tested against (cells set
// their planes around each record, 0x4CAE73 / 0x4CAEA0). The background (horiz.3do: the horizon
// panels, the sky fan, the ground disc) is submitted at 0x4878D4 under whatever this holds when
// the walk starts, and a vertical cell plane cuts it along a vertical screen line.
constexpr uintptr_t kUserPlaneMask = 0x00580818;

// -- GL texture state around the background flush (report 105 section 4.18.1) ---------------------
//
// A glass's background flush sometimes paints an upside-down frame where GPL's paints its horizon:
// the look of a framebuffer's texture sampled with a mip's coordinates. GPL draws the background,
// and that flush can be roglv2's first after a baked pass. roglv2 draws on unit 0 and binds a
// texture only when its own cache says the polygon's differs, so GL state a baked pass leaves
// behind is drawn with whenever that cache is stale.
constexpr GLenum kActiveTextureEnum = 0x84E0;  // GL_ACTIVE_TEXTURE
constexpr GLenum kTexture0 = 0x84C0;           // GL_TEXTURE0
constexpr GLenum kCurrentProgram = 0x8B8D;     // GL_CURRENT_PROGRAM
constexpr GLenum kDrawFramebuffer = 0x8CA6;    // GL_DRAW_FRAMEBUFFER_BINDING
typedef void(WINAPI* ActiveTextureFn)(GLenum);
typedef void(WINAPI* BindTextureFn)(GLenum, GLuint);
typedef void(WINAPI* GetFloatvFn)(GLenum, GLfloat*);

// roglv2's glBindTexture import: the binds it issues while the background flushes.
BindTextureFn g_realBindTexture = nullptr;
bool          g_watchBinds = false;
unsigned      g_bindsSeen = 0;

void WINAPI bindTextureHook(GLenum target, GLuint name) {
    if (g_watchBinds) ++g_bindsSeen;
    g_realBindTexture(target, name);
}

// The textures one unit held, by name and level-0 size.
struct TextureTable {
    struct Entry {
        GLuint   name = 0;
        int      width = 0;
        int      height = 0;
        unsigned count = 0;
    };
    Entry    entries[6];
    unsigned used = 0;
    unsigned more = 0;

    void add(GLuint name, int width, int height) {
        for (unsigned i = 0; i < used; ++i)
            if (entries[i].name == name && entries[i].width == width &&
                entries[i].height == height) {
                ++entries[i].count;
                return;
            }
        if (used == 6) {
            ++more;
            return;
        }
        entries[used].name = name;
        entries[used].width = width;
        entries[used].height = height;
        entries[used].count = 1;
        ++used;
    }
};

struct TextureCensus {
    unsigned     samples = 0;
    unsigned     activeNotZero = 0;
    unsigned     program = 0;
    unsigned     unit0Off = 0;
    unsigned     unit1On = 0;
    unsigned     textureMatrix = 0;  // unit 0's texture matrix not the identity
    TextureTable units[2];
    TextureTable framebuffers;  // the draw framebuffer's name; sizes 0
};

// Per second.
struct Second {
    unsigned walks = 0;         // mirror walks GPL made
    unsigned bakedWalks = 0;    // of them, baked
    uint64_t polygonsBaked = 0;  // gpl_statGfxPolysDrawn over the baked walks: GPL's leftovers
    uint64_t polygonsGpl = 0;    // and over GPL's own
    unsigned armsUnflushed = 0;  // baked walks whose residual test no flush took: nothing left over
    // The flush at a baked pass's start: it reached roglv2, it did not (nothing was queued), or
    // nobody could tell.
    unsigned backgroundFlushed = 0;
    unsigned backgroundNotFlushed = 0;
    unsigned backgroundUnknown = 0;
    // Walks that began, and that ended, with user clip planes on: [0] GPL's, [1] baked.
    unsigned planesAtStart[2] = {};
    unsigned planesAtEnd[2] = {};
    uint32_t startBits = 0;
    uint32_t endBits = 0;
    // The polygons roglv2 holds when the walk starts: the background GPL has just submitted plus
    // anything queued before the mirror camera was bound. [baked][the pass rides a mirror-only
    // render (Stereo.cpp: every glass but the last) rather than the eye's own].
    uint64_t queuedSum[2][2] = {};
    unsigned queuedWalks[2][2] = {};
    int      queuedMax[2][2] = {};
    unsigned queuedUnknown = 0;
    // GL's state [0 when the walk starts, 1 just after a baked pass's background flush]
    // [the background flush is roglv2's first since a baked pass drew], and roglv2's binds during
    // that flush [first since a baked pass]. `fresh` splits the walks [mirror-only render][first].
    TextureCensus textures[2][2];
    uint64_t      binds[2] = {};
    unsigned      bindFlushes[2] = {};
    unsigned      bindless[2] = {};
    unsigned      fresh[2][2] = {};
    unsigned      freshUnknown = 0;
    unsigned refused[kMirrorRefusalCount] = {};
    // The baked passes.
    unsigned passes = 0;
    unsigned notDrawn = 0;
    unsigned fogPasses = 0;
    uint64_t visible = 0;
    uint64_t dispatches = 0;
    uint64_t repeats = 0;
    uint64_t ranges = 0;
    uint64_t draws = 0;
    uint64_t orderedRanges = 0;
    double   cpuMilliseconds = 0.0;
    int      rect[4] = {0, 0, 0, 0};  // the last pass's
    int      widest = 0;
    int      narrowest = 0;
    // Their cars and flagmen.
    unsigned listPasses = 0;
    unsigned listsLost = 0;    // passes with lists that the renderer never drew: cars missing
    unsigned hidden = 0;       // debug view 7
    uint64_t lists = 0;
    uint64_t flagmanLists = 0;
    uint64_t commands = 0;
    uint64_t drawnListDraws = 0;
    uint64_t walkRuns = 0;
    uint64_t walkRunsDrawn = 0;
    uint64_t flagmanRuns = 0;
    uint64_t flagmanRunsDrawn = 0;
    unsigned emptyLists = 0;
    unsigned badObject = 0;
    unsigned badFrame = 0;
    unsigned badMaterial = 0;
    unsigned badSprite = 0;
    unsigned tableFull = 0;
    unsigned withoutTexture = 0;
    // GPL's model-view at each list's dispatch against the pass's view (CarRenderer::prepare): a
    // list dispatched in world space whose model-view is the mirror view gplvr drew the track
    // from is placed by the poses alone, so `composed` 0 and errors near 0 prove the camera.
    unsigned worldLists = 0;
    unsigned composedLists = 0;
    float    maxRotationError = 0.0f;
    float    maxTranslationError = 0.0f;
    double   prepareMicroseconds = 0.0;
    double   drawMicroseconds = 0.0;
    // Debug view 15: GPL's own walks, the baked items it dispatched.
    unsigned observedWalks = 0;
    uint64_t observed = 0;
};

struct State {
    bool             installed = false;
    uintptr_t        polygonsDrawn = 0;
    uintptr_t        planeMask = 0;
    const int32_t*   detailLevel = nullptr;
    Second           second;
    // The texture census's GL, resolved on the first walk.
    GlFunctions      gl;
    ActiveTextureFn  activeTexture = nullptr;
    GetFloatvFn      getFloatv = nullptr;
    bool             glReady = false;
    bool             glFailed = false;
    bool             bindsHooked = false;
    // The walk in progress: its background flush is roglv2's first since a baked pass drew.
    int              walkFresh = 0;
};

State g_mirror;

bool censusGl(State& s) {
    if (s.glReady) return true;
    if (s.glFailed) return false;
    if (glResolve(s.gl) && s.gl.getIntegerv && s.gl.getTexLevelParameteriv && s.gl.isEnabled &&
        s.gl.wglGetProcAddress) {
        s.activeTexture =
            reinterpret_cast<ActiveTextureFn>(s.gl.wglGetProcAddress("glActiveTexture"));
        s.getFloatv = reinterpret_cast<GetFloatvFn>(GetProcAddress(s.gl.module, "glGetFloatv"));
    }
    if (!s.activeTexture || !s.getFloatv) {
        s.glFailed = true;
        logf("baked mirrors: the texture census could not resolve glGetIntegerv, glGetFloatv, "
             "glGetTexLevelParameteriv, glIsEnabled or glActiveTexture; its lines stay empty");
        return false;
    }
    s.glReady = true;
    return true;
}

void sampleTextures(State& s, TextureCensus& c) {
    if (!censusGl(s)) return;
    const GlFunctions& gl = s.gl;
    GLint active = 0;
    GLint program = 0;
    GLint framebuffer = 0;
    gl.getIntegerv(kActiveTextureEnum, &active);
    gl.getIntegerv(kCurrentProgram, &program);
    gl.getIntegerv(kDrawFramebuffer, &framebuffer);
    ++c.samples;
    if (static_cast<GLenum>(active) != kTexture0) ++c.activeNotZero;
    if (program) ++c.program;
    c.framebuffers.add(static_cast<GLuint>(framebuffer), 0, 0);
    for (int unit = 1; unit >= 0; --unit) {
        s.activeTexture(kTexture0 + unit);
        bool  on = gl.isEnabled(GL_TEXTURE_2D) != 0;
        GLint name = 0;
        GLint width = 0;
        GLint height = 0;
        gl.getIntegerv(GL_TEXTURE_BINDING_2D, &name);
        gl.getTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        gl.getTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        c.units[unit].add(static_cast<GLuint>(name), width, height);
        if (unit == 0 && !on) ++c.unit0Off;
        if (unit == 1 && on) ++c.unit1On;
    }
    // Unit 0 is active here.
    GLfloat matrix[16] = {};
    s.getFloatv(GL_TEXTURE_MATRIX, matrix);
    for (int i = 0; i < 16; ++i)
        if (matrix[i] != (i % 5 == 0 ? 1.0f : 0.0f)) {
            ++c.textureMatrix;
            break;
        }
    s.activeTexture(static_cast<GLenum>(active));
}

void formatTable(const TextureTable& table, bool sized, char* out, size_t size) {
    size_t used = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < table.used && used < size; ++i) {
        const TextureTable::Entry& e = table.entries[i];
        int n = sized ? std::snprintf(out + used, size - used, "%s#%u %dx%d x%u", i ? ", " : "",
                                      e.name, e.width, e.height, e.count)
                      : std::snprintf(out + used, size - used, "%s#%u x%u", i ? ", " : "",
                                      e.name, e.count);
        if (n < 0) break;
        used += static_cast<size_t>(n);
    }
    if (table.more && used < size)
        std::snprintf(out + used, size - used, "%s%u more", table.used ? ", " : "", table.more);
    if (!table.used && !table.more) std::snprintf(out, size, "none");
}

void reportCensus(const char* when, const char* walks, const TextureCensus& c) {
    if (!c.samples) return;
    char units[2][256];
    char framebuffers[128];
    formatTable(c.units[0], true, units[0], sizeof(units[0]));
    formatTable(c.units[1], true, units[1], sizeof(units[1]));
    formatTable(c.framebuffers, false, framebuffers, sizeof(framebuffers));
    logf("baked mirrors: GL state %s, %s -- %u walks; active unit not 0 in %u, a program bound in "
         "%u, unit 0 texturing off in %u, unit 1 texturing on in %u, unit 0's texture matrix not "
         "the identity in %u; unit 0 held %s; unit 1 held %s; draw framebuffer %s",
         when, walks, c.samples, c.activeNotZero, c.program, c.unit0Off, c.unit1On,
         c.textureMatrix, units[0], units[1], framebuffers);
}

uint32_t polygonsNow() {
    if (!g_mirror.polygonsDrawn) return 0;
    return *reinterpret_cast<const volatile uint32_t*>(g_mirror.polygonsDrawn);
}

uint32_t planeMaskNow() {
    if (!g_mirror.planeMask) return 0;
    return *reinterpret_cast<const volatile uint32_t*>(g_mirror.planeMask);
}

// Replaces the rel32 of the mirror walk's call. The parameter block is the one gpl_sceneRender
// was handed, as the main walk's thunk reads it (Hooks.cpp). End runs whether or not the pass was
// baked: it also closes debug view 15's observation of GPL's own walk.
//
// A baked pass's leftovers are flushed here, under its residual test, and the test is then
// dropped. Nothing is submitted between the walk and bitmapDraw's own flush (0x487A51..0x487C58:
// the matrix pop, gpl_renderClearDrawnMarks, the mip's bitmap lookup), so the flush only comes
// earlier into the same framebuffer; and a walk that left roglv2 nothing cannot leave its depth
// and rectangle armed for the main pass's horizon.
void __cdecl mirrorWorldWalkThunk(int a1, int a2, int a3, int a4, int a5, int a6, int a7,
                                  int a8) {
    Second&  t = g_mirror.second;
    void*    params = hooksLastSceneParams();
    uint32_t startMask = planeMaskNow();
    int      queued = residualQueuedPolygons();
    int      paneOnly = stereoMirrorOnlyPass() ? 1 : 0;
    // The background is queued, not yet flushed: no roglv2 flush since a baked pass drew makes it
    // the first.
    int since = residualFlushesSinceBakedPass();
    int fresh = since == 0 ? 1 : 0;
    g_mirror.walkFresh = fresh;
    if (since < 0)
        ++t.freshUnknown;
    else
        ++t.fresh[paneOnly][fresh];
    sampleTextures(g_mirror, t.textures[0][fresh]);
    g_bindsSeen = 0;
    g_watchBinds = g_mirror.bindsHooked;
    bool baked = bakedPassBeginMirror(params);
    g_watchBinds = false;
    if (queued < 0) {
        ++t.queuedUnknown;
    } else {
        int b = baked ? 1 : 0;
        t.queuedSum[b][paneOnly] += static_cast<unsigned>(queued);
        ++t.queuedWalks[b][paneOnly];
        t.queuedMax[b][paneOnly] = std::max(t.queuedMax[b][paneOnly], queued);
    }
    uint32_t before = polygonsNow();
    reinterpret_cast<WorldWalkFn>(kWorldWalk)(a1, a2, a3, a4, a5, a6, a7, a8);
    uint32_t drawn = polygonsNow() - before;
    bakedPassEndMirror(params);
    uint32_t endMask = planeMaskNow();
    if (startMask) {
        ++t.planesAtStart[baked ? 1 : 0];
        t.startBits |= startMask;
    }
    if (endMask) {
        ++t.planesAtEnd[baked ? 1 : 0];
        t.endBits |= endMask;
    }
    ++t.walks;
    if (baked) {
        rasterFlushPending();
        if (residualDisarm()) ++t.armsUnflushed;
        ++t.bakedWalks;
        t.polygonsBaked += drawn;
    } else {
        t.polygonsGpl += drawn;
    }
    bakedPassMirrorWalkDone(baked);
}

}  // namespace

void bakedMirrorInstall() {
    State& s = g_mirror;
    if (s.installed || g_options.bakedRenderer == 0 || g_options.bakedMirrors == 0) return;

    s.polygonsDrawn = rebase(kPolygonsDrawn);
    s.planeMask = rebase(kUserPlaneMask);
    if (!s.planeMask)
        logf("baked mirrors: gpl_userClipPlaneMask %08X not placed in this image; the `user clip "
             "planes` line reads 0",
             static_cast<unsigned>(kUserPlaneMask));
    uintptr_t detail = rebase(kMirrorDetailLevel);
    auto      compare = reinterpret_cast<const unsigned char*>(kDetailCompare);
    uint32_t  operand = 0;
    std::memcpy(&operand, compare + 2, sizeof(operand));
    if (detail && compare[0] == 0x39 && compare[1] == 0x1D && operand == detail)
        s.detailLevel = reinterpret_cast<const int32_t*>(detail);
    else
        logf("baked mirrors: gpl_mirrorDetailLevel not read -- `cmp` at %08X names %08X, the "
             "rebased address is %08X; the report line says `detail ?`",
             static_cast<unsigned>(kDetailCompare), operand, static_cast<unsigned>(detail));

    auto site = reinterpret_cast<unsigned char*>(kMirrorWalkSite);
    if (std::memcmp(site, kMirrorWalkStock, sizeof(kMirrorWalkStock)) != 0) {
        logf("baked mirrors: NOT installed -- %08X does not hold the stock `call "
             "gpl_worldRenderSegmentRange` (%02X %02X %02X %02X %02X); GPL draws the mirrors",
             static_cast<unsigned>(kMirrorWalkSite), site[0], site[1], site[2], site[3], site[4]);
        return;
    }
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&mirrorWorldWalkThunk) -
                                    (kMirrorWalkSite + 5));
    if (!patchBytes(site + 1, &rel, sizeof(rel))) {
        logf("baked mirrors: NOT installed -- could not write the mirror walk call at %08X; GPL "
             "draws the mirrors",
             static_cast<unsigned>(kMirrorWalkSite));
        return;
    }
    s.installed = true;
    HMODULE              roglv2 = GetModuleHandleA("roglv2.dll");
    const ImportRedirect bindImport[] = {{"glBindTexture",
                                          reinterpret_cast<void*>(&bindTextureHook),
                                          reinterpret_cast<void**>(&g_realBindTexture)}};
    s.bindsHooked = roglv2 && redirectImports(roglv2, bindImport, 1) == 1 && g_realBindTexture;
    if (!s.bindsHooked)
        logf("baked mirrors: roglv2's glBindTexture import not taken; the `textures` lines count "
             "no binds");
    logf("baked mirrors: mirror walk call at %08X redirected to %08X -- each mirror walk GPL makes "
         "is a baked pass from the mirror camera (gpl_statGfxPolysDrawn %08X%s). Watch `baked "
         "mirrors:` (walks baked, refusals by reason, GPL's leftover polygons) and `baked "
         "mirrors: cars` (commands == list draws); debug view 15 hands the mirrors back to GPL",
         static_cast<unsigned>(kMirrorWalkSite),
         static_cast<unsigned>(reinterpret_cast<uintptr_t>(&mirrorWorldWalkThunk)),
         static_cast<unsigned>(s.polygonsDrawn),
         s.polygonsDrawn ? "" : " NOT placed: the polygon counts read 0");
}

void bakedMirrorRefused(MirrorRefusal why) {
    if (why >= 0 && why < kMirrorRefusalCount) ++g_mirror.second.refused[why];
}

void bakedMirrorAccount(const BakedMirrorPass& pass) {
    Second& t = g_mirror.second;
    if (pass.drawn) {
        ++t.passes;
        if (pass.fog) ++t.fogPasses;
        t.visible += pass.stats.itemsVisible;
        t.dispatches += pass.dispatches;
        t.repeats += pass.repeats;
        t.ranges += pass.stats.rangesDrawn;
        t.draws += pass.stats.draws;
        t.orderedRanges += pass.stats.orderedRanges;
        t.cpuMilliseconds += pass.cpuMilliseconds;
        std::memcpy(t.rect, pass.rect, sizeof(t.rect));
        t.widest = std::max(t.widest, pass.rect[2]);
        t.narrowest = t.narrowest ? std::min(t.narrowest, pass.rect[2]) : pass.rect[2];
    } else {
        ++t.notDrawn;
    }
    if (!pass.hadLists) return;
    const baked::CarPassStats& c = pass.cars;
    ++t.listPasses;
    if (c.hidden)
        ++t.hidden;
    else if (!c.drawn && c.listDraws)
        ++t.listsLost;
    t.lists += c.lists;
    t.flagmanLists += c.flagmanLists;
    if (c.drawn) {
        t.commands += c.commands;
        t.drawnListDraws += c.listDraws;
        t.walkRuns += c.walkRuns;
        t.walkRunsDrawn += c.walkRunsDrawn;
        t.flagmanRuns += c.flagmanRuns;
        t.flagmanRunsDrawn += c.flagmanRunsDrawn;
    }
    t.emptyLists += c.emptyLists;
    t.badObject += c.badObject;
    t.badFrame += c.badFrame;
    t.badMaterial += c.badMaterial;
    t.badSprite += c.badSprite;
    t.tableFull += c.tableFull;
    t.withoutTexture += c.withoutTexture;
    t.worldLists += c.worldLists;
    t.composedLists += c.composedLists;
    t.maxRotationError = std::max(t.maxRotationError, c.maxRotationError);
    t.maxTranslationError = std::max(t.maxTranslationError, c.maxTranslationError);
    t.prepareMicroseconds += c.prepareMicroseconds;
    t.drawMicroseconds += c.drawMicroseconds;
}

void bakedMirrorBackgroundFlush(bool known, bool flushed) {
    Second& t = g_mirror.second;
    if (!known)
        ++t.backgroundUnknown;
    else if (flushed)
        ++t.backgroundFlushed;
    else
        ++t.backgroundNotFlushed;
    int fresh = g_mirror.walkFresh;
    sampleTextures(g_mirror, t.textures[1][fresh]);
    if (g_watchBinds) {
        t.binds[fresh] += g_bindsSeen;
        ++t.bindFlushes[fresh];
        if (!g_bindsSeen) ++t.bindless[fresh];
    }
    g_watchBinds = false;
}

void bakedMirrorObserved(size_t dispatches) {
    Second& t = g_mirror.second;
    ++t.observedWalks;
    t.observed += dispatches;
}

unsigned bakedMirrorPasses() { return g_mirror.second.passes; }

void bakedMirrorReport(double seconds, const baked::GpuPassTimes* gpu) {
    State&  s = g_mirror;
    Second& t = s.second;
    unsigned refusals = 0;
    for (unsigned count : t.refused) refusals += count;
    if (!s.installed || (!t.walks && !refusals)) {
        t = Second{};
        return;
    }
    char detail[16] = "?";
    if (s.detailLevel) std::snprintf(detail, sizeof(detail), "%d", *s.detailLevel);
    unsigned gplWalks = t.walks - t.bakedWalks;
    double   passes = t.passes ? static_cast<double>(t.passes) : 1.0;
    logf("baked mirrors: walks %.1f/s, %u baked and %u GPL's (mirror detail %s; detail 0 has no "
         "walk); GPL's polygons per walk %.0f in the baked ones (its leftovers: lit polygons, "
         "refused items, shadows, particles) and %.0f in its own; residual tests dropped "
         "unflushed %u; the background flushed before the copy-in %u, NOT flushed (nothing "
         "queued) %u, unknown %u; baked passes %u drawn (%u with fog), %u NOT drawn; per drawn pass: items "
         "visible %.0f, dispatches of baked items %.0f (%.1f again), ranges %.0f, draws %.0f, "
         "walk-order ranges %.0f, cpu %.3f ms; rect %dx%d at %d,%d (widths %d..%d)",
         t.walks / seconds, t.bakedWalks, gplWalks, detail,
         t.bakedWalks ? static_cast<double>(t.polygonsBaked) / t.bakedWalks : 0.0,
         gplWalks ? static_cast<double>(t.polygonsGpl) / gplWalks : 0.0, t.armsUnflushed,
         t.backgroundFlushed, t.backgroundNotFlushed, t.backgroundUnknown, t.passes, t.fogPasses, t.notDrawn, t.visible / passes, t.dispatches / passes,
         t.repeats / passes, t.ranges / passes, t.draws / passes, t.orderedRanges / passes,
         t.cpuMilliseconds / passes, t.rect[2], t.rect[3], t.rect[0], t.rect[1], t.narrowest,
         t.widest);
    logf("baked mirrors: user clip planes (gpl_userClipPlaneMask) on when the walk starts -- baked "
         "%u, GPL's %u (bits %02X); still on when it ends -- baked %u, GPL's %u (bits %02X). The "
         "background is submitted just before the start under that mask: a start count above 0 "
         "means GPL's sky and horizon were clipped by a cell's planes",
         t.planesAtStart[1], t.planesAtStart[0], t.startBits, t.planesAtEnd[1],
         t.planesAtEnd[0], t.endBits);
    auto queuedMean = [&](int b, int pane) {
        return t.queuedWalks[b][pane] ? static_cast<double>(t.queuedSum[b][pane]) /
                                            t.queuedWalks[b][pane]
                                      : 0.0;
    };
    logf("baked mirrors: polygons roglv2 holds when the walk starts (the background GPL has just "
         "submitted, horiz.3do at most 48, plus anything queued before the mirror camera was "
         "bound) -- baked: in mirror-only renders mean %.1f max %d over %u walks, in eye renders "
         "mean %.1f max %d over %u; GPL's: mirror-only %.1f max %d over %u, eye %.1f max %d over "
         "%u; unknown %u",
         queuedMean(1, 1), t.queuedMax[1][1], t.queuedWalks[1][1], queuedMean(1, 0),
         t.queuedMax[1][0], t.queuedWalks[1][0], queuedMean(0, 1), t.queuedMax[0][1],
         t.queuedWalks[0][1], queuedMean(0, 0), t.queuedMax[0][0], t.queuedWalks[0][0],
         t.queuedUnknown);
    auto bindMean = [&](int fresh) {
        return t.bindFlushes[fresh] ? static_cast<double>(t.binds[fresh]) / t.bindFlushes[fresh]
                                    : 0.0;
    };
    logf("baked mirrors: the background flush as roglv2's first draw since a baked pass (report "
         "105 section 4.18.1) -- first in mirror-only renders %u, in eye renders %u; after "
         "another flush in mirror-only renders %u, in eye renders %u; unknown %u. roglv2's "
         "glBindTexture calls during a baked pass's background flush (none: it drew with what was "
         "bound) -- when first mean %.1f, none in %u of %u; after another flush mean %.1f, none "
         "in %u of %u%s",
         t.fresh[1][1], t.fresh[0][1], t.fresh[1][0], t.fresh[0][0], t.freshUnknown,
         bindMean(1), t.bindless[1], t.bindFlushes[1], bindMean(0), t.bindless[0],
         t.bindFlushes[0], s.bindsHooked ? "" : " (NOT watched: the import was not taken)");
    reportCensus("when the walk starts", "first draw since a baked pass", t.textures[0][1]);
    reportCensus("when the walk starts", "after another flush", t.textures[0][0]);
    reportCensus("after the background flush", "first draw since a baked pass",
                 t.textures[1][1]);
    reportCensus("after the background flush", "after another flush", t.textures[1][0]);
    if (gpu && gpu->passes) {
        double timed = gpu->passes;
        logf("baked mirrors: gpu per pass %.3f ms = copy in + solid %.3f + walk-order %.3f + copy "
             "out %.3f, over %u timed passes (%u untimed)",
             (gpu->solidMs + gpu->orderedMs + gpu->copyOutMs) / timed, gpu->solidMs / timed,
             gpu->orderedMs / timed, gpu->copyOutMs / timed, gpu->passes, gpu->untimed);
        bakedLogGpuSpans("baked mirrors", *gpu);
    }
    if (refusals)
        logf("baked mirrors: walks left to GPL -- not bound %u, no parameters %u, no mirror camera "
             "%u, no node filter %u, drawing toggled off %u, no scene %u, upload stopped %u, "
             "renderer not up %u, upload not done %u, direct path (p[85] bit 0 clear) %u, no "
             "rectangle %u, capture's vanilla half %u, debug view 15 %u",
             t.refused[kMirrorNotBound], t.refused[kMirrorNoParams], t.refused[kMirrorNoCamera],
             t.refused[kMirrorNoFilter], t.refused[kMirrorDrawingOff], t.refused[kMirrorNoScene],
             t.refused[kMirrorUploadStopped], t.refused[kMirrorRendererDown],
             t.refused[kMirrorUploading], t.refused[kMirrorDirectPath], t.refused[kMirrorNoRect],
             t.refused[kMirrorCapture], t.refused[kMirrorByGpl]);
    if (t.observedWalks)
        logf("baked mirrors: DEBUG VIEW 15 -- GPL drew %u mirror walks itself, %.0f dispatches of "
             "baked items per walk (the baked passes' `dispatches of baked items` in the normal "
             "view should match it at the same spot); its mirror cars are walked beside GPL's "
             "draw: the `baked cars: N car draws (... mirror M ...)` line's MATCHED is the "
             "contract",
             t.observedWalks, static_cast<double>(t.observed) / t.observedWalks);
    if (t.listPasses) {
        double listPasses = t.listPasses;
        logf("baked mirrors: cars %.2f lists a pass (%.2f of them flagmen) over %u passes with "
             "lists (%u NOT drawn, %u hidden by view 7); commands issued %llu == list draws %llu "
             "of the drawn passes (contract); translucent runs %llu drawn of %llu, flagman runs "
             "%llu drawn of %llu (contract: equal); refused: empty lists %u, draws naming a "
             "missing object %u / frame %u / material %u, sprites without a texture %u, past the "
             "table %u; textured draws drawn flat %u; the mirror view against GPL's model-view at "
             "the dispatch: placed by the poses alone %u lists, through GPL's model-view %u "
             "(largest error rotation %.5f, translation %.4f m); cpu per pass prepare %.3f ms + "
             "draw %.3f ms",
             t.lists / listPasses, t.flagmanLists / listPasses, t.listPasses, t.listsLost,
             t.hidden, static_cast<unsigned long long>(t.commands),
             static_cast<unsigned long long>(t.drawnListDraws),
             static_cast<unsigned long long>(t.walkRunsDrawn),
             static_cast<unsigned long long>(t.walkRuns),
             static_cast<unsigned long long>(t.flagmanRunsDrawn),
             static_cast<unsigned long long>(t.flagmanRuns), t.emptyLists, t.badObject,
             t.badFrame, t.badMaterial, t.badSprite, t.tableFull, t.withoutTexture, t.worldLists,
             t.composedLists, t.maxRotationError, t.maxTranslationError,
             t.prepareMicroseconds / 1000.0 / listPasses, t.drawMicroseconds / 1000.0 / listPasses);
    }
    t = Second{};
}

}  // namespace gplvr
