#include "Stereo.h"

#include "Hooks.h"
#include "Log.h"
#include "VrCamera.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace gplvr {

// The rasterizer entry points stereo uses. All of them are __cdecl: gpl_simFrameRender
// calls them as `push args; call [global]; add esp, n`.
typedef int(__cdecl* BitmapWidthFn)(void* bitmap);
typedef int(__cdecl* BitmapHeightFn)(void* bitmap);
typedef int(__cdecl* BitmapInstanceAllocFn)(void** out, void* source, int x, int y, int w,
                                            int h);
typedef void(__cdecl* BitmapInstanceFreeFn)(void* instance);

// gpl_cameraCreate is __thiscall: the camera object is caller-allocated and arrives in
// ecx, the six arguments are pushed, and the callee cleans up (`retn 18h`).
typedef void*(__thiscall* CameraCreateFn)(void* self, void* surface, float fov, float cx,
                                          float cy, float nearZ, float aspect);

// gpl_cameraBind is __cdecl and takes the camera. Line for line it rebuilds the
// projection if the dirty byte is set, then calls polySetDest(camera->surface,
// camera->fov) -- so binding a camera IS a destination change, which is the whole reason
// we need it here.
typedef int(__cdecl* CameraBindFn)(void* camera);

namespace {

struct Entries {
    // The rasterizer entries are held as their GLOBALS, not as resolved pointers. The
    // table is filled in by GetProcAddress after we install and could be re-resolved on
    // a mode change, so every call reads the slot's current value.
    void** bitmapWidthSlot = nullptr;
    void** bitmapHeightSlot = nullptr;
    void** instanceAllocSlot = nullptr;
    void** instanceFreeSlot = nullptr;

    // Both are code in the exe, so their addresses are fixed once discovered.
    CameraCreateFn cameraCreate = nullptr;
    CameraBindFn   cameraBind = nullptr;
};

Entries g_entries;

// One half of the back buffer and the camera that draws into it. Which eye's viewpoint
// a half receives is decided per frame, so these are indexed by buffer half, not by eye.
struct Half {
    void* surface = nullptr;
    void* camera = nullptr;
};

struct State {
    void* sourceSurface = nullptr;
    int   sourceWidth = 0;
    int   sourceHeight = 0;
    Half  half[2];  // [0] = left half of the buffer, [1] = right half
    bool  reported = false;
    bool  gaveUp = false;
    bool  verifiedFrustum = false;
    bool  verifiedRebuild[2] = {false, false};

    // One-shot fov probe. `requested` is set from the key hook, `armed` by the render
    // once the probe frame is sitting in the back buffer.
    bool  fovProbeRequested = false;
    bool  fovProbeArmed = false;
    float fovProbeFov[2] = {0.0f, 0.0f};
    float fovProbeAspect[2] = {0.0f, 0.0f};

    // The frustum each eye camera should be rendering with, kept so the cameraBind hook
    // can put it back after sceneRender overwrites it.
    float pendingFov[2] = {0.0f, 0.0f};
    float pendingAspect[2] = {0.0f, 0.0f};
    bool  pendingValid[2] = {false, false};
    bool  loggedReapply = false;

    // Watchdog counters. The fov was silently overridden once already; "it is fixed"
    // is worth exactly as much as the instrument that keeps saying so.
    int   fovOverrideReports = 0;
    int   untrackedReports = 0;
    bool  wasTracked = false;
    int   seatReports = 0;
    int   eulerReports = 0;
    float worstEuler = 0.0f;
    float lastSeatPitch = 999.0f;

