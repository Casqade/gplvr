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
    Quat  originOrientation = {0, 0, 0, 1};
    float originPosition[3] = {0, 0, 0};

    bool       injected = false;
    bool       haveRendered = false;
    SharedView rendered[2] = {};
    int  loggedFrustumWidth = 0;
    int  loggedFrustumHeight = 0;
    bool loggedAsymmetric = false;
    bool  loggedLookOverride = false;

    bool  lookSeeded = false;
    float lookYaw = 0.0f;
    float lookPitch = 0.0f;

    // Position prediction. acquireQpc is when the pose for the frame in progress was read,
    // renderTicks how long a frame then takes to be published, smoothed.
    LONGLONG  qpcFrequency = 0;
    LONGLONG  acquireQpc = 0;
    double    renderTicks = 0.0;
    bool      loggedNoPrediction = false;
    ULONGLONG predictReportTick = 0;
    int       predictFrames = 0;
    int       predictDisplays[4] = {};  // reached the display 1, 2, 3+ periods on
    double    predictShiftSum = 0.0;
};

State g_state;

// Move each eye to where it will be when the frame being rendered now is displayed.
//
// The runtime predicted these views for the display the presenter was preparing when it
// published them. This frame reaches a LATER display: it is picked up at the first
// presenter latch after it is published, and because GPL's frame is tied to its sim tick
// rather than to the headset, how many displays later varies from frame to frame. SteamVR
// reprojects the orientation of whatever it is handed, but has no depth to reproject
// position with, so eyes one display behind on one frame and two behind on the next make
// near geometry step while the head moves sideways.
//
// So the displays in between are counted, and each eye moves by its own velocity -- the
// head's, plus the head's rotation carrying the eye around it -- over that many periods.
// Orientation is left to reprojection, and the pose echoed with the frame carries the
// moved positions, so the image is reprojected from where it was really drawn.
constexpr double kMaxPredictSeconds = 0.05;
constexpr float  kMaxPredictMetres = 0.05f;

void predictEyePositions(SharedView views[2], const SharedPose& head,
                         const SharedMotion& motion, LONGLONG now) {
    if (!(motion.flags & kMotionTiming) || !(motion.flags & kMotionLinear)) {
        if (!g_state.loggedNoPrediction) {
            g_state.loggedNoPrediction = true;
            logf("vr: position prediction is waiting -- the presenter has not published %s",
                 (motion.flags & kMotionTiming)
                     ? "a head velocity"
                     : "the display clock (XR_KHR_win32_convert_performance_counter_time)");
        }
        return;
    }
    if (!g_state.qpcFrequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_state.qpcFrequency = frequency.QuadPart;
    }

    LONGLONG period = loadQpc(motion.period);
    LONGLONG latch = loadQpc(motion.latchTime);
    if (period <= 0 || g_state.qpcFrequency <= 0) return;

    // The first latch at or after this frame is ready. These views were read after the
    // latch they were published with, so at least one period always passes. The presenter
    // already located them viewLead displays ahead of that latch's display, which a paced
    // frame's latch matches, leaving nothing to add.
    LONGLONG ready = now + static_cast<LONGLONG>(g_state.renderTicks);
    LONGLONG displays = (ready - latch + period - 1) / period;
    if (displays < 1) displays = 1;
    if (motion.viewLead > 0) displays -= motion.viewLead;
    if (displays < 0) displays = 0;

    double seconds = static_cast<double>(displays * period) /
                     static_cast<double>(g_state.qpcFrequency);
    if (seconds > kMaxPredictSeconds) seconds = kMaxPredictSeconds;

    float angular[3] = {0.0f, 0.0f, 0.0f};
    if (motion.flags & kMotionAngular) {
        angular[0] = motion.angularVelocity[0];
        angular[1] = motion.angularVelocity[1];
        angular[2] = motion.angularVelocity[2];
    }

    float firstShift = 0.0f;
    for (int eye = 0; eye < 2; ++eye) {
        SharedPose& pose = views[eye].pose;
        float r[3] = {pose.positionX - head.positionX, pose.positionY - head.positionY,
                      pose.positionZ - head.positionZ};
        // v_eye = v_head + w x r
        float velocity[3] = {
            motion.linearVelocity[0] + angular[1] * r[2] - angular[2] * r[1],
            motion.linearVelocity[1] + angular[2] * r[0] - angular[0] * r[2],
            motion.linearVelocity[2] + angular[0] * r[1] - angular[1] * r[0]};
        float shift[3] = {static_cast<float>(velocity[0] * seconds),
                          static_cast<float>(velocity[1] * seconds),
                          static_cast<float>(velocity[2] * seconds)};
        float length =
            std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
        if (length > kMaxPredictMetres) {
            float scale = kMaxPredictMetres / length;
            shift[0] *= scale;
            shift[1] *= scale;
            shift[2] *= scale;
            length = kMaxPredictMetres;
        }
        pose.positionX += shift[0];
        pose.positionY += shift[1];
        pose.positionZ += shift[2];
        if (eye == 0) firstShift = length;
    }

    ++g_state.predictFrames;
    ++g_state.predictDisplays[displays < 3 ? displays : 3];
    g_state.predictShiftSum += firstShift;

    ULONGLONG tick = GetTickCount64();
    if (!g_state.predictReportTick) g_state.predictReportTick = tick;
    if (tick - g_state.predictReportTick >= 2000) {
        int frames = g_state.predictFrames;
        logf("vr: position prediction -- %d frames moved 0 periods past their views %d times, "
             "1 on %d, 2 on %d, 3 or more on %d; pose read to publish %.1f ms; mean eye shift "
             "%.2f mm",
             frames, g_state.predictDisplays[0], g_state.predictDisplays[1],
             g_state.predictDisplays[2], g_state.predictDisplays[3],
             g_state.renderTicks * 1000.0 / static_cast<double>(g_state.qpcFrequency),
             frames ? g_state.predictShiftSum / frames * 1000.0 : 0.0);
        g_state.predictReportTick = tick;
        g_state.predictFrames = 0;
        for (int& count : g_state.predictDisplays) count = 0;
        g_state.predictShiftSum = 0.0;
    }
}

