#include "MirrorWindow.h"

#include "Hooks.h"
#include "Log.h"
#include "MirrorUv.h"

#include <chrono>
#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

// The footprint's half extents grow by this much, for the filtering at the picture's edge and the
// eye's motion between the pose the pane was aimed from and the one the runtime shows.
constexpr float kFitMargin = 1.025f;
// Past this many glass half sizes the pane's picture is spread too thin to be worth looking at;
// the footprint is cut to it about its middle.
constexpr float kFitMaxHalf = 2.5f;
// A derived surface, or a picture, whose glass centre is this close to a glass's is that glass's.
constexpr float kSameCentre = 0.001f;
// The fit's log line: when a box moves by this many glass half sizes, at most once in kSayEvery
// seconds per eye and glass.
constexpr float  kSayChange = 0.05f;
constexpr double kSayEvery = 2.0;

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalise(float v[3]) {
    float length = std::sqrt(dot(v, v));
    if (length < 1e-6f) return false;
    for (int i = 0; i < 3; ++i) v[i] /= length;
    return true;
}

float apart(const float a[3], const float b[3]) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return std::sqrt(dot(d, d));
}

float clampFov(float fov) {
    return fov < kMirrorMinFov ? kMirrorMinFov : (fov > kMirrorMaxFov ? kMirrorMaxFov : fov);
}

// What a picture in a glass's region of mirror.mip was rendered with: the exact camera's plane and
// axes, and the box of that plane its field covers, before fovScale, about the glass's centre.
struct Picture {
    bool  valid = false;
    float centre[3] = {};     // the glass's, car axes
    float normal[3] = {};     // the exact camera's forward, the aimed plane's normal
    float left[3] = {};       // the exact camera's left and up, in the plane
    float up[3] = {};
    float middle[2] = {};     // metres along left and up from the centre
    float half[2] = {};       // metres
    float glassHalf[2] = {};  // the glass's half width and height
};

// cockpitGlassTurn's aimed glass at scale 1 (MirrorWindow.h, mirrorWindowGlassSet).
struct Reference {
    bool  set = false;
    float centre[3] = {};
    float pivot[3] = {};
    float gradient[2][3] = {};
    float atPivot[2] = {};
};

struct Said {
    float  box[4] = {};
    double at = -1e9;
};

struct State {
    Reference reference[kMaxWindowGlasses];
    // The picture each glass's region holds now, and what each eye's copy of mirror.mip holds.
    Picture live[kMaxWindowGlasses];
    Picture saved[2][kMaxWindowGlasses];
    Said    said[2][kMaxWindowGlasses];
};

State g_window;

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The model's glass outline in car axes, into `points`: the derived surface's own corners when the
// surface is this glass, else the rectangle of its half sizes about its centre, along its level
// edge n0 x Z and the vertical square to it. Returns the count, 0 when there is none.
int outlineOf(int glass, const MirrorGlass& mirror, float points[kMaxMirrorOutline][3]) {
    const MirrorSurface* surface = mirrorUvSurface(glass);
    if (surface && surface->outlinePoints >= 3 && apart(surface->centre, mirror.centre) < kSameCentre) {
        std::memcpy(points, surface->outline, sizeof(float) * 3 * surface->outlinePoints);
        return surface->outlinePoints;
    }
    const float* n0 = mirror.normal;
    float        edge[3] = {n0[1], -n0[0], 0.0f};
    float        vertical[3];
    if (!normalise(edge)) return 0;
    cross(n0, edge, vertical);
    if (vertical[2] < 0.0f)
        for (float& c : vertical) c = -c;
    if (!normalise(vertical)) return 0;
    static const float kSign[4][2] = {{1.0f, 1.0f}, {-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i)
            points[k][i] = mirror.centre[i] + kSign[k][0] * mirror.halfWidth * edge[i] +
                           kSign[k][1] * mirror.halfHeight * vertical[i];
    return 4;
}

// One line when an eye's box for a glass has moved, with the square field about the glass's centre
// that would cover the same footprint (`reach`, in half sizes) for comparison.
void say(int pass, int glass, const Picture& fit, int points, float reach) {
    Said& said = g_window.said[pass & 1][glass];
    float box[4] = {fit.half[0] / fit.glassHalf[0], fit.half[1] / fit.glassHalf[1],
                    fit.middle[0] / fit.glassHalf[0], fit.middle[1] / fit.glassHalf[1]};
    bool  moved = false;
    for (int i = 0; i < 4; ++i) moved = moved || std::fabs(box[i] - said.box[i]) > kSayChange;
    double now = seconds();
    if (!moved || now - said.at < kSayEvery) return;
    std::memcpy(said.box, box, sizeof(box));
    said.at = now;
    logf("mirror in housing: the %s eye's pane of glass %d covers %.2f x %.2f of the glass's half "
         "sizes, its middle %+.2f %+.2f of them from the glass's centre, what the model's glass "
         "(%d outline points) shows that eye; one square field about the centre would need %.2f",
         (pass & 1) ? "right" : "left", glass, box[0], box[1], box[2], box[3], points, reach);
}

}  // namespace