    // gpl_cameraCreate does not allocate; the engine passes it a static buffer. So do
    // we. 512 is four times what the object needs.
    alignas(16) unsigned char storage[2][512] = {};
};

State g_state;

int bitmapWidth(void* bitmap) {
    auto fn = reinterpret_cast<BitmapWidthFn>(*g_entries.bitmapWidthSlot);
    return fn ? fn(bitmap) : 0;
}

int bitmapHeight(void* bitmap) {
    auto fn = reinterpret_cast<BitmapHeightFn>(*g_entries.bitmapHeightSlot);
    return fn ? fn(bitmap) : 0;
}

float cameraFloat(void* camera, unsigned offset) {
    return *reinterpret_cast<const float*>(static_cast<const unsigned char*>(camera) +
                                           offset);
}

void* cameraPointer(void* camera, unsigned offset) {
    return *reinterpret_cast<void* const*>(static_cast<const unsigned char*>(camera) +
                                           offset);
}

// Re-aim an existing camera. gpl_cameraRebuildProjection is lazy -- it does nothing
// unless the byte at +0x3C is set -- and cameraBind calls it on the way into every
// render, so writing the two floats and raising the flag is the whole job. Rebuilding
// the camera object would also work and would cost a bitmapInstanceAlloc per frame.
void cameraSetFrustum(void* camera, float fov, float aspect) {
    auto bytes = static_cast<unsigned char*>(camera);
    *reinterpret_cast<float*>(bytes + kCameraFov) = fov;
    *reinterpret_cast<float*>(bytes + kCameraAspect) = aspect;
    bytes[kCameraDirty] = 1;
}

void releaseHalf(Half& target) {
    if (target.surface && g_entries.instanceFreeSlot) {
        if (auto fn = reinterpret_cast<BitmapInstanceFreeFn>(*g_entries.instanceFreeSlot))
            fn(target.surface);
    }
    target.surface = nullptr;
    target.camera = nullptr;
}

// Builds one half: a sub-rect view of the render target, and a camera aimed at it.
// fov and near are copied from the main camera so the eyes match whatever the game is
// currently doing -- including a cockpit FOV the player has changed.
bool buildHalf(Half& target, unsigned char* storage, void* source, int x, int y, int w,
               int h, float fov, float nearZ) {
    auto alloc = reinterpret_cast<BitmapInstanceAllocFn>(*g_entries.instanceAllocSlot);
    if (!alloc) return false;

    void* instance = nullptr;
    if (!alloc(&instance, source, x, y, w, h) || !instance) {
        logf("stereo: bitmapInstanceAlloc(%p, %d,%d %dx%d) failed", source, x, y, w, h);
        return false;
    }

    // Ask the instance for its own size rather than assuming it honoured the request.
    const int actualW = bitmapWidth(instance);
    const int actualH = bitmapHeight(instance);
    if (actualW <= 0 || actualH <= 0) {
        logf("stereo: instance %p reports a %dx%d size", instance, actualW, actualH);
        if (auto release = reinterpret_cast<BitmapInstanceFreeFn>(*g_entries.instanceFreeSlot))
            release(instance);
        return false;
    }
    // aspect = 1 means SQUARE PIXELS, whatever the viewport shape.
    //
    // Phase 1 passed width / (2 * height) here, copied from the mirror camera's setup
    // call, and that was a misreading. gpl_cameraRebuildProjection gives
    //   tan(vertical half-angle) = tan(fov/2) * aspect * height / width
    // so `aspect` is a stretch factor and 1 is undistorted. w/(2h) squashed each
    // 1280x1440 half by 2.25 times vertically. With VR on this is overwritten per frame
    // with the runtime's own frustum anyway; this is the value the flat mode uses.
    const float aspect = 1.0f;

    std::memset(storage, 0, kCameraSize);
    void* camera = g_entries.cameraCreate(storage, instance, fov, 0.0f, 0.0f, nearZ, aspect);
    if (!camera) {
        if (auto release = reinterpret_cast<BitmapInstanceFreeFn>(*g_entries.instanceFreeSlot))
            release(instance);
        return false;
    }

    target.surface = instance;
    target.camera = camera;
    logf("stereo: eye target %d,%d %dx%d -> surface %p camera %p (fov %.4f rad, near %.4f, "
         "aspect %.4f)",
         x, y, actualW, actualH, instance, camera, fov, nearZ, aspect);
    return true;
}

// Rebuilds the eye targets if the main camera's render target has changed. Cheap: it
// only ever does work on the first world frame and after a resolution change.
bool ensureEyes(void* mainCamera) {
    if (g_state.gaveUp) return false;

    void* source = cameraPointer(mainCamera, kCameraSurface);
    if (!source) return false;

    const int width = bitmapWidth(source);
    const int height = bitmapHeight(source);
    if (width < 2 || height < 1) return false;

    if (g_state.half[0].camera && source == g_state.sourceSurface &&
        width == g_state.sourceWidth && height == g_state.sourceHeight)
        return true;

    stereoRelease();

    const float fov = cameraFloat(mainCamera, kCameraFov);
    const float nearZ = cameraFloat(mainCamera, kCameraNear);
    const int   halfWidth = width / 2;

    logf("stereo: building eye targets from surface %p, %dx%d -> two %dx%d halves", source,
         width, height, halfWidth, height);
    // The game's own camera, for comparison. This is the check on the projection reading
    // in buildHalf: if GPL passes something near 1.0 for a full-screen camera then
    // `aspect` really is a stretch factor, and if it passes width/(2*height) the reading
    // is wrong and the eyes are being squashed on purpose.
    logf("stereo: the game's main camera has fov %.4f rad, aspect %.4f, half extents "
         "%.1f x %.1f",
         fov, cameraFloat(mainCamera, kCameraAspect),
         cameraFloat(mainCamera, 0x0C), cameraFloat(mainCamera, 0x10));

    if (!buildHalf(g_state.half[0], g_state.storage[0], source, 0, 0, halfWidth, height,
                   fov, nearZ) ||
        !buildHalf(g_state.half[1], g_state.storage[1], source, halfWidth, 0, halfWidth,
                   height, fov, nearZ)) {
        stereoRelease();
        // One failure is informative; a failure every frame is a log flood.
        g_state.gaveUp = true;
        logf("stereo: could not build the eye targets -- falling back to a single render");
        return false;
    }

    g_state.sourceSurface = source;
    g_state.sourceWidth = width;
    g_state.sourceHeight = height;
    return true;
}

}  // namespace