// The presenter places seat-locked layers from this same pose, so a panel fixed to the seat
// and the eyes the game renders share one origin.
void publishOrigin() {
    SharedPose seat;
    seat.orientationX = g_state.originOrientation.x;
    seat.orientationY = g_state.originOrientation.y;
    seat.orientationZ = g_state.originOrientation.z;
    seat.orientationW = g_state.originOrientation.w;
    seat.positionX = g_state.originPosition[0];
    seat.positionY = g_state.originPosition[1];
    seat.positionZ = g_state.originPosition[2];
    presenterPublishSeat(seat);
}

// Take `head` as the seat: the recentre.
void captureOrigin(const SharedPose& head) {
    g_state.haveOrigin = true;
    g_state.recentreRequested = false;

    const Quat headQuat = {head.orientationX, head.orientationY, head.orientationZ,
                           head.orientationW};

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
    // Position and HEADING only -- never pitch, never roll. Not a preference:
    // OpenXR's LOCAL space is gravity-aligned by definition, every runtime's own
    // recentre (Meta, SteamVR, WMR) resets translation and yaw alone, and every sim
    // that ships a centre-view key does the same. The reason is vestibular: your
    // inner ear knows which way down is, and a virtual horizon that disagrees with
    // it is the strongest single trigger for simulator sickness. Yaw carries no such
    // conflict, because rotating about the gravity vector does not move "down".
    //
    // Measured here before this was unconditional: a head carrying just -1.0 degrees
    // of roll at recentre produced 5.2 degrees of roll at 89 degrees of head yaw. A
    // tilted origin does not merely offset the view, it poisons every later rotation
    // by giving the yaw a tilted axis to turn about.
    //
    // Someone wanting a head-down neutral posture needs a deliberate pitch offset
    // applied after this, which is how sims expose it -- not a tilted origin.
    const float forwardIn[3] = {0.0f, 0.0f, -1.0f};
    float       forward[3];
    rotate(headQuat, forwardIn, forward);

    // Looking straight up or down leaves no heading to recover; keep the one already
    // in force rather than snapping to an arbitrary one.
    const float planar =
        std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (planar > 0.01f) {
        const float yaw = std::atan2(-forward[0], -forward[2]);
        g_state.originOrientation = {0.0f, std::sin(yaw * 0.5f), 0.0f,
                                     std::cos(yaw * 0.5f)};
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
         "-- discarded, the seat keeps position and heading only");

    publishOrigin();
}

}  // namespace

