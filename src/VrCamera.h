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

// A pose the presenter published, in the seat's frame and GPL's axes.
//
// The same conversion vrAcquireEyes applies to an eye, exposed so a motion controller goes
// through it too: `position` is where the pose is relative to the seat, in GPL axes and in
// GPL units (vrWorldScale applied, exactly as an eye offset is), and `forward` is the unit
// vector the pose points along -- OpenXR's -Z, which for an aim pose is the controller's
// own pointing ray -- in the same axes.
//
// ONE definition for hands and eyes. A second copy of this arithmetic is a second place
// for a sign error, and the seat capture's own history (a tilted origin poisoning every
// later rotation) is what that costs.
//
// False before the first recentre, when there is no seat to be relative to. Either output
// pointer may be null.
bool vrSeatRelative(const SharedPose& pose, float position[3], float forward[3]);

// Take the current head pose as the new origin. The driver's seat is wherever they were
// sitting when they asked, which is the only definition that survives someone shifting
// in their chair.
void vrRequestRecentre();

// The seat itself, in the presenter's LOCAL space: position and heading, as captureOrigin
// left it. False before the first recentre.
//
// The presenter builds its seat-locked reference space from exactly this pose, so anything
// on the game side that wants to place something against a seat-locked layer -- the HUD
// panel, say -- reads it from here rather than keeping a second copy.
bool vrSeatPose(SharedPose& out);

// Publish the seat now, for a layer that needs one: the recentre pose if there is one,
// otherwise the newest head pose the presenter published, taken as the recentre is. False
// when there is neither.
bool vrPublishSeat();

// The runtime views the last successful vrAcquireEyes rendered from, still in the
// presenter's own reference space: orientation and fov as published, eye positions moved
// forward by the position prediction when it is on. The producer echoes these back with the frame
// so the presenter can submit the pose the image was actually drawn from.
bool vrLastRenderedViews(SharedView out[2]);

// True when the head rotation is being replaced by a look override -- the fixed debug
// rotation, or the mouse.
bool vrLookOverrideActive();

// That override as a 3x3 in GPL axes, row-major. ONE definition, used both by the VR
// path (which writes it over the head rotation) and by the flat path (which composes it
// onto the game's own camera), so the two cannot drift apart.
void vrLookOverrideMatrix(float out[9]);

// Compose the override onto a camera orientation, in place, right-multiplied so it acts
// in camera-local axes exactly as a head rotation does. Returns false and leaves the
// matrix untouched when no override is active.
//
// This is the flat-monitor path. With vr = 0 there is no presenter and vrAcquireEyes
// never runs, so the override has to be applied where the scene parameters pass by --
// which is what makes the distortion judgeable on a monitor, where the headset's mirror
// preview is letterboxed and shrunk and cannot settle anything.
bool vrApplyLookOverride(float orientation[9]);

// Sweep that rotation, in degrees. Called once a frame from the key poll.
void vrLookOverrideBy(float yawDegrees, float pitchDegrees);

// Back to the starting angles from the ini.
void vrLookOverrideReset();

// The angles currently in force, for logging.
void vrLookOverrideAngles(float& yawDegrees, float& pitchDegrees);

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
