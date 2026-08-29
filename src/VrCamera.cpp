#include "VrCamera.h"

#include "Hooks.h"
#include "Log.h"
#include "presenter/Producer.h"

#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

struct Quat {
    float x, y, z, w;
};

Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

// v' = v + 2w(qv x v) + 2 qv x (qv x v)
void rotate(const Quat& q, const float v[3], float out[3]) {
    const float tx = 2.0f * (q.y * v[2] - q.z * v[1]);
    const float ty = 2.0f * (q.z * v[0] - q.x * v[2]);
    const float tz = 2.0f * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

// The two frame conversions. Written as functions rather than a matrix because the
// matrix form is where sign errors hide.
void xrToGpl(const float xr[3], float gpl[3]) {
    gpl[0] = -xr[2];  // GPL forward is XR -Z
    gpl[1] = -xr[0];  // GPL left    is XR -X
    gpl[2] = xr[1];   // GPL up      is XR +Y
}

void gplToXr(const float gpl[3], float xr[3]) {
    xr[0] = -gpl[1];
    xr[1] = gpl[2];
    xr[2] = -gpl[0];
}

struct State {
    bool  haveOrigin = false;
    bool  recentreRequested = false;
    bool  dumpRequested = false;
    Quat  originOrientation = {0, 0, 0, 1};
    float originPosition[3] = {0, 0, 0};

    bool       injected = false;
    bool       haveRendered = false;
    SharedView rendered[2] = {};
    int  loggedFrustumWidth = 0;
    int  loggedFrustumHeight = 0;
    bool loggedAsymmetric = false;
    bool  loggedDebugRotation = false;
    bool  debugSeeded = false;
    float debugYaw = 0.0f;
    float debugPitch = 0.0f;
};

State g_state;

}  // namespace

void vrRequestRecentre() { g_state.recentreRequested = true; }

bool vrDebugRotationActive() {
    return g_options.vrDebugRotate || g_options.vrDebugYawDegrees != 0.0f ||
           g_options.vrDebugPitchDegrees != 0.0f;
}

void vrDebugRotateBy(float yawDegrees, float pitchDegrees) {
    if (!g_state.debugSeeded) {
        g_state.debugSeeded = true;
        g_state.debugYaw = g_options.vrDebugYawDegrees;
        g_state.debugPitch = g_options.vrDebugPitchDegrees;
    }
    g_state.debugYaw += yawDegrees;
    g_state.debugPitch += pitchDegrees;

    // Pitch past straight up or down stops being a pitch and starts being a roll, which
    // would confuse the very thing this is measuring.
    if (g_state.debugPitch > 89.0f) g_state.debugPitch = 89.0f;
    if (g_state.debugPitch < -89.0f) g_state.debugPitch = -89.0f;

    while (g_state.debugYaw > 180.0f) g_state.debugYaw -= 360.0f;
    while (g_state.debugYaw < -180.0f) g_state.debugYaw += 360.0f;
}

void vrDebugRotateReset() {
    g_state.debugSeeded = true;
    g_state.debugYaw = g_options.vrDebugYawDegrees;
    g_state.debugPitch = g_options.vrDebugPitchDegrees;
}

void vrDebugRotationAngles(float& yawDegrees, float& pitchDegrees) {
    yawDegrees = g_state.debugYaw;
    pitchDegrees = g_state.debugPitch;
}

void vrRequestDump() { g_state.dumpRequested = true; }

void vrClearRendered() { g_state.haveRendered = false; }

bool vrFrustumInjected() { return g_state.injected; }

bool vrLastRenderedViews(SharedView out[2]) {
    if (!g_state.haveRendered) return false;
    out[0] = g_state.rendered[0];
    out[1] = g_state.rendered[1];
    return true;
}

