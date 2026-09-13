#include "Stereo.h"

#include "Hooks.h"
#include "Log.h"
#include "RenderTarget.h"
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

// The driver's eyes to the mirror glass, metres, at the seat. It is not derivable: GPL's
// mirror is an artistic 40 degrees rather than a measured reflection, so nothing in the
// game states where the glass is. What it sets is how sharply the mirror's field answers
// a lean -- smaller means a given lean changes more. The stock view is reproduced exactly
// at the seat whatever this holds, so a wrong value costs realism in motion and nothing
// at rest. A 1967 car puts the mirrors on the cowling within arm's reach.
//
// HOW FAR FROM YOUR EYES A REAL PANE WOULD HAVE TO BE FOR 40 DEGREES TO BE GEOMETRIC:
// w / tan(20 deg), which for mirrors of 60 to 180 mm is 0.08 m to 0.25 m. On your nose.
// A 1967 car puts them on the cowling at 0.55 to 0.85 m, where 80 to 120 mm of glass
// subtends 5 to 12 degrees -- so GPL's mirror is four to six times wider than life, a
// deliberate choice that makes it usable at 640x480 at the cost of everything in it
// appearing that much smaller.
//
// This constant is therefore the REAL distance, not the one 40 degrees implies. Using the
// 0.15 m the stock angle implies would make a 10 cm lean a two-thirds change and the
// mirror would lurch.
//
// IT DRIVES TWO THINGS AT ONCE, which is why it is back at 0.70 after a spell at 0.50.
// Lowering it opens the field faster as you lean in (wanted) and swings the view faster as
// you move sideways (not wanted, and the swing was the half that started reading as
// unnatural). Both scale as 1/distance, so this alone can only trade one against the
// other. kMirrorAnchorFovDegrees below is what separates them.
//
// The half-width the two together imply is fiction, and it cancels: it appears once to
// reproduce the anchor angle at the seat, leaving
// fov = 2*atan(tan(anchor/2) * kMirrorEyeToGlass / L), a pure 1/L scaling identical for
// glass of any size. Do not "correct" it to a realistic pane.
constexpr float kMirrorEyeToGlass = 0.70f;

// The angle the glass covers at the seat, replacing GPL's 40 degrees. Zero keeps the
// game's own value and changes nothing.
//
// THIS EXISTS TO DECOUPLE TWO THINGS kMirrorEyeToGlass CONTROLS AT ONCE. That distance
// sets both how much the field opens as you lean in (wanted: more) and how far the view
// swings as you move sideways (wanted: less) -- and it moves them the same way, so tuning
// it can only trade one against the other. The anchor angle changes the field without
// touching the swing.
//
// It is also the honest fix for the swing feeling wrong. The swing itself is correct --
// atan(sideways / distance), the law of reflection -- but GPL's mirror is four to six
// times wider than life, so 15 degrees of swing slides a 47-degree cone across 38% of its
// own width and the result is a sweeping pan nobody's mirror does. At a lifelike 12
// degrees the same swing is more than a field width: the view simply LEAVES, which is
// what happens when you move out of a real mirror's useful position. Narrowing this also
// makes everything in the glass proportionally bigger, which is what makes the lean-in
// response visible rather than merely measurable.
//
// 40 = stock. 20 is halfway to life and keeps the mirror usable. Below about 12 it is
// geometrically honest and probably useless to drive with.
constexpr float kMirrorAnchorFovDegrees = 15.0f;

// How far out to the side the mirrors sit, metres from the centreline. 0 puts a single
// pane straight ahead of the driver.
//
// THE CAR HAS TWO MIRRORS AND ONE mirror.mip. The reflection can only ever be computed
// for one pane, and modelling that pane on the centreline is what made moving the head in
// front of the LEFT mirror show the scene far to the RIGHT: relative to a centre pane,
// that head position is a 0.3 m sideways move, and the law of reflection duly swings the
// axis about 23 degrees the other way. In front of a real left-hand pane the same head
// sees straight astern.
//
// What this does instead is let the pane FOLLOW the head sideways, clamped to the spread
// the real mirrors occupy: anywhere between the two, the driver is taken to be using
// whichever one they have moved in front of, and the view is square astern. Past the
// outer pane the clamp bites and the swing returns, which is what running out of mirror
// feels like.
//
// Forward/back and up/down are untouched: the lean-in field change and the pitch response
// both come through unchanged. Only the sideways axis is absorbed, and it is the only one
// that had no correct answer with a single shared image.
constexpr float kMirrorPaneSpread = 0.0f;