void localToWorld(const float m[9], float lx, float ly, float lz, float& wx, float& wy,
                  float& wz) {
    wx = m[0] * lx + m[1] * ly + m[2] * lz;
    wy = m[3] * lx + m[4] * ly + m[5] * lz;
    wz = m[6] * lx + m[7] * ly + m[8] * lz;
}

void stereoBind(const Binding& binding) {
    g_entries = Entries{};

    g_entries.bitmapWidthSlot = binding.raster.bitmapWidth;
    g_entries.bitmapHeightSlot = binding.raster.bitmapHeight;
    g_entries.instanceAllocSlot = binding.raster.bitmapInstanceAlloc;
    g_entries.instanceFreeSlot = binding.raster.bitmapInstanceFree;
    g_entries.cameraCreate = reinterpret_cast<CameraCreateFn>(binding.cameraCreate);
    g_entries.cameraBind = reinterpret_cast<CameraBindFn>(binding.cameraBind);

    if (!stereoAvailable()) {
        logf("stereo: unavailable -- bitmapWidth=%p bitmapHeight=%p instanceAlloc=%p "
             "instanceFree=%p cameraCreate=%08X",
             g_entries.bitmapWidthSlot, g_entries.bitmapHeightSlot,
             g_entries.instanceAllocSlot, g_entries.instanceFreeSlot,
             static_cast<unsigned>(binding.cameraCreate));
        return;
    }
    logf("stereo: ready -- cameraCreate=%08X", static_cast<unsigned>(binding.cameraCreate));
}

bool stereoAvailable() {
    return g_entries.bitmapWidthSlot && g_entries.bitmapHeightSlot &&
           g_entries.instanceAllocSlot && g_entries.instanceFreeSlot &&
           g_entries.cameraCreate;
}

