#include "MirrorUv.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "Log.h"

namespace gplvr {
namespace {

// A glass's authored UVs, kept so they can be put back. One entry per polygon node, with the
// 2n floats exactly as they were read.
//
// 32 vertices, not addPoly's 256: a mirror facet is a quad or near it, the 330P4's worst glass
// is 19 facets of a few vertices each, and sizing for 256 made this state 800 KB of BSS for
// nothing. A polygon over this REFUSES the car rather than being skipped -- half a glass moved
// to a new region and half left behind would show two reflections on one piece of glass.
constexpr int kMaxUvVerts = 32;
constexpr int kMaxUvFloats = 2 * kMaxUvVerts;

struct SavedPoly {
    uintptr_t node = 0;
    int       verts = 0;
    float     uv[kMaxUvFloats] = {};
};

struct Glass {
    MirrorTextureRect rect;
    float             u0 = 0.0f, u1 = 0.0f, v0 = 0.0f, v1 = 0.0f;  // what was WRITTEN
    int               polys = 0;
    SavedPoly         saved[kMaxMirrorPolysPerSurface];
};

struct State {
    MirrorSurfaceSet set;
    bool             haveSet = false;
    int              team = -1;

    int   count = 0;
    Glass glass[kMaxMirrorSurfaces];

    bool applied = false;
    bool gaveUp = false;
    int  baseWidth = 0;
    int  baseHeight = 0;

