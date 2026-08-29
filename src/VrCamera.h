#pragma once

// Turning the presenter's OpenXR poses into something GPL's camera understands.
//
// Two coordinate frames, both right-handed, neither agreeing with the other:
//
//   OpenXR   +X right, +Y up,   -Z forward
//   GPL      +X forward, +Y left, +Z up
//
// so an XR vector (x, y, z) is the GPL vector (-z, -x, y), and back the other way a GPL
// vector (a, b, c) is the XR vector (-b, c, -a). Both are proper rotations; the mapping
// has determinant +1, which is worth checking rather than assuming, because getting it
// wrong mirrors the world and the symptom is "it feels wrong" rather than anything
// obviously broken.
//
// The head rotation is composed with the game's own camera rather than replacing it. The
// matrix at p+24 already holds the car's attitude and its g-force lean; post-multiplying
// by the head rotation means looking left turns your view left *relative to the car*,
// whatever the car happens to be doing. Replacing it outright would nail your head to
// the horizon and throw away the car's roll.
//
// The fov matters as much as the pose. GPL renders 90 degrees horizontal; the runtime
// wants 104. Submit one as the other and the world moves 1.16 times too fast when you
// turn your head, which is a far more reliable way to make someone ill than any amount
// of judder. So the eye cameras are re-aimed at the runtime's own frustum -- see the
// derivation in the .cpp.

#include "presenter/SharedPresentation.h"

namespace gplvr {

struct VrEye {
    // The head's rotation relative to the recentre pose, in GPL LOCAL axes. Row-major,
    // columns are the axes -- the same convention as the scene block's p+24.
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    // This eye's position relative to the recentre pose, in GPL LOCAL axes, in metres.
    // A world unit is a metre, so no scale factor: STATUS.md, section 4.6.
    float offset[3] = {0, 0, 0};

    // What to write into the eye camera. fov is the FULL horizontal angle in radians;
    // aspect is the engine's stretch factor, where 1 means square pixels.
    float fov = 0.0f;
    float aspect = 1.0f;
};

// Reads the newest views the presenter published and converts them. False means VR is
// off, the presenter has not published yet, or the sample was caught mid-write -- in
// every case the caller should fall back to the flat IPD offset.
//
// eyeWidth/eyeHeight are the pixel dimensions of one eye's render target; the aspect
// depends on them.
bool vrAcquireEyes(VrEye out[2], int eyeWidth, int eyeHeight);

// Take the current head pose as the new origin. The driver's seat is wherever they were
// sitting when they asked, which is the only definition that survives someone shifting
// in their chair.
void vrRequestRecentre();

// The raw runtime views the last successful vrAcquireEyes consumed, unchanged and still
// in the presenter's own reference space. The producer echoes these back with the frame
// so the presenter can submit the pose the image was actually drawn from.
bool vrLastRenderedViews(SharedView out[2]);

// Dump one block of diagnostics on the next frame: the composed orientation matrix and
// whether it is still a proper rotation, the eye offsets, and the frustum written into
// the cameras. Meant to be triggered while LOOKING at whatever looks wrong -- a matrix
// that has stopped being orthonormal shears the world in proportion to how far the head
// is turned, which hides completely when you are facing forward.
void vrRequestDump();

// True when the head rotation is being replaced by the fixed debug rotation.
bool vrDebugRotationActive();

// Sweep that rotation, in degrees. Called once a frame from the key poll.
void vrDebugRotateBy(float yawDegrees, float pitchDegrees);

// Back to the starting angles from the ini.
void vrDebugRotateReset();

// The angles currently in force, for logging.
void vrDebugRotationAngles(float& yawDegrees, float& pitchDegrees);

// Forget the last rendered views. Must be called on any frame the eyes were NOT rendered
// from a head pose -- stereo switched off, a menu, a failed acquire. Otherwise the
// producer keeps publishing the last good pair, the presenter submits a frozen pose, and
// the runtime warps the image by an angle that grows the further you look away from it.
void vrClearRendered();

// True when the last vrAcquireEyes re-aimed the cameras at the runtime's frustum. The
// presenter needs to know: an image that already carries the runtime's exact fov must be
// stretched to fill the swapchain, not aspect-fitted into it.
bool vrFrustumInjected();

}  // namespace gplvr