// Leaning past the glass would send the field through 180 degrees and the projection
// through infinity. Neither limit is ever reached by a seated driver; they are here so
// that a tracking glitch cannot produce a degenerate frustum.
constexpr float kMirrorMinDistance = 0.15f;
constexpr float kMirrorMaxFov = 2.44f;  // 140 degrees

struct Entries {
    // The rasterizer entries are held as their GLOBALS, not as resolved pointers. The
    // table is filled in by GetProcAddress after we install and could be re-resolved on
    // a mode change, so every call reads the slot's current value.
    void** bitmapWidthSlot = nullptr;
    void** bitmapHeightSlot = nullptr;
    void** instanceAllocSlot = nullptr;
    void** instanceFreeSlot = nullptr;
    void** backBufferSlot = nullptr;

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
    // Where each eye actually landed: x, y, w, h in source pixels. Not derivable from
    // sourceWidth/2 -- an odd-width buffer leaves a column belonging to neither eye.
    int   eyeRect[2][4] = {};
    bool  reported = false;
    bool  gaveUp = false;
    bool  verifiedFrustum = false;
    bool  verifiedRebuild[2] = {false, false};
    // The fov the camera actually held after the last render, per pass, so a SWEEP can
    // be traced rather than a single startup frame.
    float lastRenderedFov[2] = {0.0f, 0.0f};
    int   rebuildReports = 0;

    // One eye's full horizontal angle and the distance between the two eye positions,
    // both as the runtime last reported them. Kept for anything that has to place
    // something at a chosen depth in the finished image -- the HUD's disparity is the
    // only such thing today -- so that it uses the user's real IPD rather than a constant.
    float eyeFov = 0.0f;
    float eyeSeparation = 0.0f;
    bool  geometryValid = false;

    // The frustum each eye camera should be rendering with, kept so the cameraBind hook
    // can put it back after sceneRender overwrites it.
    float pendingFov[2] = {0.0f, 0.0f};
    float pendingAspect[2] = {0.0f, 0.0f};
    bool  pendingValid[2] = {false, false};
    bool  loggedReapply = false;
    bool  loggedFlatFrustum = false;
    float loggedMainCx = 999.0f;
    float loggedMainCy = 999.0f;
    int   loggedSurfaceW = -1;
    int   loggedSurfaceH = -1;
    int   loggedBackW = -1;
    int   loggedBackH = -1;

    // Watchdog counters. The fov was silently overridden once already; "it is fixed"
    // is worth exactly as much as the instrument that keeps saying so.
    int   fovOverrideReports = 0;
    int   untrackedReports = 0;
    bool  wasTracked = false;
    int   seatReports = 0;
    int   mirrorReports = 12;
    bool  mirrorReported = false;
    float lastMirrorHead[3] = {0.0f, 0.0f, 0.0f};
    int   eulerReports = 0;
    float worstEuler = 0.0f;
    float lastSeatPitch = 999.0f;

    // gpl_cameraCreate does not allocate; the engine passes it a static buffer. So do
    // we. 512 is four times what the object needs.
    alignas(16) unsigned char storage[2][512] = {};
};

State g_state;

// The null check is not paranoia: roglv2 resolves a handle as
// `*(handleTable + 2) + 8 * handle` and dereferences it unchecked, so handle 0 is a null
// vtable call and takes the process down. Every call site here passes a surface that has
// already been tested, but that is a property of today's callers rather than of these
// functions.
int bitmapWidth(void* bitmap) {
    if (!bitmap) return 0;
    auto fn = reinterpret_cast<BitmapWidthFn>(*g_entries.bitmapWidthSlot);
    return fn ? fn(bitmap) : 0;
}