bool vrSeatRelative(const SharedPose& pose, float position[3], float forward[3]) {
    if (!g_state.haveOrigin) return false;

    const Quat inverseOrigin = conjugate(g_state.originOrientation);

    if (position) {
        const float fromOrigin[3] = {pose.positionX - g_state.originPosition[0],
                                     pose.positionY - g_state.originPosition[1],
                                     pose.positionZ - g_state.originPosition[2]};
        float relative[3];
        rotate(inverseOrigin, fromOrigin, relative);
        xrToGpl(relative, position);

        // The same world scale the eye offsets get. Scaling a hand differently from an eye
        // would put the controller somewhere the driver is not reaching.
        const float scaleWorld =
            (g_options.vrWorldScale > 0.05f && g_options.vrWorldScale < 20.0f)
                ? g_options.vrWorldScale
                : 1.0f;
        position[0] *= scaleWorld;
        position[1] *= scaleWorld;
        position[2] *= scaleWorld;
    }

    if (forward) {
        const Quat  poseQuat = {pose.orientationX, pose.orientationY, pose.orientationZ,
                                pose.orientationW};
        const float aheadIn[3] = {0.0f, 0.0f, -1.0f};  // OpenXR: -Z is where a pose points
        float       turned[3];
        rotate(poseQuat, aheadIn, turned);
        float relative[3];
        rotate(inverseOrigin, turned, relative);
        xrToGpl(relative, forward);

        // Normalised here rather than trusted: the quaternion arrived over shared memory
        // and a caller dividing by a length of zero is a crash, not a wrong picture.
        const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] +
                                       forward[2] * forward[2]);
        if (length > 1e-6f) {
            forward[0] /= length;
            forward[1] /= length;
            forward[2] /= length;
        } else {
            forward[0] = 1.0f;
            forward[1] = 0.0f;
            forward[2] = 0.0f;
        }
    }
    return true;
}

bool vrSeatPose(SharedPose& out) {
    if (!g_state.haveOrigin) return false;
    out.orientationX = g_state.originOrientation.x;
    out.orientationY = g_state.originOrientation.y;
    out.orientationZ = g_state.originOrientation.z;
    out.orientationW = g_state.originOrientation.w;
    out.positionX = g_state.originPosition[0];
    out.positionY = g_state.originPosition[1];
    out.positionZ = g_state.originPosition[2];
    return true;
}

void vrRequestRecentre() { g_state.recentreRequested = true; }

bool vrPublishSeat() {
    if (g_state.haveOrigin) {
        publishOrigin();
        return true;
    }
    SharedView views[2] = {};
    SharedPose head = {};
    if (!presenterLatestViews(views, head)) return false;
    captureOrigin(head);
    return true;
}

bool vrLookOverrideActive() { return g_options.vrMouseLook; }

// yaw   about +Z (up),   positive turns left
// pitch about +Y (left); a positive rotation about +Y tilts forward toward -Z, which is
//       looking DOWN, so looking up is the negative angle
void vrLookOverrideMatrix(float out[9]) {
    if (!g_state.lookSeeded) vrLookOverrideReset();

    constexpr float kRadians = 0.017453292f;
    const float     a = g_state.lookYaw * kRadians;
    const float     b = -g_state.lookPitch * kRadians;
    const float     ca = std::cos(a), sa = std::sin(a);
    const float     cb = std::cos(b), sb = std::sin(b);

    // Rz(a) * Ry(b), row-major, matching how the callers multiply it.
    const float fixed[9] = {ca * cb, -sa,  ca * sb,
                            sa * cb, ca,   sa * sb,
                            -sb,     0.0f, cb};
    std::memcpy(out, fixed, sizeof(fixed));
}

