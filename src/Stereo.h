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
    float pad2[7];               // p[33..39]
    // The main camera's frustum, applied by sceneRender to whatever camera sits in
    // p[2] -- ours included -- on the two lines immediately before cameraBind:
    //     cameraSetFov(camera, p[40])
    //     cameraSetPrincipalPoint(camera, p[41], p[42])
    // The fov half of that is the bug that cost weeks in Phase 2. The principal point
    // is the same bug on the very next line, and it went unnoticed for as long again.
    float mainFov;               // p[40]
    float mainCx;                // p[41]
    float mainCy;                // p[42]   the cockpit view's vertical offset
    void* mirrorCamera;          // p[43]   0 = skip the mirror pass
    float mirrorEye[3];          // p[44..46]
    float pad3[18];              // p[47..64]
    float mirrorOrientation[9];  // p[65..73]
};

static_assert(sizeof(float) == 4, "32-bit floats assumed");
static_assert(offsetof(SceneParams, camera) == 2 * 4, "p[2] is the main camera");
static_assert(offsetof(SceneParams, eyeX) == 3 * 4, "p[3..5] is the eye position");
static_assert(offsetof(SceneParams, orientation) == 24 * 4, "p+24 is the orientation");
static_assert(offsetof(SceneParams, mainFov) == 40 * 4, "p[40] is the main fov");
static_assert(offsetof(SceneParams, mainCy) == 42 * 4, "p[42] is the main cy");
static_assert(offsetof(SceneParams, mirrorCamera) == 43 * 4, "p[43] is the mirror camera");

// Camera object layout, from gpl_cameraCreate and gpl_cameraRebuildProjection.
// Only the fields we read are named; everything else the engine derives itself.
enum : unsigned {
    kCameraSurface = 0x00,  // void*  destination bitmap
    kCameraNear = 0x04,     // float
    // Half extents, and the principal point PRE-MULTIPLIED by them -- gpl_cameraCreate
    // stores cx*halfW and cy*halfH, which is why cameraRebuildProjection can divide by
    // the centre and get a plain normalised cx back.
    kCameraHalfWExtent = 0x24,
    kCameraHalfHExtent = 0x28,
    kCameraCx = 0x34,       // float, = cx * halfWExtent
    kCameraCy = 0x38,       // float, = cy * halfHExtent
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

// The sub-rect one eye is rendered into, in source pixels: x, y, w, h. False when there
// are no eye targets. The presenter samples exactly this, which is what lets an eye be a
// square inside its half rather than the whole half.
bool stereoEyeRect(int eye, int out[4]);


// Re-apply the eye frustum to `camera`, if it is one of ours.
//
// Called from the cameraBind hook, which sits inside sceneRender AFTER sceneRender has
// overwritten the camera's fov with the game's own and BEFORE cameraBind rebuilds the
// projection from it. Writing the frustum any earlier -- as stereoRender does -- is
// simply undone.
//
// It puts the PRINCIPAL POINT back as well. sceneRender sets the fov and the principal
// point on consecutive lines, and only the fov half of that was ever noticed; the other
// half shifted every VR frame by an eighth of half-height for as long again.
void stereoOnCameraBind(void* camera);

}  // namespace gplvr
