#include "MirrorShared.h"

#include "Log.h"
#include "MirrorUv.h"

#include <cmath>

namespace gplvr {
namespace {

constexpr float kDegree = 0.0174532925f;

// A corner of a glass's view this close to perpendicular to the shared camera's axis (about 84
// degrees off it) would project toward infinity: one camera cannot hold that glass.
constexpr float kMinCornerDepth = 0.1f;

// How far behind its glass each glass's part of the shared view is exact, in metres: a car a few
// lengths back. Each glass's own camera sits where it reflects the eye -- the 330 P4's wings put
// theirs about 0.8 m either side of the car's centreline -- and the shared camera at their mean,
// so anything nearer or farther shifts in its glass by the parallax between the two. Laid out
// for infinity instead, a wing showed the middle of the view, as if it were a centre mirror.
constexpr float kSharedExactDistance = 15.0f;

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool normalise(float v[3]) {
    float length = std::sqrt(dot(v, v));
    if (length < 1e-6f) return false;
    for (int i = 0; i < 3; ++i) v[i] /= length;
    return true;
}

// A camera's right and up for `forward`, level with the car: right = forward x Z, up = right x
// forward. Looking astern, right is the car's left (+Y), as the stock mirror image has it.
bool basis(const float forward[3], float right[3], float up[3]) {
    right[0] = forward[1];
    right[1] = -forward[0];
    right[2] = 0.0f;
    if (!normalise(right)) return false;
    up[0] = right[1] * forward[2] - right[2] * forward[1];
    up[1] = right[2] * forward[0] - right[0] * forward[2];
    up[2] = right[0] * forward[1] - right[1] * forward[0];
    return true;
}

// The four corners of a glass's view as directions, into `corner`.
bool viewCorners(const MirrorGlassView& view, float corner[4][3]) {
    float right[3], up[3];
    if (!basis(view.direction, right, up)) return false;
    float tx = std::tan(view.fov * 0.5f);
    float ty = std::tan(view.fovV * 0.5f);
    for (int k = 0; k < 4; ++k) {
        float sx = k & 1 ? 1.0f : -1.0f;
        float sy = k & 2 ? 1.0f : -1.0f;
        for (int i = 0; i < 3; ++i)
            corner[k][i] = view.direction[i] + sx * tx * right[i] + sy * ty * up[i];
    }
    return true;
}

struct Bounds {
    float xMin = 1e30f, xMax = -1e30f, yMin = 1e30f, yMax = -1e30f;
};

// The corners' extent on the image plane of a camera looking along `forward`, at unit distance.
bool project(const float corner[4][3], const float forward[3], const float right[3],
             const float up[3], Bounds& b) {
    for (int k = 0; k < 4; ++k) {
        float depth = dot(corner[k], forward);
        if (depth < kMinCornerDepth * std::sqrt(dot(corner[k], corner[k]))) return false;
        float x = dot(corner[k], right) / depth;
        float y = dot(corner[k], up) / depth;
        if (x < b.xMin) b.xMin = x;
        if (x > b.xMax) b.xMax = x;
        if (y < b.yMin) b.yMin = y;
        if (y > b.yMax) b.yMax = y;
    }
    return true;
}

}  // namespace

bool mirrorGlassView(const MirrorGlass& glass, const MirrorAim& aim, const float eye[3],
                     MirrorGlassView& out) {
    // Yaw turns the normal about the car's up axis, positive anticlockwise seen from above, and a
    // mirror turned by an angle turns its reflection by twice that the same way -- so a positive
    // yaw swings the view, which looks astern, toward the car's right. Pitch then turns it about
    // the car's sideways axis so a positive angle lifts the face.
    const float* n0 = glass.normal;
    float        cy = std::cos(aim.yaw * kDegree), sy = std::sin(aim.yaw * kDegree);
    float        cp = std::cos(aim.pitch * kDegree), sp = std::sin(aim.pitch * kDegree);
    float        yx = n0[0] * cy - n0[1] * sy;
    float        yy = n0[0] * sy + n0[1] * cy;
    float        n[3] = {yx * cp - n0[2] * sp, yy, -yx * sp + n0[2] * cp};
    if (!normalise(n)) return false;

    float t[3] = {glass.centre[0] - eye[0], glass.centre[1] - eye[1], glass.centre[2] - eye[2]};
    float distance = std::sqrt(dot(t, t));
    if (distance < 1e-4f) return false;

    // The camera: the eye reflected through the glass plane. The direction: the view of the
    // glass reflected about its normal.
    float along = -dot(t, n);  // (E - C).n
    float un = dot(t, n) / distance;
    for (int i = 0; i < 3; ++i) {
        out.camera[i] = eye[i] - 2.0f * along * n[i];
        out.direction[i] = t[i] / distance - 2.0f * un * n[i];
    }

    // The field: the glass's angular size from the eye.
    float span = distance < kMirrorMinDistance ? kMirrorMinDistance : distance;
    float fov = 2.0f * std::atan(glass.halfWidth / span) * aim.fovScale;
    float fovV = 2.0f * std::atan(glass.halfHeight / span) * aim.fovScale;
    if (fov < kMirrorMinFov) fov = kMirrorMinFov;
    if (fov > kMirrorMaxFov) fov = kMirrorMaxFov;
    if (fovV < kMirrorMinFov) fovV = kMirrorMinFov;
    if (fovV > kMirrorMaxFov) fovV = kMirrorMaxFov;
    out.fov = fov;
    out.fovV = fovV;
    out.distance = distance;
    return true;
}

bool mirrorSharedLayout(const MirrorGlass glasses[], const MirrorAim aims[], int count,
                        const float eye[3], MirrorSharedLayout& out, const char*& why) {
    if (count < 1 || count > kMaxMirrorSurfaces) {
        why = "the car has no glass to lay out";
        return false;
    }
    // Each corner of each glass's view as the point kSharedExactDistance along it from that
    // glass's camera, relative to the shared camera.
    MirrorGlassView view[kMaxMirrorSurfaces];
    float           shared[3] = {};
    float           forward[3] = {};
    for (int g = 0; g < count; ++g) {
        if (!mirrorGlassView(glasses[g], aims[g], eye, view[g])) {
            why = "a glass's view could not be made from the eye";
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            shared[i] += view[g].camera[i] / static_cast<float>(count);
            forward[i] += view[g].direction[i];
        }
    }
    float corner[kMaxMirrorSurfaces][4][3];
    for (int g = 0; g < count; ++g) {
        if (!viewCorners(view[g], corner[g])) {
            why = "a glass looks straight up or down";
            return false;
        }
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i)
                corner[g][k][i] =
                    view[g].camera[i] + kSharedExactDistance * corner[g][k][i] - shared[i];
    }
    if (!normalise(forward)) {
        why = "the glasses look in opposite directions";
        return false;
    }