void mirrorWindowFit(int glass, int pass, const MirrorGlass& mirror, const MirrorAim& aim,
                     const float eye[3], bool made, MirrorGlassView& view) {
    if (glass < 0 || glass >= kMaxWindowGlasses) return;
    Picture& picture = g_window.live[glass];
    picture.valid = false;
    MirrorPortal& p = view.exact;
    if (!mirrorExact() || !made || !(p.depth > 0.0f) || !(aim.fovScale > 0.0f)) return;

    Picture fit;
    for (int i = 0; i < 3; ++i) {
        fit.centre[i] = mirror.centre[i];
        fit.normal[i] = p.forward[i];
        fit.up[i] = p.up[i];
    }
    cross(fit.up, fit.normal, fit.left);
    if (!normalise(fit.left)) return;
    fit.glassHalf[0] = mirror.halfWidth;
    fit.glassHalf[1] = mirror.halfHeight;
    if (!(fit.glassHalf[0] > 0.0f) || !(fit.glassHalf[1] > 0.0f)) return;
    fit.half[0] = fit.glassHalf[0];
    fit.half[1] = fit.glassHalf[1];

    // The footprint: each outline point's ray from the eye meets the aimed plane, measured along
    // the camera's left and up from the glass's centre. A ray that misses keeps the glass's own
    // field, the one mirrorPortal aimed.
    float points[kMaxMirrorOutline][3];
    int   count = outlineOf(glass, mirror, points);
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    float toCentre[3] = {fit.centre[0] - eye[0], fit.centre[1] - eye[1], fit.centre[2] - eye[2]};
    bool  seen = count >= 3;
    for (int k = 0; k < count && seen; ++k) {
        float ray[3] = {points[k][0] - eye[0], points[k][1] - eye[1], points[k][2] - eye[2]};
        float across = dot(ray, fit.normal);
        float t = std::fabs(across) > 1e-6f ? dot(toCentre, fit.normal) / across : -1.0f;
        if (!(t > 0.0f)) {
            seen = false;
            break;
        }
        float hit[3] = {t * ray[0] - toCentre[0], t * ray[1] - toCentre[1],
                        t * ray[2] - toCentre[2]};
        float at[2] = {dot(hit, fit.left), dot(hit, fit.up)};
        for (int axis = 0; axis < 2; ++axis) {
            if (at[axis] < lo[axis]) lo[axis] = at[axis];
            if (at[axis] > hi[axis]) hi[axis] = at[axis];
        }
    }
    for (int axis = 0; axis < 2 && seen; ++axis) seen = hi[axis] - lo[axis] > 1e-4f;
    float reach = 1.0f;
    if (seen) {
        for (int axis = 0; axis < 2; ++axis) {
            float half = 0.5f * (hi[axis] - lo[axis]) * kFitMargin;
            float most = kFitMaxHalf * fit.glassHalf[axis];
            fit.middle[axis] = 0.5f * (lo[axis] + hi[axis]);
            fit.half[axis] = half < most ? half : most;
            float outer =
                std::fmax(std::fabs(lo[axis]), std::fabs(hi[axis])) / fit.glassHalf[axis];
            reach = std::fmax(reach, outer * kFitMargin);
        }
    }

    // mirrorPortal's field and principal point with the box for the glass: half extents fovScale
    // x the box's over the depth, and the box's middle, fovScale x out on the plane, at screen
    // (0, 0) of gpl_cameraRebuildProjection's screen_x = cx - cot(fov/2) y/x, screen_y = cy -
    // z/(x tan(fovV/2)). The half extents the mapping uses are the clamped field's.
    p.fov = clampFov(2.0f * std::atan(aim.fovScale * fit.half[0] / p.depth));
    p.fovV = clampFov(2.0f * std::atan(aim.fovScale * fit.half[1] / p.depth));
    float tx = std::tan(p.fov * 0.5f), ty = std::tan(p.fovV * 0.5f);
    fit.half[0] = p.depth * tx / aim.fovScale;
    fit.half[1] = p.depth * ty / aim.fovScale;
    float d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = fit.centre[i] +
               aim.fovScale * (fit.middle[0] * fit.left[i] + fit.middle[1] * fit.up[i]) -
               view.camera[i];
    p.cx = dot(d, fit.left) / (p.depth * tx);
    p.cy = dot(d, fit.up) / (p.depth * ty);
    fit.valid = true;
    picture = fit;
    say(pass, glass, fit, seen ? count : 0, reach);
}