void stereoOnCameraBind(void* camera) {
    for (int pass = 0; pass < 2; ++pass) {
        if (!g_state.pendingValid[pass] || camera != g_state.half[pass].camera) continue;

        cameraSetFrustum(camera, g_state.pendingFov[pass], g_state.pendingAspect[pass]);

        if (!g_state.loggedReapply) {
            g_state.loggedReapply = true;
            logf("stereo: eye frustum re-applied at bind -- fov %.4f aspect %.4f. Before "
                 "this hook sceneRender's own cameraSetFov won and the eyes rendered the "
                 "game's field of view.",
                 g_state.pendingFov[pass], g_state.pendingAspect[pass]);
        }
        return;
    }
}

void stereoRequestFovProbe() {
    g_state.fovProbeRequested = true;
    logf("stereo: fov probe armed -- the next tracked frame renders both halves from "
         "one viewpoint, left at the normal frustum and right at twice its tangent");
}

bool stereoTakeFovProbe(float fov[2], float aspect[2]) {
    if (!g_state.fovProbeArmed) return false;
    g_state.fovProbeArmed = false;
    fov[0] = g_state.fovProbeFov[0];
    fov[1] = g_state.fovProbeFov[1];
    aspect[0] = g_state.fovProbeAspect[0];
    aspect[1] = g_state.fovProbeAspect[1];
    return true;
}

int stereoEyeWidth() {
    return g_state.half[0].camera ? g_state.sourceWidth / 2 : 0;
}

void stereoRelease() {
    releaseHalf(g_state.half[0]);
    releaseHalf(g_state.half[1]);
    g_state.sourceSurface = nullptr;
    g_state.sourceWidth = 0;
    g_state.sourceHeight = 0;
}

// GPL's own matrix-to-euler, transcribed from gpl_matrixToEuler (0x004E4ED0).
//
// This matters because gpl_sceneRender does NOT render from the orientation matrix we
// hand it. It calls this, then rebuilds the view from the three angles:
//
//     gpl_matrixToEuler(&yaw, &pitch, &roll, params->orientation);
//     gpl_setViewTransform(eyeX, eyeY, eyeZ, yaw, pitch, roll);
//
// So our matrix survives only as far as this decomposition preserves it. It is a
// standard ZYX decomposition and should round-trip exactly -- but "should" is what has
// been wrong every other time, and there is a degenerate branch here when the forward
// axis approaches vertical, where the roll extraction divides by a norm going to zero.
void gplMatrixToEuler(const float m[9], float& yaw, float& pitch, float& roll) {
    yaw = std::atan2(m[3], m[0]);
    const float norm = std::sqrt(m[3] * m[3] + m[0] * m[0]);
    pitch = std::atan2(-m[6], norm);

    if (norm < 1e-6f) {
        // Gimbal lock: looking straight up or down, yaw and roll are the same axis.
        roll = yaw - std::atan2(m[5], m[2]);
        if (m[6] > 0.0f) roll = 3.1415927f - roll;
        return;
    }
    const float cosYaw = m[0] / norm;
    const float sinYaw = -m[3] / norm;
    roll = std::atan2(-(m[5] * cosYaw + m[2] * sinYaw), m[4] * cosYaw + m[1] * sinYaw);
}

// Rebuild in the order the decomposition implies, so the two can be compared.
void gplEulerToMatrix(float yaw, float pitch, float roll, float out[9]) {
    const float cz = std::cos(yaw), sz = std::sin(yaw);
    const float cy = std::cos(pitch), sy = std::sin(pitch);
    const float cx = std::cos(roll), sx = std::sin(roll);

    out[0] = cz * cy;
    out[1] = cz * sy * sx - sz * cx;
    out[2] = cz * sy * cx + sz * sx;
    out[3] = sz * cy;
    out[4] = sz * sy * sx + cz * cx;
    out[5] = sz * sy * cx - cz * sx;
    out[6] = -sy;
    out[7] = cy * sx;
    out[8] = cy * cx;
}