    // The mean direction first, then the centre of everything it sees, so the camera's symmetric
    // field wastes as little of the texture as it can.
    float  right[3], up[3];
    Bounds all;
    for (int round = 0; round < 2; ++round) {
        all = Bounds{};
        if (!basis(forward, right, up)) {
            why = "the glasses look straight up or down";
            return false;
        }
        for (int g = 0; g < count; ++g)
            if (!project(corner[g], forward, right, up, all)) {
                why = "the glasses look too far apart for one camera";
                return false;
            }
        if (round == 1) break;
        float x = 0.5f * (all.xMin + all.xMax);
        float y = 0.5f * (all.yMin + all.yMax);
        for (int i = 0; i < 3; ++i) forward[i] += x * right[i] + y * up[i];
        normalise(forward);
    }

    float halfX = std::fmax(std::fabs(all.xMin), std::fabs(all.xMax));
    float halfY = std::fmax(std::fabs(all.yMin), std::fabs(all.yMax));
    out.fov = 2.0f * std::atan(halfX);
    out.fovV = 2.0f * std::atan(halfY);
    if (out.fov > kMirrorMaxFov || out.fovV > kMirrorMaxFov) {
        why = "one camera holding every glass would need a field over 140 degrees";
        return false;
    }
    if (halfX < 1e-4f || halfY < 1e-4f) {
        why = "the glasses' views have no extent";
        return false;
    }

    for (int i = 0; i < 3; ++i) out.forward[i] = forward[i];
    out.count = count;
    for (int g = 0; g < count; ++g) {
        Bounds b;
        project(corner[g], forward, right, up, b);
        out.u0[g] = 0.5f * (b.xMin / halfX + 1.0f);
        out.u1[g] = 0.5f * (b.xMax / halfX + 1.0f);
        out.v0[g] = 0.5f * (b.yMin / halfY + 1.0f);
        out.v1[g] = 0.5f * (b.yMax / halfY + 1.0f);
    }
    return true;
}