    const char* refusal = nullptr;
    int         refusalReports = 6;
};

State g_state;

void refuse(const char* why) {
    State& s = g_state;
    if (s.refusal == why || s.refusalReports <= 0) {
        s.refusal = why;
        return;
    }
    --s.refusalReports;
    s.refusal = why;
    logf("mirror uv: no region per glass -- %s. Every glass keeps the UVs its model was "
         "authored with, and the mirror is the two halves of mirror.mip.",
         why);
}

bool readUv(uintptr_t node, int verts, float* out) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(node + 16),
                    sizeof(float) * 2 * static_cast<size_t>(verts));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeUv(uintptr_t node, int verts, const float* in) {
    __try {
        std::memcpy(reinterpret_cast<void*>(node + 16), in,
                    sizeof(float) * 2 * static_cast<size_t>(verts));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// THE REGION LAYOUT. Each glass gets a region shaped to its own aspect, because a region of the
// wrong shape spends its texels on the wrong axis: the 330P4's centre glass is 103x29 mm, and a
// square region gives it as many rows as columns when it needs three and a half times as many
// columns. aimPane writes the compensating aspect into the camera whatever the region's shape,
// so the shape only decides sharpness, never geometry.
//
// Every glass is given the same AREA at its own aspect -- w = s*sqrt(a), h = s/sqrt(a) -- and
// the largest s whose rects shelf-pack into mirror.mip is found by bisection. The shelves are
// then stretched to tile the texture exactly: every shelf to the full width, and the shelves
// together to the full height. Stretching only ever adds texels on an axis, so it cannot make a
// glass softer, and it leaves nothing of mirror.mip unrendered.
//
// Two square glasses come out as two halves side by side, which is what the hardcoded two-pane
// path has always used.
struct Region {
    float u0 = 0.0f, u1 = 0.0f, v0 = 0.0f, v1 = 0.0f;
};

// A glass this far from square is either a slit or a misread, and shaping a region to it would
// starve the other axis to a handful of texels.
constexpr float kMinRegionAspect = 0.125f;
constexpr float kMaxRegionAspect = 8.0f;

float glassAspect(const MirrorSurface& surface) {
    float w = surface.halfWidth;
    float h = surface.halfHeight;
    if (!std::isfinite(w) || !std::isfinite(h) || w < 1e-4f || h < 1e-4f) return 1.0f;
    float aspect = w / h;
    if (aspect < kMinRegionAspect) aspect = kMinRegionAspect;
    if (aspect > kMaxRegionAspect) aspect = kMaxRegionAspect;
    return aspect;
}

// First fit by decreasing height. `order` is tallest first, so a glass never has to fit a shelf
// lower than itself and a shelf's height is that of the first glass placed on it.
bool shelfPack(const float* aspect, const int* order, int count, float scale, float width,
               float height, int* shelfOf, float* shelfWidth, float* shelfHeight, int& shelves) {
    shelves = 0;
    float used = 0.0f;
    for (int n = 0; n < count; ++n) {
        int   i = order[n];
        float root = std::sqrt(aspect[i]);
        float w = scale * root;
        float h = scale / root;
        if (w > width) return false;
        int k = 0;
        while (k < shelves && shelfWidth[k] + w > width) ++k;
        if (k == shelves) {
            if (used + h > height) return false;
            shelfWidth[k] = 0.0f;
            shelfHeight[k] = h;
            used += h;
            ++shelves;
        }
        shelfOf[i] = k;
        shelfWidth[k] += w;
    }
    return true;
}

// Lays out `count` regions in a width x height texture. Returns the number of shelves, or 0 when
// some region would round to no texels at all.
int layoutRegions(const MirrorSurfaceSet& set, int count, int width, int height, Region* out) {
    float aspect[kMaxMirrorSurfaces];
    int   order[kMaxMirrorSurfaces];
    for (int i = 0; i < count; ++i) {
        aspect[i] = glassAspect(set.surface[i]);
        order[i] = i;
    }
    // Tallest first is narrowest aspect first. Insertion sort, stable, so equal glasses keep the
    // order the walk derived them in.
    for (int a = 1; a < count; ++a) {
        int i = order[a];
        int b = a;
        while (b > 0 && aspect[order[b - 1]] > aspect[i]) {
            order[b] = order[b - 1];
            --b;
        }
        order[b] = i;
    }

    float W = static_cast<float>(width);
    float H = static_cast<float>(height);
    int   shelfOf[kMaxMirrorSurfaces];
    float shelfWidth[kMaxMirrorSurfaces];
    float shelfHeight[kMaxMirrorSurfaces];
    int   shelves = 0;

    // At `high` one glass alone has an area of s*s, at least eight times the texture's, so the
    // bisection starts with a scale that fits (0) and one that cannot.
    float low = 0.0f;
    float high = (W > H ? W : H) * std::sqrt(kMaxRegionAspect);
    for (int step = 0; step < 40; ++step) {
        float mid = 0.5f * (low + high);
        if (shelfPack(aspect, order, count, mid, W, H, shelfOf, shelfWidth, shelfHeight, shelves))
            low = mid;
        else
            high = mid;
    }
    if (!(low > 0.0f) ||
        !shelfPack(aspect, order, count, low, W, H, shelfOf, shelfWidth, shelfHeight, shelves))
        return 0;

    auto nearest = [](float x) { return static_cast<int>(std::floor(x + 0.5f)); };
    float total = 0.0f;
    for (int k = 0; k < shelves; ++k) total += shelfHeight[k];

    float top = 0.0f;
    for (int k = 0; k < shelves; ++k) {
        int y0 = nearest(H * top / total);
        top += shelfHeight[k];
        int y1 = nearest(H * top / total);

        // Left to right in the order the glasses were placed, which is `order` again.
        float across = 0.0f;
        for (int n = 0; n < count; ++n) {
            int i = order[n];
            if (shelfOf[i] != k) continue;
            int x0 = nearest(W * across / shelfWidth[k]);
            across += low * std::sqrt(aspect[i]);
            int x1 = nearest(W * across / shelfWidth[k]);
            if (x1 <= x0 || y1 <= y0) return 0;

            Region& region = out[i];
            region.u0 = static_cast<float>(x0) / W;
            region.u1 = static_cast<float>(x1) / W;
            region.v0 = static_cast<float>(y0) / H;
            region.v1 = static_cast<float>(y1) / H;
        }
    }
    return shelves;
}

// A UV range to the texel rect the copy-out writes, including the t flip: rogl_mipmapLoaderLoad
// sets a byte at mip+113 for the name "mirror.mip", both of roglv2's draw paths then sample
// t' = 1 - t (0x2000A3D3..0x2000A3E5, and the drawer's vtbl+64 at 0x2001DFE5), and the copy-out
// passes y through as the destination row offset. So a region at v is written at rows 1 - v.
MirrorTextureRect rectForRange(float u0, float u1, float v0, float v1, int width, int height) {
    auto span = [](float low, float high, int size, int& start, int& extent) {
        if (high < low) {
            float swap = low;
            low = high;
            high = swap;
        }
        start = static_cast<int>(std::floor(low * static_cast<float>(size) + 0.5f));
        int end = static_cast<int>(std::floor(high * static_cast<float>(size) + 0.5f));
        if (start < 0) start = 0;
        if (end > size) end = size;
        if (end <= start) end = start + 1 <= size ? start + 1 : size;
        if (start >= end) start = end - 1;
        if (start < 0) start = 0;
        extent = end - start;
    };

    MirrorTextureRect rect;
    span(u0, u1, width, rect.x, rect.w);
    span(1.0f - v1, 1.0f - v0, height, rect.y, rect.h);
    return rect;
}

// Whether one vertex's (u, v) lies in the region written for its glass. Both axes: two glasses
// stacked on shelves share the whole u range, so u alone cannot tell a region from authored UVs.
bool insideRegion(const Glass& glass, const float* uv) {
    constexpr float kSlack = 0.002f;
    return uv[0] >= glass.u0 - kSlack && uv[0] <= glass.u1 + kSlack &&
           uv[1] >= glass.v0 - kSlack && uv[1] <= glass.v1 + kSlack;
}

void restore() {
    State& s = g_state;
    if (!s.applied) return;

    // Only polygons that still hold what was WRITTEN are put back. A node holding anything else
    // has been reloaded or taken over, and writing the authored values into it would be the same
    // mistake in the other direction.
    for (int g = 0; g < s.count; ++g) {
        Glass& glass = s.glass[g];
        for (int p = 0; p < glass.polys; ++p) {
            SavedPoly& saved = glass.saved[p];
            if (!saved.node || saved.verts < 1) continue;
            float now[kMaxUvFloats];
            if (!readUv(saved.node, saved.verts, now)) continue;
            bool ours = true;
            for (int k = 0; ours && k < 2 * saved.verts; k += 2) ours = insideRegion(glass, now + k);
            if (ours) writeUv(saved.node, saved.verts, saved.uv);
        }
    }

    s.applied = false;
    s.count = 0;
    for (Glass& glass : s.glass) glass = Glass{};
}

bool apply(int baseWidth, int baseHeight) {
    State& s = g_state;
    if (!s.haveSet || !s.set.derived) {
        refuse("no mirror surfaces were derived for this car");
        return false;
    }
    if (!s.set.polysUsable) {
        refuse(s.set.polyRefusal ? s.set.polyRefusal
                                 : "this car's polygons cannot all be moved together");
        return false;
    }
    if (baseWidth < 4 || baseHeight < 4) {
        refuse("mirror.mip's size is not known yet");
        return false;
    }
    // ONE GLASS IS LAID OUT TOO, as a single region over the whole texture. Without a layout the
    // panes fall back to the two shared halves, which draw this glass into one half and a Y
    // mirror of it into the other -- and cars18/c03's lone glass reads u 0.255-0.745, straight
    // across the seam, so it showed two different reflections side by side.
    if (s.set.count < 1) {
        refuse("no mirror surfaces were derived for this car");
        return false;
    }

    int wanted = s.set.count;
    if (wanted > kMaxMirrorSurfaces) wanted = kMaxMirrorSurfaces;

    // Laid out before anything is written, so a layout that cannot be made has nothing to undo.
    Region region[kMaxMirrorSurfaces];
    int    shelves = layoutRegions(s.set, wanted, baseWidth, baseHeight, region);
    if (!shelves) {
        refuse("mirror.mip is too small to give every glass a region of at least one texel");
        return false;
    }

    for (int i = 0; i < wanted; ++i) {
        const MirrorSurface& surface = s.set.surface[i];
        Glass&               glass = s.glass[i];
        glass = Glass{};

        // The authored span has to be non-degenerate in both axes or there is nothing to map
        // linearly from -- a glass whose UVs are a single point cannot be stretched over a
        // region without inventing the mapping.
        float du = surface.u1 - surface.u0;
        float dv = surface.v1 - surface.v0;
        if (du < 0.0f) du = -du;
        if (dv < 0.0f) dv = -dv;
        if (du < 1e-4f || dv < 1e-4f) {
            refuse("a glass's authored UVs cover no area, so there is no mapping to move");
            // Glasses before this one are already rewritten, and leaving them that way with no
            // regions in force would point them at parts of mirror.mip nothing renders into.
            s.count = i;
            s.applied = true;
            restore();
            return false;
        }

        glass.u0 = region[i].u0;
        glass.u1 = region[i].u1;
        glass.v0 = region[i].v0;
        glass.v1 = region[i].v1;
        glass.rect = rectForRange(glass.u0, glass.u1, glass.v0, glass.v1, baseWidth, baseHeight);

        float uLow = surface.u0 < surface.u1 ? surface.u0 : surface.u1;
        float vLow = surface.v0 < surface.v1 ? surface.v0 : surface.v1;
        float uScale = (glass.u1 - glass.u0) / du;
        float vScale = (glass.v1 - glass.v0) / dv;

        for (int p = 0; p < surface.polys && p < kMaxMirrorPolysPerSurface; ++p) {
            SavedPoly& saved = glass.saved[glass.polys];
            saved.node = surface.poly[p];
            saved.verts = surface.polyVerts[p];
            // Every polygon of a glass moves or none of them does, so anything this cannot
            // handle refuses the car instead of being passed over.
            if (!saved.node || saved.verts < 3 || saved.verts > kMaxUvVerts) {
                refuse("a glass has a polygon with more vertices than this moves at once");
                s.count = i + 1;
                s.applied = true;
                restore();
                return false;
            }
            if (!readUv(saved.node, saved.verts, saved.uv)) {
                refuse("a glass's polygon UVs could not be read, so they cannot be moved or "
                       "put back");
                s.count = i + 1;
                s.applied = true;
                restore();
                return false;
            }

            float moved[kMaxUvFloats];
            for (int k = 0; k < 2 * saved.verts; k += 2) {
                moved[k] = glass.u0 + (saved.uv[k] - uLow) * uScale;
                moved[k + 1] = glass.v0 + (saved.uv[k + 1] - vLow) * vScale;
            }
            // Counted BEFORE the write is attempted, so a polygon whose authored UVs are saved
            // is always one restore() will put back even if the write failed half way.
            ++glass.polys;
            if (!writeUv(saved.node, saved.verts, moved)) {
                refuse("a glass's polygon UVs could not be written");
                s.count = i + 1;
                s.applied = true;
                restore();
                return false;
            }
        }

        if (!glass.polys) {
            refuse("a glass has no polygon to move, so it cannot be given a region");
            s.count = i + 1;
            s.applied = true;  // whatever did get written has to be put back
            restore();
            return false;
        }
        s.count = i + 1;
    }

    s.applied = true;
    s.baseWidth = baseWidth;
    s.baseHeight = baseHeight;
    s.refusal = nullptr;

    char glasses[160] = {};
    size_t used = 0;
    for (int i = 0; i < s.count && used < sizeof(glasses); ++i) {
        const MirrorTextureRect& r = s.glass[i].rect;
        int wrote = _snprintf_s(glasses + used, sizeof(glasses) - used, _TRUNCATE,
                                "%s%s %dx%d", i ? ", " : "", s.set.surface[i].label, r.w, r.h);
        if (wrote < 0) break;
        used += static_cast<size_t>(wrote);
    }
    logf("mirror uv: team %d has %d glass%s, each in a region of mirror.mip (%dx%d) of its own: "
         "%s",
         s.team, s.count, s.count == 1 ? "" : "es", baseWidth, baseHeight, glasses);
    return true;
}

}  // namespace

void mirrorUvSetSurfaces(int team, const MirrorSurfaceSet* set) {
    restore();
    State& s = g_state;
    s.gaveUp = false;
    s.refusal = nullptr;
    s.refusalReports = 6;
    s.baseWidth = 0;
    s.baseHeight = 0;
    if (!set) {
        s.haveSet = false;
        s.team = -1;
        return;
    }
    s.set = *set;
    s.haveSet = true;
    s.team = team;
}

void mirrorUvEnsure(int baseWidth, int baseHeight) {
    State& s = g_state;
    if (s.gaveUp) return;

    if (!s.applied) {
        if (!apply(baseWidth, baseHeight)) s.gaveUp = true;
        return;
    }

    // THE MODEL CAN BE RELOADED UNDER THESE ADDRESSES, which puts the authored UVs back and is
    // indistinguishable from never having written them. The cheap check first: one dword, the
    // cockpit handle the walk started from.
    if (s.haveSet && s.set.cockpit && mirrorGeometryCockpit(s.team) != s.set.cockpit) {
        logf("mirror uv: team %d's cockpit node moved, so its model was reloaded -- the regions "
             "are dropped and will be laid out again once the geometry is derived",
             s.team);
        s.applied = false;
        s.count = 0;
        for (Glass& glass : s.glass) glass = Glass{};
        s.haveSet = false;
        return;
    }

    if (baseWidth != s.baseWidth || baseHeight != s.baseHeight) {
        logf("mirror uv: mirror.mip is now %dx%d, was %dx%d -- the regions are laid out again",
             baseWidth, baseHeight, s.baseWidth, s.baseHeight);
        restore();
        return;
    }

    // Re-assert: a polygon whose u has gone back outside its region is one the model rewrote, so
    // it is moved again. Checking one vertex is enough to notice, and the whole polygon is
    // rewritten from the saved authored values when it has.
    for (int g = 0; g < s.count; ++g) {
        Glass& glass = s.glass[g];
        for (int p = 0; p < glass.polys; ++p) {
            SavedPoly& saved = glass.saved[p];
            if (!saved.node || saved.verts < 1) continue;
            float now[kMaxUvFloats];
            if (!readUv(saved.node, saved.verts, now)) continue;
            if (insideRegion(glass, now)) continue;

            float du = s.set.surface[g].u1 - s.set.surface[g].u0;
            float dv = s.set.surface[g].v1 - s.set.surface[g].v0;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            if (du < 1e-4f || dv < 1e-4f) continue;
            float uLow = s.set.surface[g].u0 < s.set.surface[g].u1 ? s.set.surface[g].u0
                                                                   : s.set.surface[g].u1;
            float vLow = s.set.surface[g].v0 < s.set.surface[g].v1 ? s.set.surface[g].v0
                                                                   : s.set.surface[g].v1;
            float uScale = (glass.u1 - glass.u0) / du;
            float vScale = (glass.v1 - glass.v0) / dv;

            float moved[kMaxUvFloats];
            for (int k = 0; k < 2 * saved.verts; k += 2) {
                moved[k] = glass.u0 + (saved.uv[k] - uLow) * uScale;
                moved[k + 1] = glass.v0 + (saved.uv[k + 1] - vLow) * vScale;
            }
            writeUv(saved.node, saved.verts, moved);
        }
    }
}

void mirrorUvRelease() { restore(); }

int mirrorUvCount() { return g_state.applied ? g_state.count : 0; }

bool mirrorUvRect(int index, MirrorTextureRect& out) {
    if (index < 0 || index >= mirrorUvCount()) return false;
    out = g_state.glass[index].rect;
    return out.w > 0 && out.h > 0;
}

}  // namespace gplvr
