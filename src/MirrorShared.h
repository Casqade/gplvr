#pragma once

// ONE MIRROR RENDER FOR EVERY GLASS (mirrorMode 0 and 1). GPL renders its mirror once and lets each
// glass read a part of the image. gplvr's panes render one image per glass instead, each aimed as
// that glass reflects the eye (Stereo.cpp aimPane), which on the 2020 demo's Ferrari 330 P4 is
// three renders per eye. This lays out the shared version: one camera whose view holds every
// glass's own view, and for each glass the region of mirror.mip where its view lands in that
// image. MirrorUv points each glass's UVs at its region; the regions may overlap, since they all
// read one picture.
//
// Everything here is in car axes (X forward, Y left, Z up); the functions down to
// mirrorSharedCamera are pure arithmetic.

#include "MirrorConfig.h"
#include "MirrorGeometry.h"

namespace gplvr {

// -- THE TWO MIRROR GLASSES OF THE 1967 TABLE ----------------------------------------------
//
// Every 1967 car has two round mirror glasses, and GPL already gives each its own half of
// mirror.mip: the +Y (left) glass samples u 0.5-1, the -Y (right) glass u 0-0.5
// (docs/audit-2026-09-13/41-mirror-panes.md 1.2). Where each glass sits, which way it faces
// and how big it is are read out of the team's .3do, in car-local metres -- X forward, Y left,
// Z up, origin at the physics body -- with the normal on the face toward the driver. The -Y
// glass is the exact Y mirror of the +Y one on every car. Stereo.cpp aims a car's panes from
// this when its glasses were not derived from the model (MirrorGeometry.h).
struct PaneGeometry {
    const char* team;
    float       centre[3];   // +Y glass
    float       normal[3];   // +Y glass
    float       halfWidth;   // half the glass across the car
    float       halfHeight;  // half the glass vertically
};

// Indexed by gpl_driverGetTeamIndex, whose order is the team-name table at 0x546178.
constexpr PaneGeometry kPaneGeometry[] = {
    {"brabham", {0.481f, 0.312f, 0.288f}, {-0.986f, 0.106f, 0.126f}, 0.057f, 0.057f},
    {"brm", {0.510f, 0.302f, 0.213f}, {-0.986f, 0.106f, 0.126f}, 0.057f, 0.057f},
    {"coventry", {0.481f, 0.312f, 0.288f}, {-0.986f, 0.106f, 0.126f}, 0.057f, 0.057f},
    {"eagle", {0.566f, 0.345f, 0.188f}, {-0.992f, -0.059f, 0.107f}, 0.057f, 0.057f},
    {"ferrari", {0.535f, 0.282f, 0.196f}, {-1.0f, 0.0f, 0.0f}, 0.0655f, 0.051f},
    {"lotus", {0.582f, 0.322f, 0.278f}, {-0.986f, 0.106f, 0.126f}, 0.057f, 0.057f},
    {"murasama", {0.566f, 0.345f, 0.188f}, {-0.992f, -0.059f, 0.107f}, 0.057f, 0.057f},
};
constexpr int kTeamCount = sizeof(kPaneGeometry) / sizeof(kPaneGeometry[0]);
constexpr int kTeamLotus = 5;

// Leaning past the glass would send the field through 180 degrees and the projection through
// infinity. Neither limit is ever reached by a seated driver; they are here so that a tracking
// glitch cannot produce a degenerate frustum.
constexpr float kMirrorMinDistance = 0.15f;
constexpr float kMirrorMinFov = 0.0175f;  // 1 degree
constexpr float kMirrorMaxFov = 2.44f;    // 140 degrees

struct MirrorGlass {
    float centre[3];
    float normal[3];
    float halfWidth;   // across the car
    float halfHeight;  // vertically
};

// What one glass shows an eye: a camera at the eye reflected through the glass plane (the normal
// turned by the driver's aim), looking along the eye's view of the glass reflected about that
// normal, with the glass's angular size from the eye (times the aim's fovScale) as its field.
struct MirrorGlassView {
    float camera[3];
    float direction[3];  // unit
    float fov;           // horizontal, radians
    float fovV;          // vertical, radians
    float distance;      // from the eye to the glass centre, metres
};
bool mirrorGlassView(const MirrorGlass& glass, const MirrorAim& aim, const float eye[3],
                     MirrorGlassView& out);

// The shared camera, and each glass's region in mirror.mip's UVs as MirrorUv writes them (u to
// the camera's right, v up). The camera's own image fills the whole texture.
struct MirrorSharedLayout {
    float forward[3];  // unit
    float fov;         // horizontal, radians
    float fovV;
    int   count;
    float u0[kMaxMirrorSurfaces];
    float u1[kMaxMirrorSurfaces];
    float v0[kMaxMirrorSurfaces];
    float v1[kMaxMirrorSurfaces];
};

// The layout for `count` glasses seen from `eye`, with the camera where mirrorSharedCamera puts it
// for that eye: each glass's region shows what its own pane would for things 15 m behind it
// (MirrorShared.cpp kSharedExactDistance). False, with `why`, when one camera cannot hold every
// glass's view.
bool mirrorSharedLayout(const MirrorGlass glasses[], const MirrorAim aims[], int count,
                        const float eye[3], MirrorSharedLayout& out, const char*& why);

// Where the shared camera sits for `eye`: the mean of the eye reflected through each glass, so
// each eye keeps its own viewpoint and the reflection its depth.
void mirrorSharedCamera(const MirrorGlass glasses[], const MirrorAim aims[], int count,
                        const float eye[3], float out[3]);

// -- THE SHARED VIEW FROM FRAME TO FRAME, driven by Stereo.cpp -----------------------------
//
// THE LAYOUT IS PER EYE. Each eye sees a glass from its own angle, so each glass's own view turns
// with the head and differs between the eyes; that difference is the reflection's depth, and
// panning with the head is the mirror following it. One layout for both eyes -- the first version
// of this, laid out from GPL's eye -- drew the same picture on the glass for both and read as
// flat as a head-centre render, and still. So each pass lays out its own picture and moves the
// glasses' UVs onto it (mirrorUvMoveRegions) before the pass draws them.

// Takes the shared view up for this frame: `count` glasses with their aims, laid out from GPL's
// own eye `seat` (car axes) for MirrorUv's first apply. False, with the reason said once, when
// it cannot -- MirrorUv's own regions then come back. mirrorSharedOff does that unconditionally.
bool mirrorSharedPlan(const MirrorGlass glasses[], const MirrorAim aims[], int count,
                      const float seat[3]);
void mirrorSharedOff();

// Pass `pass`'s picture, from `eye` (car axes): its layout, with the glasses' UVs moved onto it,
// and the camera's position into `position`. The planned layout stands in when this eye's
// cannot be made.
const MirrorSharedLayout& mirrorSharedForEye(const float eye[3], int pass, float position[3]);

// A pass that samples a picture without rendering it: the UVs back on the layout pass `source`
// rendered it with.
void mirrorSharedRegionsFor(int source);

}  // namespace gplvr