void mirrorSharedCamera(const MirrorGlass glasses[], const MirrorAim aims[], int count,
                        const float eye[3], float out[3]) {
    float sum[3] = {};
    int   used = 0;
    for (int g = 0; g < count; ++g) {
        MirrorGlassView view;
        if (!mirrorGlassView(glasses[g], aims[g], eye, view)) continue;
        for (int i = 0; i < 3; ++i) sum[i] += view.camera[i];
        ++used;
    }
    for (int i = 0; i < 3; ++i) out[i] = used ? sum[i] / static_cast<float>(used) : eye[i];
}

namespace {

struct Shared {
    MirrorGlass        glass[kMaxMirrorSurfaces] = {};
    MirrorAim          aim[kMaxMirrorSurfaces];
    MirrorSharedLayout planned = {};
    MirrorSharedLayout eyeLayout = {};
    // The regions each pass's picture was laid out with, for a pass that samples it later.
    MirrorUvRegion eyeRegion[2][kMaxMirrorSurfaces] = {};
    bool           eyeValid[2] = {};
    const char*    refusal = nullptr;
    float          saidFov = 0.0f;
};

Shared g_shared;

void regionsOf(const MirrorSharedLayout& layout, MirrorUvRegion out[]) {
    for (int i = 0; i < layout.count; ++i)
        out[i] = MirrorUvRegion{layout.u0[i], layout.u1[i], layout.v0[i], layout.v1[i]};
}

}  // namespace

bool mirrorSharedPlan(const MirrorGlass glasses[], const MirrorAim aims[], int count,
                      const float seat[3]) {
    Shared&     s = g_shared;
    const char* why = "the car's glasses were not derived from its model";
    if (count > 0) {
        for (int i = 0; i < count; ++i) {
            s.glass[i] = glasses[i];
            s.aim[i] = aims[i];
        }
        MirrorSharedLayout layout;
        if (mirrorSharedLayout(s.glass, s.aim, count, seat, layout, why)) {
            MirrorUvRegion regions[kMaxMirrorSurfaces];
            regionsOf(layout, regions);
            mirrorUvSetRegions(regions, count);
            if (s.refusal || layout.count != s.planned.count ||
                std::fabs(layout.fov - s.saidFov) > 0.0175f) {
                s.saidFov = layout.fov;
                logf("mirror: shared view from GPL's eye, fov %.1f x %.1f deg, %d glass%s "
                     "(each eye lays out its own):",
                     layout.fov * 57.29578f, layout.fovV * 57.29578f, count,
                     count == 1 ? "" : "es");
                for (int i = 0; i < count; ++i)
                    logf("  %s glass: u %.3f-%.3f v %.3f-%.3f", mirrorConfigGlassName(i),
                         layout.u0[i], layout.u1[i], layout.v0[i], layout.v1[i]);
            }
            s.planned = layout;
            s.refusal = nullptr;
            return true;
        }
    }
    mirrorUvClearRegions();
    s.eyeValid[0] = s.eyeValid[1] = false;
    if (why != s.refusal) {
        s.refusal = why;
        logf("mirror: no shared view -- %s; each glass renders its own reflection", why);
    }
    return false;
}

void mirrorSharedOff() {
    Shared& s = g_shared;
    mirrorUvClearRegions();
    s.eyeValid[0] = s.eyeValid[1] = false;
    s.refusal = nullptr;
    s.planned.count = 0;
}

const MirrorSharedLayout& mirrorSharedForEye(const float eye[3], int pass, float position[3]) {
    Shared&     s = g_shared;
    const char* why = nullptr;
    if (!mirrorSharedLayout(s.glass, s.aim, s.planned.count, eye, s.eyeLayout, why))
        s.eyeLayout = s.planned;
    int at = pass & 1;
    regionsOf(s.eyeLayout, s.eyeRegion[at]);
    s.eyeValid[at] = mirrorUvMoveRegions(s.eyeRegion[at], s.eyeLayout.count);
    mirrorSharedCamera(s.glass, s.aim, s.eyeLayout.count, eye, position);
    return s.eyeLayout;
}

void mirrorSharedRegionsFor(int source) {
    Shared& s = g_shared;
    int     at = source & 1;
    if (s.eyeValid[at]) mirrorUvMoveRegions(s.eyeRegion[at], s.planned.count);
}

}  // namespace gplvr