bool stereoRender(SceneParams* params, void(__cdecl* original)(void*)) {
    if (!stereoAvailable() || !params || !params->camera) return false;
    if (!ensureEyes(params->camera)) return false;

    void* const savedCamera = params->camera;
    void* const savedMirror = params->mirrorCamera;
    const float savedX = params->eyeX;
    const float savedY = params->eyeY;
    const float savedZ = params->eyeZ;
    float       savedOrientation[9];
    std::memcpy(savedOrientation, params->orientation, sizeof(savedOrientation));


    const float halfIpd = g_options.stereoIpd * 0.5f;

    // Head tracking, when the presenter has poses to give. It supplies the eye
    // separation as well, so stereoIpd is not consulted at all in this path -- the
    // runtime knows the user's real IPD and we do not.
    VrEye      vr[2];
    const bool tracked =
        vrAcquireEyes(vr, g_state.sourceWidth / 2, g_state.sourceHeight);

    // The probe needs head poses to render from, so it waits for a tracked frame rather
    // than firing into the menu and reporting on a mono image.
    // Without this a frustum from the last tracked frame would keep being re-applied at
    // bind time after tracking drops, quietly overriding the game's own fov in a mode
    // that is supposed to be stock.
    if (!tracked) g_state.pendingValid[0] = g_state.pendingValid[1] = false;

    // A frame with no head poses renders from the car's own pose and the game's own
    // frustum, which lands in the headset as a one-frame jump to a different viewpoint.
    // That is almost certainly the "blink": it was always here, but it only became
    // visible once the tracked frames started using a genuinely different field of
    // view from the untracked ones.
    if (tracked != g_state.wasTracked) {
        g_state.wasTracked = tracked;
        if (!tracked && g_state.untrackedReports < 8) {
            ++g_state.untrackedReports;
            logf("stereo: head poses UNAVAILABLE this frame -- rendering from the car "
                 "pose and the game's fov. One of these is one blink.");
        }
    }

    const bool probe = g_state.fovProbeRequested && tracked;

    // -- level the seat ------------------------------------------------------------
    //
    // params->orientation maps camera-local to world, so its columns are the camera's
    // axes: column 0 is forward, column 2 is up, in GPL's frame (X forward, Y left,
    // Z up). The elevation of that forward vector is the camera's pitch.
    //
    // GPL's cockpit camera is pitched down -- recentring aims at the wheel. Composing
    // the head rotation onto a pitched frame makes a head yaw rotate about a tilted
    // axis, which injects roll that grows with yaw and equals the pitch offset at 90
    // degrees. Removing the pitch first makes the head yaw about true vertical.
    //
    // The correction is a rotation about the camera's own left axis (local Y), applied
    // on the right so it acts in camera space and leaves the car's heading alone.
    float seatPitch = 0.0f;
    if (tracked) {
        // MEASURE unconditionally. The first version only computed this inside the
        // `vrLevelSeat` branch, so running with the fix off -- the obvious thing to do
        // when comparing -- produced no number at all. An instrument that only works
        // when the fix is enabled cannot tell you whether the fix is needed.
        const float forwardZ = savedOrientation[2 * 3 + 0];  // column 0, world Z
        seatPitch = std::asin(forwardZ < -1.0f ? -1.0f
                                               : (forwardZ > 1.0f ? 1.0f : forwardZ));

        // Rotation about +Y by b takes local X to (cos b, 0, -sin b), i.e. positive b
        // pitches the forward axis DOWN. To cancel a downward pitch (negative
        // elevation) we therefore rotate by b = +seatPitch.
        const float cb = std::cos(g_options.vrLevelSeat ? seatPitch : 0.0f);
        const float sb = std::sin(g_options.vrLevelSeat ? seatPitch : 0.0f);
        const float aboutY[9] = {cb, 0.0f, sb,
                                 0.0f, 1.0f, 0.0f,
                                 -sb, 0.0f, cb};

        float levelled[9];
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                levelled[row * 3 + column] =
                    savedOrientation[row * 3 + 0] * aboutY[0 * 3 + column] +
                    savedOrientation[row * 3 + 1] * aboutY[1 * 3 + column] +
                    savedOrientation[row * 3 + 2] * aboutY[2 * 3 + column];

        // Say what it was and prove the correction landed, rather than asserting the
        // sign was right. If "after" is not ~0 the rotation went the wrong way.
        // Report on CHANGE, not the first few frames. The first version capped at four
        // reports and spent all four on the 373x280 lobby camera before the sim camera
        // existed, so it measured the menu and said nothing about driving.
        const float degrees = seatPitch * 57.29578f;
        if (std::fabs(degrees - g_state.lastSeatPitch) > 1.0f &&
            g_state.seatReports < 40) {
            ++g_state.seatReports;
            g_state.lastSeatPitch = degrees;
            constexpr float kDegrees = 57.29578f;
            logf("stereo: game camera pitch %.2f deg (negative is nose-down); after "
                 "levelling %.2f deg. vrLevelSeat=%d. This angle becomes ROLL at 90 "
                 "degrees of head yaw, because the head then yaws about a tilted axis.",
                 seatPitch * kDegrees, std::asin(levelled[2 * 3 + 0]) * kDegrees,
                 g_options.vrLevelSeat ? 1 : 0);
        }

        std::memcpy(savedOrientation, levelled, sizeof(levelled));
    }
    if (probe) g_state.fovProbeRequested = false;

    for (int pass = 0; pass < 2; ++pass) {
        // pass 0 draws the left half of the buffer. Which eye that half belongs to is
        // the one thing a display can get wrong in a way that is invisible on a flat
        // screen and instantly nauseating in a headset, hence the switch.
        const int index = g_options.stereoSwapEyes ? 1 - pass : pass;

        // During the probe both halves render from eye 0's viewpoint. Leaving the IPD
        // in would put 65 mm of parallax between the halves and make a bitwise
        // comparison meaningless -- every pixel would differ whether or not the fov
        // did.
        const int poseIndex = probe ? 0 : index;

        if (tracked) {
            // Compose rather than replace: p+24 already carries the car's attitude and
            // its g-force lean, and the head turns relative to that.
            float composed[9];
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    composed[row * 3 + column] =
                        savedOrientation[row * 3 + 0] * vr[poseIndex].rotation[0 * 3 + column] +
                        savedOrientation[row * 3 + 1] * vr[poseIndex].rotation[1 * 3 + column] +
                        savedOrientation[row * 3 + 2] * vr[poseIndex].rotation[2 * 3 + column];
            std::memcpy(params->orientation, composed, sizeof(composed));

            // The eye offset is in car-local axes, so it rotates with the car -- the
            // same reason a raw world offset was wrong in Phase 0.
            float dx = 0.0f, dy = 0.0f, dz = 0.0f;
            localToWorld(savedOrientation, vr[poseIndex].offset[0],
                         vr[poseIndex].offset[1], vr[poseIndex].offset[2], dx, dy, dz);

            params->camera = g_state.half[pass].camera;
            params->eyeX = savedX + dx;
            params->eyeY = savedY + dy;
            params->eyeZ = savedZ + dz;

            // Re-aim at the runtime's frustum. Writing fov and aspect and setting the
            // dirty byte is enough -- cameraBind rebuilds the projection from them on
            // its way into the render, so there is no need to rebuild the camera.
            // The probe doubles the right half's frustum in TANGENT space, because
            // that is what `fov` means to gpl_cameraRebuildProjection -- doubling the
            // angle instead would be a different and less extreme change, and past 90
            // degrees not even well defined.
            float useFov = vr[poseIndex].fov;
            const float useAspect = vr[poseIndex].aspect;
            if (probe && pass == 1)
                useFov = 2.0f * std::atan(std::tan(useFov * 0.5f) * 2.0f);
            if (probe) {
                g_state.fovProbeFov[pass] = useFov;
                g_state.fovProbeAspect[pass] = useAspect;
            }

            g_state.pendingFov[pass] = useFov;
            g_state.pendingAspect[pass] = useAspect;
            g_state.pendingValid[pass] = true;

            cameraSetFrustum(g_state.half[pass].camera, useFov, useAspect);

            // Read it straight back. vrFovScale demonstrably reaches vrAcquireEyes --
            // the frustum log line moves with it -- but the picture reportedly does not
            // change at all, which cannot be true if the camera really rendered a
            // different field of view. Either the write is not sticking, or something
            // overwrites it before the render. This says which, once, rather than
            // letting the contradiction stand.
            if (!g_state.verifiedFrustum) {
                g_state.verifiedFrustum = true;
                const float readFov = cameraFloat(g_state.half[pass].camera, kCameraFov);
                const float readAspect =
                    cameraFloat(g_state.half[pass].camera, kCameraAspect);
                logf("stereo: frustum write check -- wrote fov %.4f aspect %.4f, reads "
                     "back fov %.4f aspect %.4f, dirty=%u%s",
                     useFov, useAspect, readFov, readAspect,
                     static_cast<unsigned>(
                         static_cast<unsigned char*>(g_state.half[pass].camera)
                             [kCameraDirty]),
                     (readFov == useFov) ? "" : "  <-- THE WRITE DID NOT STICK");
            }
        } else {
            // Local +Y is LEFT, so the left eye sits at +halfIpd and the right at
            // -halfIpd.
            const float lateral = (index == 0) ? halfIpd : -halfIpd;

            float dx = 0.0f, dy = 0.0f, dz = 0.0f;
            localToWorld(savedOrientation, 0.0f, lateral, 0.0f, dx, dy, dz);

            params->camera = g_state.half[pass].camera;
            params->eyeX = savedX + dx;
            params->eyeY = savedY + dy;
            params->eyeZ = savedZ + dz;
        }

        // The mirror camera targets the full render surface at its own fixed rect, so it
        // does not follow the eye into its half of the buffer. Drawing it once keeps the
        // picture closest to stock; drawing it twice doubles its cost for no gain.
        // The mirror lands in one half only by default, which would show up as a
        // difference the fov had nothing to do with. Suppress it for the probe frame.
        if (probe) {
            params->mirrorCamera = nullptr;
        } else {
            switch (g_options.stereoMirror) {
                case 0: params->mirrorCamera = nullptr; break;
                case 1: params->mirrorCamera = (pass == 0) ? savedMirror : nullptr; break;
                default: params->mirrorCamera = savedMirror; break;
            }
        }

        // Does GPL's euler round trip preserve what we composed?
        //
        // Measured on the matrix actually handed over, every frame, worst case kept.
        // If this stays at zero the euler path is transparent and is eliminated as a
        // suspect. If it grows with head angle, it is the transform that has been
        // distorting the world -- and it would be invisible facing forward, which is
        // the signature we have been chasing.
        if (tracked) {
            float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
            gplMatrixToEuler(params->orientation, yaw, pitch, roll);
            float rebuilt[9];
            gplEulerToMatrix(yaw, pitch, roll, rebuilt);

            float worst = 0.0f;
            for (int i = 0; i < 9; ++i) {
                const float d = std::fabs(rebuilt[i] - params->orientation[i]);
                if (d > worst) worst = d;
            }
            if (worst > g_state.worstEuler + 0.002f && g_state.eulerReports < 12) {
                ++g_state.eulerReports;
                g_state.worstEuler = worst;
                logf("stereo: euler round trip worst element error %.5f (yaw %.1f pitch "
                     "%.1f roll %.1f deg). GPL renders from these three angles, not from "
                     "the matrix; a growing error here IS the distortion.",
                     worst, yaw * 57.29578f, pitch * 57.29578f, roll * 57.29578f);
            }
        }

        original(params);

        // Did the projection actually get REBUILT?
        //
        // gpl_cameraRebuildProjection clears the dirty byte at +0x3C on its way out,
        // and gpl_cameraBind is the only thing that calls it during a render. So
        // reading that byte back after the pass is a direct test of whether our fov and
        // aspect ever reached the projection matrix:
        //
        //   0 -- the rebuild ran and consumed them; the render used our frustum
        //   1 -- nothing ever bound this camera, so the projection is still the one
        //        cameraCreate built, whatever we wrote into the object
        //
        // The write check above only proved the float survives in memory. It never
        // proved the renderer consumed it, and that gap is exactly what vrFovScale
        // having no visible effect at 0.35 points at.
        if (tracked) {
            const unsigned char dirty = static_cast<unsigned char*>(
                g_state.half[pass].camera)[kCameraDirty];
            const float renderedFov = cameraFloat(g_state.half[pass].camera, kCameraFov);
            const float renderedAspect =
                cameraFloat(g_state.half[pass].camera, kCameraAspect);

            if (!g_state.verifiedRebuild[pass]) {
                g_state.verifiedRebuild[pass] = true;
                logf("stereo: pass %d after render -- dirty=%u, camera fov %.4f aspect "
                     "%.4f%s",
                     pass, static_cast<unsigned>(dirty), renderedFov, renderedAspect,
                     dirty ? "  <-- NOT REBUILT: nothing bound this camera, so the fov "
                             "we wrote never reached the projection"
                           : "  (rebuilt, so the projection used our fov)");
            }

            // Every frame, not once: the whole point is to catch a path that overrides
            // the fov only sometimes. Capped so a persistent fault cannot flood the log.
            if (g_state.pendingValid[pass] &&
                renderedFov != g_state.pendingFov[pass] &&
                g_state.fovOverrideReports < 8) {
                ++g_state.fovOverrideReports;
                logf("stereo: FOV OVERRIDDEN on pass %d -- we set %.4f, the camera "
                     "rendered %.4f (aspect %.4f vs %.4f). Something outside the two "
                     "hooked cameraBind sites is writing this camera.",
                     pass, g_state.pendingFov[pass], renderedFov,
                     g_state.pendingAspect[pass], renderedAspect);
            }
        }
    }

    if (probe) g_state.fovProbeArmed = true;

    params->camera = savedCamera;
    params->mirrorCamera = savedMirror;
    params->eyeX = savedX;
    params->eyeY = savedY;
    params->eyeZ = savedZ;
    std::memcpy(params->orientation, savedOrientation, sizeof(savedOrientation));

    // Flush the second eye.
    //
    // roglv2 batches polygons and only hands a batch over when the destination changes.
    // Pass 0's batch is flushed when pass 1 sets its own destination -- but pass 1's is
    // not flushed until the frame is PRESENTED, and we take our copy of the back buffer
    // just before that. So the right half of the shared texture was a frame behind the
    // left, every frame.
    //
    // Measured, not deduced: with both eyes showing the left half the picture was smooth,
    // with both showing the right half both lagged, and swapping the halves moved the lag
    // to the other eye. It follows the half, not the eye.
    //
    // Binding the main camera is one more destination change, using the engine's own
    // function -- gpl_cameraBind calls polySetDest(camera->surface, camera->fov) -- and
    // it also leaves the engine's camera globals as it found them.
    //
    // This is the bug BFVR could not have: it renders each eye into its own texture and
    // never depends on when two halves of one buffer become visible.
    if (g_entries.cameraBind) g_entries.cameraBind(savedCamera);

    if (!g_state.reported) {
        g_state.reported = true;
        logf("stereo: first stereo frame rendered, IPD %.4f world units (half %.4f)",
             g_options.stereoIpd, halfIpd);
    }
    return true;
}

}  // namespace gplvr
