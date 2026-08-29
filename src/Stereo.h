#pragma once

// Phase 1 -- real stereo, no HMD.
//
// Two cameras, each rendering into one half of the back buffer, from eye positions
// half an IPD either side of the head. Everything here uses the engine's own
// mechanisms; nothing new is drawn and no rasterizer code is added.
//
// The recipe is copied from the game itself. gpl_simFrameRender builds its mirror
// camera exactly this way:
//
//     bitmapInstanceAlloc(&dest, mainCamera->surface, x, y, w, h)
//     cameraCreate(storage, dest, fov, cx, cy, near, w / (2 * h))
//     ... sceneRender ...
//     bitmapInstanceFree(dest)
//
// so a sub-rect camera is a supported, exercised-every-frame path rather than
// something we invented.

#include "Discovery.h"

#include <cstddef>

namespace gplvr {

// The scene-render parameter block. Offsets read off gplc67.exe -- see
// gplvr-feasibility.md 2.2, and gpl_simFrameRender, which reaches the same two
// camera fields as [ebx+0x59] and [ebx+0xFD] with params at ebx+0x51.
struct SceneParams {
    float pad0[2];               // p[0..1]
    void* camera;                // p[2]    main camera
    float eyeX, eyeY, eyeZ;      // p[3..5] world position. Z is up.
    float pad1[18];              // p[6..23]
    float orientation[9];        // p[24..32] 3x3 row-major; columns are the local axes
    float pad2[10];              // p[33..42]
    void* mirrorCamera;          // p[43]   0 = skip the mirror pass
    float mirrorEye[3];          // p[44..46]
    float pad3[18];              // p[47..64]
    float mirrorOrientation[9];  // p[65..73]
};

static_assert(sizeof(float) == 4, "32-bit floats assumed");
static_assert(offsetof(SceneParams, camera) == 2 * 4, "p[2] is the main camera");
static_assert(offsetof(SceneParams, eyeX) == 3 * 4, "p[3..5] is the eye position");
static_assert(offsetof(SceneParams, orientation) == 24 * 4, "p+24 is the orientation");
static_assert(offsetof(SceneParams, mirrorCamera) == 43 * 4, "p[43] is the mirror camera");

// Camera object layout, from gpl_cameraCreate and gpl_cameraRebuildProjection.
// Only the fields we read are named; everything else the engine derives itself.
enum : unsigned {
    kCameraSurface = 0x00,  // void*  destination bitmap
    kCameraNear = 0x04,     // float
    // Decoded from gpl_cameraRebuildProjection:
    //   screen_x = cx - cot(fov/2) * (y/x)
    //   screen_y = cy - (width / (aspect*height)) * cot(fov/2) * (z/x)
    // with the screen edges at +/-1. So kCameraFov is the FULL HORIZONTAL angle, and
    //   tan(vertical half-angle) = tan(fov/2) * aspect * height / width
    // which makes kCameraAspect a STRETCH FACTOR: 1.0 is square pixels whatever the
    // viewport shape. Phase 1 read it as width/(2*height), copied from the mirror
    // camera's setup call, and squashed every eye by 2.25x. Fixed.
    kCameraFov = 0x2C,      // float  radians, full horizontal field of view
    kCameraAspect = 0x30,   // float  stretch factor; 1.0 = square pixels
    kCameraDirty = 0x3C,    // byte   set to rebuild the projection at the next bind
    // The projection matrix occupies 0x3D..0x7C -- genuinely unaligned, which is why
    // the object is 125 bytes and not 128.
    kCameraSize = 125,
};

// Rotate a head-relative offset into world space using the p+24 matrix.
//
// p[3..5] is a WORLD position, so adding a constant to it translates along world
// axes: the offset swings around as the car turns, which is not what an eye offset
// is. Local frame is right-handed: +X forward, +Y left, +Z up.
void localToWorld(const float m[9], float lx, float ly, float lz, float& wx, float& wy,
                  float& wz);

// Records what stereo needs from discovery. Safe to call with an incomplete binding:
// stereo simply stays unavailable and the frame hook falls back to a single render.
void stereoBind(const Binding& binding);

// True once every entry point stereo needs has been resolved.
bool stereoAvailable();

// Renders both eyes into halves of the back buffer, calling `original` once per eye.
// Returns false if the eye targets could not be built, in which case the caller
// should render normally -- a failed stereo setup must never cost you the game.
bool stereoRender(SceneParams* params, void(__cdecl* original)(void*));

// Drops the eye surfaces and cameras. Called when the render target changes size and
// on shutdown; the next frame rebuilds them.
void stereoRelease();

// The width in pixels of one eye's half of the render target, or 0 if the eye targets
// are not built. The presenter needs the exact value rather than assuming half the
// texture: an odd-width buffer -- which shrinkWindowByOnePixel produces -- leaves a
// column belonging to neither eye, and sampling a clean 50% split then shifts the right
// eye by half a pixel relative to what was actually rendered into it.
int stereoEyeWidth();

// Arm the one-shot field-of-view probe.
//
// On the next tracked frame both halves are rendered from an IDENTICAL viewpoint --
// same orientation, same eye position, no IPD -- and with the mirror suppressed, so the
// only thing differing between them is the field of view: the left half gets the
// frustum we normally use, the right half twice its tangent. The halves can then be
// compared bitwise. Identical halves mean the fov we write never reaches the renderer.
// That is the question the probe exists to settle without anyone judging a picture.
// Re-apply the eye frustum to `camera`, if it is one of ours.
//
// Called from the cameraBind hook, which sits inside sceneRender AFTER sceneRender has
// overwritten the camera's fov with the game's own and BEFORE cameraBind rebuilds the
// projection from it. Writing the frustum any earlier -- as stereoRender does -- is
// simply undone, which the fov probe demonstrated bitwise.
void stereoOnCameraBind(void* camera);

void stereoRequestFovProbe();

// True exactly once, on the frame the probe rendered, reporting the two frusta actually
// written. The caller reads the buffer back and does the comparison.
bool stereoTakeFovProbe(float fov[2], float aspect[2]);

}  // namespace gplvr