int bitmapHeight(void* bitmap) {
    if (!bitmap) return 0;
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
// Put the principal point back in the middle.
//
// cameraCreate stores cx and cy PRE-MULTIPLIED by the half extents, so centring them is
// simply writing zero to both -- no need to know the extents. The dirty byte is raised
// the same way cameraSetFrustum does it, so cameraBind rebuilds the projection from the
// centred values on its way into the render.
void cameraCentrePrincipalPoint(void* camera) {
    auto bytes = static_cast<unsigned char*>(camera);
    *reinterpret_cast<float*>(bytes + kCameraCx) = 0.0f;
    *reinterpret_cast<float*>(bytes + kCameraCy) = 0.0f;
    bytes[kCameraDirty] = 1;
}

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

    // Before measuring it. When the frame is going to an off-screen target, the surface
    // GPL built for its own display mode has to be told the new size, or the eyes are
    // carved out of the old one and nothing about the resolution actually changes.
    renderTargetOverrideSurface(source);

    const int width = bitmapWidth(source);
    const int height = bitmapHeight(source);
    if (width < 2 || height < 1) return false;

    // A world render fills the back buffer. A menu's 3D viewport does not.
    //
    // The racing lobby draws a TV feed into a 373x280 surface inside a 640x480 window,
    // and we were splitting that into two 186x280 eyes -- so the headset went stereo
    // while the driver was still in the menu, and vrMenuScale stopped applying because
    // the presenter was told the frame was a side-by-side world. Quitting to the menu
    // from a race does the same thing.
    //
    // The surface's size against the back buffer's is what separates them, and it costs
    // one call.
    if (g_entries.backBufferSlot && *g_entries.backBufferSlot) {
        auto getBackBuffer =
            reinterpret_cast<void*(__cdecl*)()>(*g_entries.backBufferSlot);
        if (void* back = getBackBuffer()) {
            const int fullWidth = bitmapWidth(back);
            const int fullHeight = bitmapHeight(back);
            // Allow a small margin: odd render widths mean the world surface is not
            // always exactly the back buffer.
            // Report on CHANGE, both ways. The first version logged once and only
            // when it decided "menu", so a case where it wrongly decided "world" said
            // nothing at all -- which is exactly the case still misbehaving after a
            // race. An instrument that only speaks when it agrees with itself is no
            // instrument.
            const bool viewport = fullWidth > 0 && fullHeight > 0 &&
                                  (width * 8 < fullWidth * 7 ||
                                   height * 8 < fullHeight * 7);
            if (width != g_state.loggedSurfaceW || height != g_state.loggedSurfaceH ||
                fullWidth != g_state.loggedBackW ||
                fullHeight != g_state.loggedBackH) {
                g_state.loggedSurfaceW = width;
                g_state.loggedSurfaceH = height;
                g_state.loggedBackW = fullWidth;
                g_state.loggedBackH = fullHeight;
                logf("stereo: %dx%d render into a %dx%d back buffer -> treated as %s",
                     width, height, fullWidth, fullHeight,
                     viewport ? "a MENU viewport (mono, vrMenuScale applies)"
                              : "a WORLD view (split into two eyes)");
            }
            if (viewport) return false;
        }
    }

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

    // Where each eye lands, published to the presenter so it samples exactly what
    // was rendered rather than assuming a clean half -- an odd-width buffer leaves a
    // column belonging to neither eye.
    g_state.eyeRect[0][0] = 0;
    g_state.eyeRect[1][0] = halfWidth;
    for (int eye = 0; eye < 2; ++eye) {
        g_state.eyeRect[eye][1] = 0;
        g_state.eyeRect[eye][2] = halfWidth;
        g_state.eyeRect[eye][3] = height;
    }

    if (!buildHalf(g_state.half[0], g_state.storage[0], source, 0, 0, halfWidth,
                   height, fov, nearZ) ||
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
    g_entries.backBufferSlot = binding.raster.screenGetBackBuffer;
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

        // And the principal point, on the same grounds and for the same reason.
        //
        // sceneRender sets the fov and the principal point on consecutive lines before
        // cameraBind. The fov half was put back from Phase 2 onward and the principal
        // point was left as the game set it -- which, for the cockpit view, is offset
        // vertically by about an eighth of half-height, and pitched the whole VR image
        // down by nine degrees for nine sessions. A headset frustum is symmetric about
        // the eye; anything but centre is simply wrong here, so there is nothing to
        // choose between.
        cameraCentrePrincipalPoint(camera);

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

int stereoEyeWidth() {
    return g_state.half[0].camera ? g_state.eyeRect[0][2] : 0;
}

bool stereoEyeRect(int eye, int out[4]) {
    if (eye < 0 || eye > 1 || !g_state.half[eye].camera) return false;
    for (int i = 0; i < 4; ++i) out[i] = g_state.eyeRect[eye][i];
    return out[2] > 0 && out[3] > 0;
}

void* stereoEyeSurface(int eye) {
    if (eye < 0 || eye > 1) return nullptr;
    return g_state.half[eye].camera ? g_state.half[eye].surface : nullptr;
}

void* stereoSourceSurface() { return g_state.sourceSurface; }

bool stereoEyeGeometry(float& fov, float& separation) {
    if (!g_state.geometryValid) return false;
    fov = g_state.eyeFov;
    separation = g_state.eyeSeparation;
    return fov > 0.0f && separation > 0.0f;
}

void stereoRelease() {
    releaseHalf(g_state.half[0]);
    releaseHalf(g_state.half[1]);
    for (int eye = 0; eye < 2; ++eye)
        for (int i = 0; i < 4; ++i) g_state.eyeRect[eye][i] = 0;
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
    // The params block is the GAME'S live memory, not a copy of it -- gpl_simFrameRender
    // reuses it and zeroes p[43] the moment sceneRender returns. So anything we adjust in
    // it has to be put back before we hand control away. The mirror aim is the frame
    // hook's, applied around both this and the game's own single render.
    const float savedX = params->eyeX;
    const float savedY = params->eyeY;
    const float savedZ = params->eyeZ;
    float       savedOrientation[9];
    std::memcpy(savedOrientation, params->orientation, sizeof(savedOrientation));


    // Head poses, or nothing. The runtime supplies the eye separation as well as the
    // orientation, so there is no eye-separation constant anywhere in here: the runtime
    // knows the user's real IPD and we do not.
    VrEye      vr[2];
    const bool tracked =
        vrAcquireEyes(vr, g_state.sourceWidth / 2, g_state.sourceHeight);

    // Without this a frustum from the last tracked frame would keep being re-applied at
    // bind time after tracking drops, quietly overriding the game's own fov in a mode
    // that is supposed to be stock.
    if (!tracked) g_state.pendingValid[0] = g_state.pendingValid[1] = false;

    g_state.geometryValid = tracked;
    if (tracked) {
        float dx = vr[0].offset[0] - vr[1].offset[0];
        float dy = vr[0].offset[1] - vr[1].offset[1];
        float dz = vr[0].offset[2] - vr[1].offset[2];
        g_state.eyeFov = vr[0].fov;
        g_state.eyeSeparation = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // No poses, no split. Returning false sends the caller to the game's own single
    // render, and the presenter shows that one image to both eyes.
    //
    // The alternative -- splitting anyway and separating the halves by a fixed IPD from
    // the car's own pose -- is what this used to do, and it landed in the headset as a
    // one-frame jump to a different viewpoint whenever tracking dropped. That was the
    // "blink". A mono frame shown to both eyes is a still picture for a frame; a stereo
    // pair from the wrong viewpoint is a lurch.
    if (tracked != g_state.wasTracked) {
        g_state.wasTracked = tracked;
        if (!tracked && g_state.untrackedReports < 8) {
            ++g_state.untrackedReports;
            logf("stereo: head poses UNAVAILABLE this frame -- falling back to the "
                 "game's own single render, shown to both eyes.");
        }
    }
    if (!tracked) return false;

    // -- THE MIRROR, FROM WHERE THE DRIVER'S HEAD ACTUALLY IS -------------------------
    //
    // GPL puts the mirror eye at a fixed car-relative point, so leaning does nothing to
    // what the mirror shows -- the one thing a real driver does with a mirror. For a
    // planar mirror the camera belongs at the viewer's eye REFLECTED through the glass,
    // which normally needs the plane's position AND its normal. Not here: GPL's own
    // mirrorEye already stands for the plane's position, so taking it as the base leaves
    // only the normal, and the mirror's own basis carries that -- column 0 is where the
    // mirror looks.
    //
    // MONO, deliberately, and it is not a compromise being hidden. The mirror renders by
    // the COPY-OUT path: gpl_simFrameRender renders into a corner of the frame and copies
    // the result into mirror.mip, which the car model's mirror quads sample. There is one
    // mirror.mip, so both eyes necessarily see the same mirror image, and a per-eye view
    // would need a second texture and a model that knew to sample it. The head centre is
    // the right single viewpoint for a picture two eyes share.
    //
    // Head ROTATION is answered for free: the eyes orbit the neck, so turning your head
    // moves their midpoint, and a real mirror responds to exactly that.
    float savedMirrorEye[3];
    float savedMirrorBasis[9];
    float savedMirrorFov = params->mirrorFov;
    bool  mirrorMoved = false;
    std::memcpy(savedMirrorEye, params->mirrorEye, sizeof(savedMirrorEye));
    std::memcpy(savedMirrorBasis, params->mirrorOrientation, sizeof(savedMirrorBasis));
    if (params->mirrorCamera) {
        // The head centre, relative to the seat, in GPL local axes: vr[i].offset is each
        // eye's own displacement from the recentre pose, so their midpoint is the head's.
        float lx = (vr[0].offset[0] + vr[1].offset[0]) * 0.5f;
        float ly = (vr[0].offset[1] + vr[1].offset[1]) * 0.5f;
        float lz = (vr[0].offset[2] + vr[1].offset[2]) * 0.5f;

        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        localToWorld(savedOrientation, lx, ly, lz, dx, dy, dz);

        const float* m = params->mirrorOrientation;
        float nx = m[0], ny = m[3], nz = m[6];
        float dot = dx * nx + dy * ny + dz * nz;

        params->mirrorEye[0] += dx - 2.0f * dot * nx;
        params->mirrorEye[1] += dy - 2.0f * dot * ny;
        params->mirrorEye[2] += dz - 2.0f * dot * nz;
        mirrorMoved = true;

        // -- WHERE IT LOOKS: THE VIEW DIRECTION, REFLECTED --------------------------------
        //
        // Moving the camera and widening its cone still leaves it pointing dead astern, so
        // at a grazing angle the mirror shows a slice of the world that has nothing to do
        // with what the glass would reflect from where you are sitting. The cone has to
        // TILT, and the tilt is the law of reflection: the direction you are looking at
        // the glass, mirrored about the glass normal.
        //
        //     u = normalize(P - E)        where you are looking
        //     r = u - 2*(u.n)*n           what comes back at you
        //
        // At the seat u is exactly -n and r comes out as +n, so the stock rear-facing view
        // is reproduced to the last digit. Lean left and r swings the other way, which is
        // the familiar thing a real mirror does: move your head left and more of the RIGHT
        // side behind you comes into it.
        //
        // GPL's mirror is a rear-facing CAMERA whose picture is pasted on the glass, not a
        // reflection frustum, so the camera takes the reflected DIRECTION. Aiming it at the
        // glass instead -- which is what this did first -- turns it to face forward, since
        // the glass is ahead of the driver, and the mirror then shows the road in front.
        //
        // The glass centre is fixed to the CAR, not to the head: savedX/Y/Z is the seat
        // eye, before the per-eye offset is added further down, so P does not chase the
        // head and u can actually change.
        // Column 1 of the basis is the mirror's own sideways axis. Which way it points
        // does not matter: the clamp is symmetric and the offset is applied along the
        // same axis it was measured on, so the pane always moves to the head's side.
        float lax = m[1], lay = m[4], laz = m[7];
        float lateral = dx * lax + dy * lay + dz * laz;
        float paneLateral = lateral;
        if (paneLateral > kMirrorPaneSpread) paneLateral = kMirrorPaneSpread;
        if (paneLateral < -kMirrorPaneSpread) paneLateral = -kMirrorPaneSpread;

        float px = savedX - nx * kMirrorEyeToGlass + lax * paneLateral;
        float py = savedY - ny * kMirrorEyeToGlass + lay * paneLateral;
        float pz = savedZ - nz * kMirrorEyeToGlass + laz * paneLateral;

        // Head to glass. E = seat eye + d.
        float vx = px - (savedX + dx);
        float vy = py - (savedY + dy);
        float vz = pz - (savedZ + dz);
        float span = std::sqrt(vx * vx + vy * vy + vz * vz);

        // -- AND THE FIELD OF VIEW, WHICH IS THE HALF THAT MAKES IT READ AS GLASS ------
        //
        // A mirror's visible cone is bounded by the edges of the glass as seen from the
        // eye, so it WIDENS as you lean in and narrows as you sit back. Position alone
        // moves the picture around; this is what makes the picture belong to a pane of
        // glass at a fixed place in the car.
        //
        // The glass's half-width never has to be measured, because GPL's own stock fov
        // states it: 40 degrees IS the angle that glass subtends from the nominal head
        // position, so half-width = kMirrorEyeToGlass * tan(fov_stock / 2), and the new
        // angle at distance L is 2 * atan(half-width / L). Reflection preserves distance,
        // so L is the head's distance from the glass whichever side you measure it on.
        //
        // The distance is now the camera's own span to the glass centre rather than the
        // normal component alone, which is the same number when you are square to the
        // mirror and the longer, correct one when you are not: sliding sideways moves you
        // further from the pane and the view narrows, as it does in life.
        //
        // kMirrorEyeToGlass is the one number that cannot be derived. GPL's mirror is an
        // artistic 40 degrees rather than a measured reflection, so this sets how fast
        // the angle answers a lean rather than fixing an objectively right value; at
        // d = 0 the stock fov is reproduced exactly whatever it holds.
        float distance = span;
        if (distance < kMirrorMinDistance) distance = kMirrorMinDistance;

        float anchorFov = kMirrorAnchorFovDegrees > 0.0f
                              ? kMirrorAnchorFovDegrees * 0.0174532925f
                              : savedMirrorFov;

        float newFov = anchorFov;
        if (anchorFov > 0.01f && anchorFov < 3.0f) {
            float halfWidth = kMirrorEyeToGlass * std::tan(anchorFov * 0.5f);
            newFov = 2.0f * std::atan(halfWidth / distance);
            if (newFov > kMirrorMaxFov) newFov = kMirrorMaxFov;
            params->mirrorFov = newFov;
        }

        // Column 0 of the basis is the look direction, and gpl_viewRecordSetEuler builds
        // this basis FROM eulers, so decomposing and recomposing is the round trip the
        // engine already performs -- the per-frame check below measures its error at
        // zero. Yaw and pitch come from the new direction; roll is whatever the car's
        // attitude put there, and is not ours to change.
        float aimYaw = 0.0f, aimPitch = 0.0f, aimRoll = 0.0f;
        if (span > 1e-4f) {
            gplMatrixToEuler(params->mirrorOrientation, aimYaw, aimPitch, aimRoll);

            float ux = vx / span, uy = vy / span, uz = vz / span;
            float udotn = ux * nx + uy * ny + uz * nz;

            float rx = ux - 2.0f * udotn * nx;
            float ry = uy - 2.0f * udotn * ny;
            float rz = uz - 2.0f * udotn * nz;
            if (rz > 1.0f) rz = 1.0f;
            if (rz < -1.0f) rz = -1.0f;

            aimYaw = std::atan2(ry, rx);
            aimPitch = -std::asin(rz);
            gplEulerToMatrix(aimYaw, aimPitch, aimRoll, params->mirrorOrientation);
        }

        // REPORT WHEN THE HEAD HAS ACTUALLY MOVED, not for the first four frames.
        //
        // The first version spent its whole budget in the sixteen milliseconds after the
        // race loaded, every sample at the seat, so it proved the stock case and said
        // nothing whatever about the thing being tested. A sample is worth printing when
        // it differs from the last one printed.
        float moved = std::fabs(lx - g_state.lastMirrorHead[0]) +
                      std::fabs(ly - g_state.lastMirrorHead[1]) +
                      std::fabs(lz - g_state.lastMirrorHead[2]);
        if (g_state.mirrorReports > 0 && (!g_state.mirrorReported || moved > 0.03f)) {
            --g_state.mirrorReports;
            g_state.mirrorReported = true;
            g_state.lastMirrorHead[0] = lx;
            g_state.lastMirrorHead[1] = ly;
            g_state.lastMirrorHead[2] = lz;
            logf("mirror: driven from the head -- head %+.3f %+.3f %+.3f m from the seat "
                 "in GPL axes, mirror eye moved by %+.3f %+.3f %+.3f after reflecting "
                 "about the glass normal %+.2f %+.2f %+.2f. Glass centre is %.2f m away "
                 "(nominal %.2f), so the fov goes %.1f -> %.1f deg and the look direction "
                 "is the view reflected off the glass -- yaw %+.1f pitch %+.1f deg "
                 "(180/0 is straight astern). Mono: one mirror.mip serves both eyes.",
                 lx, ly, lz, dx - 2.0f * dot * nx, dy - 2.0f * dot * ny,
                 dz - 2.0f * dot * nz, nx, ny, nz, distance, kMirrorEyeToGlass,
                 anchorFov * 57.2957795f, newFov * 57.2957795f,
                 aimYaw * 57.2957795f, aimPitch * 57.2957795f);
        }
    }

    // What the game is asking for, reported on change. This is the number the overlay
    // alignment was measuring the whole time, and it costs one line to read directly
    // instead of inferring it from a horizon a human had to line up by hand.
    if (std::fabs(params->mainCy - g_state.loggedMainCy) > 0.001f ||
        std::fabs(params->mainCx - g_state.loggedMainCx) > 0.001f) {
        g_state.loggedMainCx = params->mainCx;
        g_state.loggedMainCy = params->mainCy;
        logf("stereo: the game's PRINCIPAL POINT is cx %.4f cy %.4f (normalised, screen "
             "edges at +/-1), applied to whatever camera is in p[2] -- ours -- on the "
             "line before cameraBind. cy shifts the image by that fraction of half-"
             "height with NO dependence on the field of view; the bind hook puts it back "
             "to centre.",
             params->mainCx, params->mainCy);
    }

    // -- horizon lock ---------------------------------------------------------------
    //
    // params->orientation maps camera-local to world, so its columns are the camera's
    // axes: column 0 is forward, column 1 is left, column 2 is up, in GPL's frame
    // (X forward, Y left, Z up). So the world-Z component of each column is that axis's
    // elevation, and both angles fall straight out:
    //
    //   pitch = asin(forward.z)                 how far the nose is up or down
    //   roll  = atan2(left.z, up.z)             how far the car leans
    //
    // ROLL IS THE HALF THAT MATTERS MOST, and that is not a preference. A viewer can
    // counteract a pitched or yawed camera by moving their own head; nobody can
    // counteract roll, and it is the axis that conflicts most directly with the inner
    // ear's sense of down. Meta's own VR guidance is blunt about never rotating the
    // camera in roll, and every racing sim that ships a horizon lock -- ACC, AMS2,
    // rFactor 2, the Dirt/WRC games -- aims it at roll from banking, kerbs and body
    // lean first, with pitch along for the ride.
    //
    // Both corrections are applied on the RIGHT, so they act in camera space and leave
    // the car's heading alone. Roll goes first, and the order is not arbitrary: a
    // rotation about the forward axis leaves the forward direction untouched, so the
    // pitch measured above is unaffected by it -- and once the roll is out, the left
    // axis is horizontal, which is exactly the axis a pitch correction wants to turn
    // about.
    // MEASURED unconditionally, whatever the horizon lock is set to. An earlier
    // version computed these only when the levelling was switched on, so running with
    // it off -- the obvious thing to do when comparing -- produced no number at all. An
    // instrument that only works when the option is enabled cannot tell you whether the
    // option is needed.
    const float forwardZ = savedOrientation[2 * 3 + 0];  // column 0, world Z
    const float leftZ = savedOrientation[2 * 3 + 1];     // column 1, world Z
    const float upZ = savedOrientation[2 * 3 + 2];       // column 2, world Z
    const float seatPitch = std::asin(
        forwardZ < -1.0f ? -1.0f : (forwardZ > 1.0f ? 1.0f : forwardZ));
    const float seatRoll = std::atan2(leftZ, upZ);

    // Blended, not switched. See Options::vrHorizonLock.
    const float lock = (g_options.vrHorizonLock < 0.0f)
                           ? 0.0f
                           : (g_options.vrHorizonLock > 1.0f ? 1.0f
                                                             : g_options.vrHorizonLock);

    // Rotation about +X by a maps local Y to (0, cos a, sin a) and local Z to
    // (0, -sin a, cos a). Taking a = -seatRoll drives the left axis's world-Z
    // component to cos(roll)*left.z - sin(roll)*up.z, which is zero by construction.
    const float ra = -seatRoll * lock;
    const float cr = std::cos(ra);
    const float sr = std::sin(ra);
    const float aboutX[9] = {1.0f, 0.0f, 0.0f,
                             0.0f, cr,   -sr,
                             0.0f, sr,   cr};

    // Rotation about +Y by b takes local X to (cos b, 0, -sin b), i.e. positive b
    // pitches the forward axis DOWN. To cancel a downward pitch (negative
    // elevation) we therefore rotate by b = +seatPitch.
    const float cb = std::cos(seatPitch * lock);
    const float sb = std::sin(seatPitch * lock);
    const float aboutY[9] = {cb, 0.0f, sb,
                             0.0f, 1.0f, 0.0f,
                             -sb, 0.0f, cb};

    float unrolled[9];
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            unrolled[row * 3 + column] =
                savedOrientation[row * 3 + 0] * aboutX[0 * 3 + column] +
                savedOrientation[row * 3 + 1] * aboutX[1 * 3 + column] +
                savedOrientation[row * 3 + 2] * aboutX[2 * 3 + column];

    float levelled[9];
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            levelled[row * 3 + column] =
                unrolled[row * 3 + 0] * aboutY[0 * 3 + column] +
                unrolled[row * 3 + 1] * aboutY[1 * 3 + column] +
                unrolled[row * 3 + 2] * aboutY[2 * 3 + column];

    // Say what it was and prove the correction landed, rather than asserting the
    // signs were right. If either "after" figure is not ~0 at a lock of 1.0, that
    // rotation went the wrong way.
    // Report on CHANGE, not the first few frames. The first version capped at four
    // reports and spent all four on the 373x280 lobby camera before the sim camera
    // existed, so it measured the menu and said nothing about driving.
    const float degrees = seatPitch * 57.29578f;
    if (std::fabs(degrees - g_state.lastSeatPitch) > 1.0f &&
        g_state.seatReports < 40) {
        ++g_state.seatReports;
        g_state.lastSeatPitch = degrees;
        constexpr float kDegrees = 57.29578f;
        logf("stereo: car pitch %.2f roll %.2f deg (pitch negative is nose-down); "
             "after a horizon lock of %.2f, %.2f and %.2f deg reach the view. This "
             "is the CAR'S attitude, not a fixed view offset -- it drifts through "
             "zero.",
             degrees, seatRoll * kDegrees, lock,
             std::asin(levelled[2 * 3 + 0]) * kDegrees,
             std::atan2(levelled[2 * 3 + 1], levelled[2 * 3 + 2]) * kDegrees);
    }

    std::memcpy(savedOrientation, levelled, sizeof(levelled));

    for (int pass = 0; pass < 2; ++pass) {
        // pass 0 draws the left half of the buffer and is the left eye. Proven in
        // game, not assumed: the left viewpoint lands in the left half with no swap
        // needed, and no headset has wanted otherwise.
        const int poseIndex = pass;

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
        const float useFov = vr[poseIndex].fov;
        const float useAspect = vr[poseIndex].aspect;

        g_state.pendingFov[pass] = useFov;
        g_state.pendingAspect[pass] = useAspect;
        g_state.pendingValid[pass] = true;

        cameraSetFrustum(g_state.half[pass].camera, useFov, useAspect);

        // Read it straight back. A frustum that reaches vrAcquireEyes and does not
        // reach the picture means either the write is not sticking or something
        // overwrites it before the render -- and something does, twice: sceneRender
        // sets the fov and then the principal point on its way to cameraBind. This
        // says which, once, rather than
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

        // The mirror camera targets the full render surface at its own fixed rect, so it
        // does not follow the eye into its half of the buffer. Drawing it once keeps the
        // picture closest to stock; drawing it twice doubles its cost for no gain.
        switch (g_options.stereoMirror) {
            case 0: params->mirrorCamera = nullptr; break;
            case 1: params->mirrorCamera = (pass == 0) ? savedMirror : nullptr; break;
            default: params->mirrorCamera = savedMirror; break;
        }

        // Does GPL's euler round trip preserve what we composed?
        //
        // Measured on the matrix actually handed over, every frame, worst case kept.
        // If this stays at zero the euler path is transparent and is eliminated as a
        // suspect. If it grows with head angle, it is the transform that has been
        // distorting the world -- and it would be invisible facing forward, which is
        // the signature we have been chasing.
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
        // proved the renderer consumed it, and that gap is where a frustum written at
        // the wrong moment disappears without trace.
        const unsigned char dirty = static_cast<unsigned char*>(
            g_state.half[pass].camera)[kCameraDirty];
        const float renderedFov = cameraFloat(g_state.half[pass].camera, kCameraFov);
        const float renderedAspect =
            cameraFloat(g_state.half[pass].camera, kCameraAspect);

        // Report the first frame AND every change of half a degree after it.
        //
        // One-shot is how six separate instruments in this project spent their
        // single report on a startup frame and said nothing about the thing under
        // investigation. This one has to be able to trace a change over time.
        const float renderedDegrees = renderedFov * 57.29578f;
        const bool  changed =
            std::fabs(renderedDegrees - g_state.lastRenderedFov[pass] * 57.29578f) >
            0.5f;
        if ((!g_state.verifiedRebuild[pass] || changed) &&
            g_state.rebuildReports < 60) {
            if (changed) ++g_state.rebuildReports;
            g_state.verifiedRebuild[pass] = true;
            g_state.lastRenderedFov[pass] = renderedFov;
            logf("stereo: pass %d after render -- dirty=%u, camera fov %.4f rad "
                 "(%.1f deg) aspect %.4f, we asked for %.1f deg%s",
                 pass, static_cast<unsigned>(dirty), renderedFov, renderedDegrees,
                 renderedAspect,
                 g_state.pendingValid[pass]
                     ? g_state.pendingFov[pass] * 57.29578f
                     : 0.0f,
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

    params->camera = savedCamera;
    params->mirrorCamera = savedMirror;
    if (mirrorMoved) {
        std::memcpy(params->mirrorEye, savedMirrorEye, sizeof(savedMirrorEye));
        std::memcpy(params->mirrorOrientation, savedMirrorBasis, sizeof(savedMirrorBasis));
        params->mirrorFov = savedMirrorFov;
    }
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
        logf("stereo: first stereo frame rendered -- eye separation %.4f world units, "
             "straight from the runtime",
             std::sqrt((vr[1].offset[0] - vr[0].offset[0]) *
                           (vr[1].offset[0] - vr[0].offset[0]) +
                       (vr[1].offset[1] - vr[0].offset[1]) *
                           (vr[1].offset[1] - vr[0].offset[1]) +
                       (vr[1].offset[2] - vr[0].offset[2]) *
                           (vr[1].offset[2] - vr[0].offset[2])));
    }
    return true;
}

}  // namespace gplvr