void mirrorWindowPictureSaved(int eye) {
    if (eye < 0 || eye > 1) return;
    std::memcpy(g_window.saved[eye], g_window.live, sizeof(g_window.live));
}

void mirrorWindowPictureRestored(int eye) {
    if (eye < 0 || eye > 1) return;
    std::memcpy(g_window.live, g_window.saved[eye], sizeof(g_window.live));
}

void mirrorWindowGlassSet(int glass, const float centre[3], const float pivot[3],
                          const float gradient[2][3], const float atPivot[2]) {
    if (glass < 0 || glass >= kMaxWindowGlasses) return;
    Reference& r = g_window.reference[glass];
    std::memcpy(r.centre, centre, sizeof(r.centre));
    std::memcpy(r.pivot, pivot, sizeof(r.pivot));
    std::memcpy(r.gradient, gradient, sizeof(r.gradient));
    std::memcpy(r.atPivot, atPivot, sizeof(r.atPivot));
    r.set = true;
}

int mirrorWindowVectors(float out[kMaxWindowGlasses * kWindowVectorsPerGlass][4]) {
    const State& s = g_window;
    bool         on = mirrorExact();
    int          served = 0;
    for (int g = 0; g < kMaxWindowGlasses; ++g) {
        const Reference& r = s.reference[g];
        const Picture&   p = s.live[g];
        bool  live = on && r.set && p.valid && apart(r.centre, p.centre) < kSameCentre;
        float v[kWindowVectorsPerGlass][4] = {};
        if (live) {
            // u(x) = a + (B.L) k_L ((x - pivot).L - m_L) + (B.U) k_U ((x - pivot).U - m_U), with
            // k the glass's half size over the picture's: the reference picture's image
            // coordinate there is the fitted one's.
            std::memcpy(v[0], r.pivot, sizeof(r.pivot));
            std::memcpy(v[1], p.normal, sizeof(p.normal));
            float k[2] = {p.glassHalf[0] / p.half[0], p.glassHalf[1] / p.half[1]};
            for (int c = 0; c < 2; ++c) {
                float alongLeft = dot(r.gradient[c], p.left) * k[0];
                float alongUp = dot(r.gradient[c], p.up) * k[1];
                for (int i = 0; i < 3; ++i)
                    v[2 + c][i] = alongLeft * p.left[i] + alongUp * p.up[i];
                v[2 + c][3] = r.atPivot[c] - alongLeft * p.middle[0] - alongUp * p.middle[1];
            }
        }
        for (int k = 0; k < kWindowVectorsPerGlass; ++k)
            for (int i = 0; i < 4; ++i) out[g * kWindowVectorsPerGlass + k][i] = v[k][i];
        served += live ? 1 : 0;
    }
    return served;
}

}  // namespace gplvr