bool vrApplyLookOverride(float orientation[9]) {
    if (!vrLookOverrideActive()) return false;

    float rotation[9];
    vrLookOverrideMatrix(rotation);

    // Right-multiplied, the same way the tracked path composes the head onto the car:
    // the override then acts in the camera's own axes, so yaw turns the view relative to
    // wherever the car is pointing rather than relative to the world.
    float composed[9];
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            composed[row * 3 + column] =
                orientation[row * 3 + 0] * rotation[0 * 3 + column] +
                orientation[row * 3 + 1] * rotation[1 * 3 + column] +
                orientation[row * 3 + 2] * rotation[2 * 3 + column];

    std::memcpy(orientation, composed, sizeof(composed));
    return true;
}

void vrLookOverrideBy(float yawDegrees, float pitchDegrees) {
    g_state.lookSeeded = true;
    g_state.lookYaw += yawDegrees;
    g_state.lookPitch += pitchDegrees;

    // Pitch past straight up or down stops being a pitch and starts being a roll, which
    // would confuse the very thing this is measuring.
    if (g_state.lookPitch > 89.0f) g_state.lookPitch = 89.0f;
    if (g_state.lookPitch < -89.0f) g_state.lookPitch = -89.0f;

    while (g_state.lookYaw > 180.0f) g_state.lookYaw -= 360.0f;
    while (g_state.lookYaw < -180.0f) g_state.lookYaw += 360.0f;
}

void vrLookOverrideReset() {
    g_state.lookSeeded = true;
    g_state.lookYaw = 0.0f;
    g_state.lookPitch = 0.0f;
}

void vrLookOverrideAngles(float& yawDegrees, float& pitchDegrees) {
    yawDegrees = g_state.lookYaw;
    pitchDegrees = g_state.lookPitch;
}

void vrClearRendered() {
    g_state.haveRendered = false;

    // Clear `injected` too, and this is the whole menu-scale bug.
    //
    // It is normally cleared at the top of vrAcquireEyes -- but a menu frame never
    // reaches vrAcquireEyes, because the viewport guard in ensureEyes returns first.
    // So after the first stereo frame it stayed true for the rest of the session, the
    // presenter was told stretchToFill, and vrMenuScale was skipped from then on.
    // Exactly "works the first time you enter the lobby, never again once you have
    // raced": before racing it had never been set.
    g_state.injected = false;
}

bool vrFrustumInjected() { return g_state.injected; }

void vrNoteFramePublished() {
    if (!g_state.acquireQpc) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ticks = static_cast<double>(now.QuadPart - g_state.acquireQpc);
    g_state.acquireQpc = 0;
    if (ticks <= 0.0) return;
    g_state.renderTicks =
        g_state.renderTicks > 0.0 ? g_state.renderTicks * 0.9 + ticks * 0.1 : ticks;
}

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
    SharedMotion motion;
    if (!presenterLatestViews(views, head, &motion)) return false;

    LARGE_INTEGER acquired;
    QueryPerformanceCounter(&acquired);
    g_state.acquireQpc = acquired.QuadPart;
    if (g_options.vrPredictPosition) predictEyePositions(views, head, motion, acquired.QuadPart);

    if (!g_state.haveOrigin || g_state.recentreRequested) captureOrigin(head);

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

        // The look override instead of the head's rotation, when one is active. Written
        // straight over the composed result rather than folded into the quaternion maths
        // above, so what the game receives is unambiguous: exactly this rotation, in GPL
        // axes, and nothing derived from tracking.
        if (vrLookOverrideActive()) {
            vrLookOverrideMatrix(out[eye].rotation);

            if (!g_state.loggedLookOverride) {
                g_state.loggedLookOverride = true;
                logf("vr: head rotation REPLACED by the look override -- head tracking "
                     "no longer turns the view, the mouse does. See vrMouseLook.");
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

        const float centreH = (tanRight + tanLeft) * 0.5f;
        const float centreV = (tanUp + tanDown) * 0.5f;
        const float halfH = (tanRight - tanLeft) * 0.5f;
        const float halfV = (tanUp - tanDown) * 0.5f;
        if (halfH <= 0.0f || halfV <= 0.0f) return false;

        // Record the frustum we are ACTUALLY about to render, not the one the runtime
        // asked for. The producer echoes this back with the frame and the presenter puts
        // it in the projection layer, so the compositor knows what the image really
        // covers.
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

    g_state.haveRendered = true;
    g_state.injected = true;
    return true;
}

}  // namespace gplvr