bool vrAcquireEyes(VrEye out[2], int eyeWidth, int eyeHeight) {
    g_state.injected = false;
    if (!g_options.vr || eyeWidth <= 0 || eyeHeight <= 0) return false;

    SharedView views[2] = {};
    // Built up per eye, then committed together. Writing straight into the shared state
    // would leave it half updated if the loop bails partway -- one eye from this frame
    // and one from the last, which is precisely the kind of mixed pair we have spent a
    // long time chasing.
    SharedView renderedLocal[2] = {};
    SharedPose head = {};
    if (!presenterLatestViews(views, head)) return false;

    const Quat headQuat = {head.orientationX, head.orientationY, head.orientationZ,
                           head.orientationW};

    if (!g_state.haveOrigin || g_state.recentreRequested) {
        g_state.haveOrigin = true;
        g_state.recentreRequested = false;
        // Keep only the HEADING. Discard the pitch and roll the head happened to
        // have when this fired.
        //
        // Measured: a seat captured with -4.2 degrees of pitch turned an 88.9 degree
        // head yaw into 5.2 degrees of roll, from a head carrying -1.0. Composing a
        // yaw against a tilted frame rotates about a tilted axis, and the tilt comes
        // back as roll that grows with yaw -- the "ladder", and the reason counter-
        // rolling never straightened it: the roll was manufactured, not observed.
        //
        // A seat is a heading. Its up axis is the world's up axis, always.
        if (g_options.vrLevelRecentre) {
            const float forwardIn[3] = {0.0f, 0.0f, -1.0f};
            float       forward[3];
            rotate(headQuat, forwardIn, forward);

            // Looking straight up or down leaves no heading to recover; keep the one
            // already in force rather than snapping to an arbitrary one.
            const float planar =
                std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
            if (planar > 0.01f) {
                const float yaw = std::atan2(-forward[0], -forward[2]);
                g_state.originOrientation = {0.0f, std::sin(yaw * 0.5f), 0.0f,
                                             std::cos(yaw * 0.5f)};
            }
        } else {
            g_state.originOrientation = headQuat;
        }
        g_state.originPosition[0] = head.positionX;
        g_state.originPosition[1] = head.positionY;
        g_state.originPosition[2] = head.positionZ;
        // Say what was thrown away, because a large pitch here means the recentre
        // fired while the head was somewhere unintended -- which is worth seeing.
        const float upIn[3] = {0.0f, 1.0f, 0.0f};
        const float fwdIn[3] = {0.0f, 0.0f, -1.0f};
        float       up[3], fwd[3], right[3];
        const float rightIn[3] = {1.0f, 0.0f, 0.0f};
        rotate(headQuat, upIn, up);
        rotate(headQuat, fwdIn, fwd);
        rotate(headQuat, rightIn, right);
        const float sinPitch = fwd[1] < -1.0f ? -1.0f : (fwd[1] > 1.0f ? 1.0f : fwd[1]);
        logf("vr: recentred on head (%.3f, %.3f, %.3f) -- this pose is now the seat. "
             "Head pitch %.1f roll %.1f deg %s",
             head.positionX, head.positionY, head.positionZ,
             std::asin(sinPitch) * 57.29578f,
             std::atan2(right[1], up[1]) * 57.29578f,
             g_options.vrLevelRecentre
                 ? "-- discarded, the seat keeps only the heading"
                 : "-- KEPT (vrLevelRecentre=0), so head yaw will turn this into roll");
    }

    const Quat inverseOrigin = conjugate(g_state.originOrientation);

    for (int eye = 0; eye < 2; ++eye) {
        const SharedView& view = views[eye];
        const Quat eyeQuat = {view.pose.orientationX, view.pose.orientationY,
                              view.pose.orientationZ, view.pose.orientationW};

        // -- rotation ------------------------------------------------------------
        // Build the matrix a column at a time by carrying each GPL basis vector into
        // XR, rotating it, and bringing it back. Cheaper to read than M * R * transpose(M),
        // and there is nowhere for a transposition to hide.
        for (int axis = 0; axis < 3; ++axis) {
            float basis[3] = {0.0f, 0.0f, 0.0f};
            basis[axis] = 1.0f;

            float asXr[3];
            gplToXr(basis, asXr);

            float turned[3];
            rotate(eyeQuat, asXr, turned);
            float relative[3];
            rotate(inverseOrigin, turned, relative);

            float asGpl[3];
            xrToGpl(relative, asGpl);
            out[eye].rotation[0 * 3 + axis] = asGpl[0];
            out[eye].rotation[1 * 3 + axis] = asGpl[1];
            out[eye].rotation[2 * 3 + axis] = asGpl[2];
        }

        // A fixed rotation instead of the head's, when asked for. Written straight
        // over the composed result rather than folded into the quaternion maths above,
        // so what the game receives is unambiguous: exactly this rotation, in GPL axes,
        // and nothing derived from tracking.
        //
        //   yaw   about +Z (up),   positive turns left
        //   pitch about +Y (left); a positive rotation about +Y tilts forward toward
        //         -Z, which is looking DOWN, so looking up is the negative angle
        if (vrDebugRotationActive()) {
            if (!g_state.debugSeeded) vrDebugRotateReset();

            constexpr float kRadians = 0.017453292f;
            const float     a = g_state.debugYaw * kRadians;
            const float     b = -g_state.debugPitch * kRadians;
            const float     ca = std::cos(a), sa = std::sin(a);
            const float     cb = std::cos(b), sb = std::sin(b);

            // Rz(a) * Ry(b), row-major, matching how the caller multiplies it.
            const float fixed[9] = {ca * cb, -sa, ca * sb,
                                    sa * cb, ca,  sa * sb,
                                    -sb,     0.0f, cb};
            std::memcpy(out[eye].rotation, fixed, sizeof(fixed));

            if (!g_state.loggedDebugRotation) {
                g_state.loggedDebugRotation = true;
                logf("vr: head rotation REPLACED by a fixed rotation, starting at yaw "
                     "%.1f deg pitch %.1f deg. Tracking no longer turns the view -- "
                     "hold the arrow keys to sweep it and judge the picture on the flat "
                     "monitor. If straight lines shear as it sweeps, GPL's own view "
                     "transform does it and the headset is not involved.",
                     g_state.debugYaw, g_state.debugPitch);
            }
        }

        // -- position ------------------------------------------------------------
        const float fromOrigin[3] = {view.pose.positionX - g_state.originPosition[0],
                                     view.pose.positionY - g_state.originPosition[1],
                                     view.pose.positionZ - g_state.originPosition[2]};
        float relative[3];
        rotate(inverseOrigin, fromOrigin, relative);
        xrToGpl(relative, out[eye].offset);

        // Metres into GPL units. Scales the eye separation and the head translation by
        // the same factor -- they are the same quantity as far as the world's apparent
        // size is concerned, and scaling only one of them would make the stereo and the
        // parallax disagree, which is worse than either being wrong.
        const float scaleWorld =
            (g_options.vrWorldScale > 0.05f && g_options.vrWorldScale < 20.0f)
                ? g_options.vrWorldScale
                : 1.0f;
        out[eye].offset[0] *= scaleWorld;
        out[eye].offset[1] *= scaleWorld;
        out[eye].offset[2] *= scaleWorld;

        // -- frustum -------------------------------------------------------------
        // From gpl_cameraRebuildProjection, with the camera layout from
        // gpl_cameraCreate:
        //
        //   screen_x = cx - cot(fov/2) * (y/x)
        //   screen_y = cy - (w / (aspect*h)) * cot(fov/2) * (z/x)
        //
        // Screen edges are at +/-1, so the horizontal half-angle is exactly fov/2 --
        // `fov` is the FULL horizontal field of view -- and
        //
        //   tan(vertical half-angle) = tan(fov/2) * aspect * h / w
        //
        // which makes `aspect` a stretch factor, not a viewport ratio: aspect = 1 is
        // square pixels whatever the window shape. Phase 1 passed w/(2h), copied from
        // the mirror's setup call, and that was wrong -- it squashed every eye.
        const float tanLeft = std::tan(view.fov.angleLeft);
        const float tanRight = std::tan(view.fov.angleRight);
        const float tanUp = std::tan(view.fov.angleUp);
        const float tanDown = std::tan(view.fov.angleDown);

        // Per-eye, so the two halves can be made to disagree on purpose. Rendering
        // the left eye at 1.0 and the right at 0.5 puts the answer to "does fov reach
        // the picture at all" on the flat monitor, side by side, with no headset and no
        // theory: different world extents means it does, identical halves means it
        // does not. vrFovScaleRight <= 0 means "same as vrFovScale".
        float configured = g_options.vrFovScale;
        if (eye == 1 && g_options.vrFovScaleRight > 0.1f &&
            g_options.vrFovScaleRight < 4.0f)
            configured = g_options.vrFovScaleRight;
        const float scale =
            (configured > 0.1f && configured < 4.0f) ? configured : 1.0f;
        const float centreH = (tanRight + tanLeft) * 0.5f;
        const float centreV = (tanUp + tanDown) * 0.5f;
        const float halfH = (tanRight - tanLeft) * 0.5f * scale;
        const float halfV = (tanUp - tanDown) * 0.5f * scale;
        if (halfH <= 0.0f || halfV <= 0.0f) return false;

        // Record the frustum we are ACTUALLY about to render, not the one the runtime
        // asked for. The producer echoes this back with the frame and the presenter puts
        // it in the projection layer, so the compositor knows what the image really
        // covers. Skipping this would tell it a 115%-wide image is 100% wide and have it
        // warp accordingly -- reintroducing, through the fix, exactly the distortion
        // vrFovScale exists to reduce.
        renderedLocal[eye] = views[eye];
        renderedLocal[eye].fov.angleLeft = std::atan(centreH - halfH);
        renderedLocal[eye].fov.angleRight = std::atan(centreH + halfH);
        renderedLocal[eye].fov.angleUp = std::atan(centreV + halfV);
        renderedLocal[eye].fov.angleDown = std::atan(centreV - halfV);

        out[eye].fov = 2.0f * std::atan(halfH);
        out[eye].aspect = (halfV / halfH) * (static_cast<float>(eyeWidth) /
                                             static_cast<float>(eyeHeight));

        // GPL's frustum is symmetric about cx/cy, and cx/cy are baked in at
        // cameraCreate time. A runtime with an off-centre frustum would need them
        // rewritten too; say so once rather than quietly rendering the wrong view.
        if (!g_state.loggedAsymmetric &&
            (std::fabs(tanRight + tanLeft) > 1e-3f ||
             std::fabs(tanUp + tanDown) > 1e-3f)) {
            g_state.loggedAsymmetric = true;
            logf("vr: eye %d has an ASYMMETRIC frustum (tan L%.4f R%.4f U%.4f D%.4f). "
                 "The eye cameras are built with cx=cy=0, so the view is centred where "
                 "the runtime does not want it. This needs cameraCreate's cx/cy.",
                 eye, tanLeft, tanRight, tanUp, tanDown);
        }
    }

    // Re-log whenever the eye size changes rather than once ever. Logging it once meant
    // the only line in the file described the 186x280 lobby feed, which is not the view
    // anyone is trying to judge.
    if (eyeWidth != g_state.loggedFrustumWidth ||
        eyeHeight != g_state.loggedFrustumHeight) {
        g_state.loggedFrustumWidth = eyeWidth;
        g_state.loggedFrustumHeight = eyeHeight;
        logf("vr: eye frustum from the runtime -- fov %.4f rad (%.1f deg) full "
             "horizontal, aspect %.4f for a %dx%d eye",
             out[0].fov, out[0].fov * 57.29578f, out[0].aspect, eyeWidth, eyeHeight);
        // Both eyes, because vrFovScaleRight makes them differ on purpose and a line
        // that only ever prints eye 0 cannot show that the split took effect.
        logf("vr: eye 1 frustum -- fov %.4f rad (%.1f deg), aspect %.4f", out[1].fov,
             out[1].fov * 57.29578f, out[1].aspect);
        logf("vr: eye offsets left (%.4f, %.4f, %.4f) right (%.4f, %.4f, %.4f) metres "
             "in GPL axes -- separation %.4f",
             out[0].offset[0], out[0].offset[1], out[0].offset[2], out[1].offset[0],
             out[1].offset[1], out[1].offset[2],
             std::sqrt((out[1].offset[0] - out[0].offset[0]) *
                           (out[1].offset[0] - out[0].offset[0]) +
                       (out[1].offset[1] - out[0].offset[1]) *
                           (out[1].offset[1] - out[0].offset[1]) +
                       (out[1].offset[2] - out[0].offset[2]) *
                           (out[1].offset[2] - out[0].offset[2])));
    }

    // rendered[] was filled per eye above, with the scaled fov. Overwriting it here with
    // the raw views would undo that.
    g_state.rendered[0] = renderedLocal[0];
    g_state.rendered[1] = renderedLocal[1];

    if (g_state.dumpRequested) {
        g_state.dumpRequested = false;
        const float* r = out[0].rotation;

        // Column norms and the dot products between columns. A proper rotation has all
        // three norms at 1.000 and all three dots at 0.000. Anything else shears the
        // world in proportion to how far the head is turned -- which is invisible facing
        // forward and obvious at 45 degrees, exactly the reported symptom.
        const float n0 = std::sqrt(r[0]*r[0] + r[3]*r[3] + r[6]*r[6]);
        const float n1 = std::sqrt(r[1]*r[1] + r[4]*r[4] + r[7]*r[7]);
        const float n2 = std::sqrt(r[2]*r[2] + r[5]*r[5] + r[8]*r[8]);
        const float d01 = r[0]*r[1] + r[3]*r[4] + r[6]*r[7];
        const float d02 = r[0]*r[2] + r[3]*r[5] + r[6]*r[8];
        const float d12 = r[1]*r[2] + r[4]*r[5] + r[7]*r[8];

        // r[0] is the forward axis' forward component, so acos of it is how far the head
        // has turned away from the seat.
        const float turn = std::acos(r[0] < -1.0f ? -1.0f : (r[0] > 1.0f ? 1.0f : r[0]));

        logf("vrdump: head turned %.1f deg from the seat", turn * 57.29578f);
        logf("vrdump: head matrix [%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f]",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
        logf("vrdump: column norms %.5f %.5f %.5f (want 1) dots %.5f %.5f %.5f (want 0)",
             n0, n1, n2, d01, d02, d12);
        logf("vrdump: eye offsets L(%.4f %.4f %.4f) R(%.4f %.4f %.4f) m, fwd/left/up",
             out[0].offset[0], out[0].offset[1], out[0].offset[2],
             out[1].offset[0], out[1].offset[1], out[1].offset[2]);
        logf("vrdump: written to the cameras -- fov %.4f rad (%.1f deg), aspect %.4f, "
             "eye %dx%d",
             out[0].fov, out[0].fov * 57.29578f, out[0].aspect, eyeWidth, eyeHeight);

        // Where the ROLL is.
        //
        // A horizon overlay drawn from directions at infinity through the XR pose can
        // only tilt if that pose carries roll. So take the roll apart at every stage
        // and print it, instead of arguing about which stage it must be in:
        //
        //   raw     -- the eye pose the runtime reports, in LOCAL space
        //   seat    -- the recentre origin
        //   head    -- raw relative to seat, i.e. what a head is actually doing
        //   in GPL  -- the same rotation after the frame conversion, ready to compose
        //
        // If "raw" carries roll and "head" does not, the seat is tilted. If both do,
        // the head really is rolled and the overlay is telling the truth. If neither
        // does but the picture tilts, the roll is being introduced after this point.
        constexpr float kDeg = 57.29578f;

        // Report the euler angles from a rotation's own axes, so the same routine can
        // describe an XR quaternion and a GPL matrix without a conversion in between.
        auto reportXr = [](const char* label, const Quat& q) {
            const float fwdIn[3] = {0.0f, 0.0f, -1.0f};
            const float upIn[3] = {0.0f, 1.0f, 0.0f};
            const float rightIn[3] = {1.0f, 0.0f, 0.0f};
            float fwd[3], up[3], right[3];
            rotate(q, fwdIn, fwd);
            rotate(q, upIn, up);
            rotate(q, rightIn, right);
            const float sinPitch = fwd[1] < -1.0f ? -1.0f : (fwd[1] > 1.0f ? 1.0f : fwd[1]);
            logf("vrdump: %-34s yaw %7.1f  pitch %6.1f  roll %6.1f deg", label,
                 std::atan2(-fwd[0], -fwd[2]) * 57.29578f,
                 std::asin(sinPitch) * 57.29578f,
                 std::atan2(right[1], up[1]) * 57.29578f);
        };

        const Quat rawQuat{views[0].pose.orientationX, views[0].pose.orientationY,
                           views[0].pose.orientationZ, views[0].pose.orientationW};

        // conj(origin) * raw -- the head relative to the seat, which is the only part
        // that should reach the game.
        const Quat inv = conjugate(g_state.originOrientation);
        const Quat relative{
            inv.w * rawQuat.x + inv.x * rawQuat.w + inv.y * rawQuat.z - inv.z * rawQuat.y,
            inv.w * rawQuat.y - inv.x * rawQuat.z + inv.y * rawQuat.w + inv.z * rawQuat.x,
            inv.w * rawQuat.z + inv.x * rawQuat.y - inv.y * rawQuat.x + inv.z * rawQuat.w,
            inv.w * rawQuat.w - inv.x * rawQuat.x - inv.y * rawQuat.y - inv.z * rawQuat.z};

        reportXr("eye pose in LOCAL (raw)", rawQuat);
        reportXr("seat / recentre origin", g_state.originOrientation);
        reportXr("head relative to the seat", relative);

        // And the same rotation once it is in GPL's frame: columns are the camera axes,
        // X forward, Y left, Z up, so column 1's up-component is the roll.
        const float sinPitchGpl = r[6] < -1.0f ? -1.0f : (r[6] > 1.0f ? 1.0f : r[6]);
        // Negated: GPL's roll is measured from the LEFT axis and XR's from the RIGHT,
        // so the raw figures differ in sign by construction. The first version of this
        // dump printed both and made a clean conversion look like a sign bug.
        logf("vrdump: %-34s yaw %7.1f  pitch %6.1f  roll %6.1f deg",
             "handed to GPL (same rotation)", std::atan2(r[3], r[0]) * kDeg,
             std::asin(sinPitchGpl) * kDeg, -std::atan2(r[7], r[8]) * kDeg);
        logf("vrdump: a horizon drawn from infinity can only tilt if a pose carries "
             "roll. If every roll above is ~0 while the picture ladders, the tilt is "
             "added after this point -- not by the head, and not by us here.");
    }

    g_state.haveRendered = true;
    g_state.injected = true;
    return true;
}

}  // namespace gplvr
