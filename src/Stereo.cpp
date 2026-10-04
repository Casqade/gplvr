#include "Stereo.h"

#include "DataRebase.h"
#include "FlatFrames.h"
#include "FrameBudget.h"
#include "Hooks.h"
#include "Hud.h"
#include "Interpolation.h"
#include "Log.h"
#include "MenuScale.h"
#include "MirrorCache.h"
#include "MirrorConfig.h"
#include "Pe.h"
#include "RenderTarget.h"
#include "VrCamera.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
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

// Defined below, next to the euler round-trip check it exists for.
void gplMatrixToEuler(const float m[9], float& yaw, float& pitch, float& roll);

namespace {

// -- THE TWO MIRROR GLASSES ---------------------------------------------------------------
//
// Every 1967 car has two round mirror glasses, and GPL already gives each its own half of
// mirror.mip: the +Y (left) glass samples u 0.5-1, the -Y (right) glass u 0-0.5
// (docs/audit-2026-09-13/41-mirror-panes.md 1.2). Where each glass sits, which way it faces
// and how big it is are read out of the team's .3do, in car-local metres -- X forward, Y left,
// Z up, origin at the physics body -- with the normal on the face toward the driver. The -Y
// glass is the exact Y mirror of the +Y one on every car.
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

// gpl_mirrorViewCompute (0x454E30) puts the stock mirror eye at the body origin plus
// R * gpl_mirrorMountOffset, and 0x539470 holds (0, 0, 0.35).
constexpr float kMirrorMountHeight = 0.35f;

// The viewed car's team. gpl_simFrameSetup latches the viewed car's car+784 into
// gpl_viewCarDriverIndex every frame (-1 without one); gpl_driverGetTeamIndex turns it into
// the team index. That function reads a 147-byte driver record through sub_46EFE0, which
// returns NULL for an empty slot and is then dereferenced -- so the record's two presence
// bytes are tested first, the same test sub_46EFE0 makes. Driver slots run 0..31
// (sub_46B990: 1544 bytes each up to the driver records).
constexpr uintptr_t     kViewCarDriverIndex = 0x00776144;
constexpr uintptr_t     kDriverRecords = 0x006CB4F8;
constexpr int           kDriverRecordSize = 147;
constexpr int           kDriverSlots = 32;
constexpr uintptr_t     kDriverGetTeamIndex = 0x00985AA6;
constexpr unsigned char kDriverGetTeamIndexCode[18] = {0xFF, 0x74, 0x24, 0x04, 0xE8, 0x31,
                                                       0x95, 0xAE, 0xFF, 0x50, 0xE8, 0x0A,
                                                       0xFB, 0xFF, 0xFF, 0x59, 0x59, 0xC3};

// Leaning past the glass would send the field through 180 degrees and the projection
// through infinity. Neither limit is ever reached by a seated driver; they are here so
// that a tracking glitch cannot produce a degenerate frustum.
constexpr float kMirrorMinDistance = 0.15f;
constexpr float kMirrorMinFov = 0.0175f;  // 1 degree
constexpr float kMirrorMaxFov = 2.44f;    // 140 degrees

struct Entries {
    // The rasterizer entries are held as their GLOBALS, not as resolved pointers. The
    // table is filled in by GetProcAddress after we install and could be re-resolved on
    // a mode change, so every call reads the slot's current value.
    void** bitmapWidthSlot = nullptr;
    void** bitmapHeightSlot = nullptr;
    void** instanceAllocSlot = nullptr;
    void** instanceFreeSlot = nullptr;
    void** backBufferSlot = nullptr;
    void** polyDrawSlot = nullptr;

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
    int   eulerReports = 0;
    float worstEuler = 0.0f;
    float lastSeatPitch = 999.0f;

    // gpl_cameraCreate does not allocate; the engine passes it a static buffer. So do
    // we. 512 is four times what the object needs.
    alignas(16) unsigned char storage[2][512] = {};

    // The mirror panes: a camera per glass on a mipWidth/2 x mipHeight instance at (0, 0) of
    // the source surface, the shape of one half of mirror.mip. [0] is the +Y (left) glass,
    // copied to the right half of the texture; [1] is the -Y (right) glass, copied to x = 0.
    // Rebuilt with the eye targets, since rasterizer handles are recycled across mode changes.
    Half  pane[2];
    void* paneSource = nullptr;
    int   paneMipWidth = 0;
    int   paneMipHeight = 0;
    bool  panesGaveUp = false;
    int   paneBuildReports = 8;
    alignas(16) unsigned char paneStorage[2][512] = {};

    // Set around the mirror-only sceneRender.
    bool mirrorOnly = false;

    // The game's mirror camera for the frame being rendered, so a bind of it can be told apart
    // from the eye cameras when the panes are not in use.
    void* frameMirror = nullptr;

    // That camera's destination, read while the camera is still live and cleared when the
    // passes are done. gpl_simFrameRender frees the instance and zeroes p[43] on its way out,
    // so this is a value to compare copy-out arguments against for the length of the frame and
    // nothing to read through afterwards. Null means no stereo render is in progress, which is
    // what keeps a freed handle that some later allocation reused from being taken for the
    // mirror's.
    void* frameMirrorSurface = nullptr;
    int  paneCopies = 0;
    bool reportedNoCopy = false;

    // Why the panes are or are not in use, reported when it changes. Pointer comparison.
    const char* paneReason = nullptr;

    // The viewed car's team, asked only when the driver index changes.
    int         paneDriver = -1000;
    int         paneTeam = kTeamLotus;
    const char* paneTeamFallback = nullptr;
    bool        teamCodeChecked = false;
    bool        teamCodeMatches = false;
    bool        paneDriverRefusedSaid = false;
    bool        paneGeometryReported = false;

    // The aim report, rate-limited while the head moves.
    long long paneAimReportQpc = 0;
    float     paneAimReportEye[3] = {0.0f, 0.0f, 0.0f};
};

State g_state;

// polyDraw's gate for the mirror-only render: set at its main cameraBind, cleared when the
// call returns. A byte the naked thunk tests, so it has to be a plain global. The thunk
// forwards through g_paneNextPolyDraw, whatever held the polyDraw global before it.
volatile unsigned char g_paneDropPolys = 0;
void*                  g_paneNextPolyDraw = nullptr;

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
    // TAKEN ONCE, and that is enough: GPL never moves its near plane. Measured over a
    // whole session with the eye cameras tracking it per frame and reporting every change -- one
    // line at startup, 0.125, and nothing after it. Re-reading it per frame was built, run and
    // removed; do not build it again.
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

void releasePanes() {
    releaseHalf(g_state.pane[0]);
    releaseHalf(g_state.pane[1]);
    g_state.paneSource = nullptr;
    g_state.paneMipWidth = 0;
    g_state.paneMipHeight = 0;
}

void reportPanes(const char* reason) {
    if (g_state.paneReason == reason) return;
    g_state.paneReason = reason;
    logf("mirror: %s", reason);
}

// Builds the two pane cameras from the live mirror camera, whose surface is GPL's
// mipWidth x mipHeight instance for this frame. Cheap after the first frame: it only rebuilds
// when the source surface or the mirror's size changes.
bool ensurePanes(void* mirrorCamera) {
    State& s = g_state;
    if (s.panesGaveUp || !s.sourceSurface) return false;

    void* mirrorSurface = cameraPointer(mirrorCamera, kCameraSurface);
    int   mipWidth = bitmapWidth(mirrorSurface);
    int   mipHeight = bitmapHeight(mirrorSurface);
    if (mipWidth < 2 || mipHeight < 1) return false;

    if (s.pane[0].camera && s.pane[1].camera && s.paneSource == s.sourceSurface &&
        s.paneMipWidth == mipWidth && s.paneMipHeight == mipHeight)
        return true;

    releasePanes();

    auto alloc = reinterpret_cast<BitmapInstanceAllocFn>(*g_entries.instanceAllocSlot);
    auto release = reinterpret_cast<BitmapInstanceFreeFn>(*g_entries.instanceFreeSlot);
    if (!alloc || !release) return false;

    int   width = mipWidth / 2;
    float fov = cameraFloat(mirrorCamera, kCameraFov);
    float nearZ = cameraFloat(mirrorCamera, kCameraNear);

    for (int side = 0; side < 2; ++side) {
        void* instance = nullptr;
        if (!alloc(&instance, s.sourceSurface, 0, 0, width, mipHeight) || !instance ||
            bitmapWidth(instance) != width || bitmapHeight(instance) != mipHeight) {
            logf("mirror: pane %d instance (%p, 0,0 %dx%d) failed or came back %dx%d -- the "
                 "mirror stays one stock image",
                 side, s.sourceSurface, width, mipHeight, bitmapWidth(instance),
                 bitmapHeight(instance));
            if (instance) release(instance);
            releasePanes();
            s.panesGaveUp = true;
            return false;
        }
        // aspect 0.5 on a w = h/2 surface makes the horizontal and vertical angles equal
        // (tan(v/2) = tan(h/2) * aspect * height / width); aimPane refines it per glass.
        std::memset(s.paneStorage[side], 0, kCameraSize);
        void* camera = g_entries.cameraCreate(s.paneStorage[side], instance, fov, 0.0f, 0.0f,
                                              nearZ, 0.5f);
        if (!camera) {
            logf("mirror: pane %d camera could not be created -- the mirror stays one stock "
                 "image",
                 side);
            release(instance);
            releasePanes();
            s.panesGaveUp = true;
            return false;
        }
        s.pane[side].surface = instance;
        s.pane[side].camera = camera;
    }

    s.paneSource = s.sourceSurface;
    s.paneMipWidth = mipWidth;
    s.paneMipHeight = mipHeight;
    if (s.paneBuildReports > 0) {
        --s.paneBuildReports;
        logf("mirror: pane cameras built -- mirror.mip is %dx%d, so each pane is %dx%d at 0,0 "
             "of surface %p. +Y (left glass) surface %p camera %p, copied to x = %d; -Y "
             "(right glass) surface %p camera %p, copied to x = 0. near %.4f",
             mipWidth, mipHeight, width, mipHeight, s.sourceSurface, s.pane[0].surface,
             s.pane[0].camera, width, s.pane[1].surface, s.pane[1].camera, nearZ);
    }
    return true;
}

// polyDraw(type, count, vertices, tags): roglv2 abandons a polygon with fewer than three
// vertices, so zeroing the count drops it through the rasterizer's own early-out.
__declspec(naked) void paneDropThunk() {
    __asm {
        cmp byte ptr [g_paneDropPolys], 0
        je  passThrough
        mov dword ptr [esp+8], 0
    passThrough:
        jmp dword ptr [g_paneNextPolyDraw]
    }
}

// Puts the gate on the polyDraw global, chaining whatever is there. Checked every frame:
// gpl_rasterResolveEntryPoints rewrites the globals on a mode change.
// -- which segment a pass will walk ------------------------------------------------------------
//
// sub_460B40(hint, &out, x, y, z): the search gpl_sceneRender makes at 0x487D73 over the global
// segment array, seeded by (u16)p[23] and driven by the eye position in dlong units (metres x
// 19685, dbl_53A020). It reads globals only, so it can be called with the values the pass is
// about to use. Returns the segment index the walk is centred on.
using WalkSegmentFn = int(__cdecl*)(int hint, int* out, int x, int y, int z);
constexpr uintptr_t kWalkSegmentOf = 0x00460B40;
constexpr double    kDlongUnitsPerMetre = 19685.0;

// THE FALLBACK. gpl_sceneRender asks this function where the camera is on the track, and uses
// the answer to centre the world walk and to set gpl_selectorCameraDlong / Dlat / Height. In
// flat GPL the camera sits in the cockpit, so the answer is the car's; in VR the head pose moves
// the point off it -- measured at 0.03 to 0.32 m in the mirror lines, since recentring puts the
// origin at the driver's own eye -- and at some places, measured at the bottom of Eau Rouge, the
// search then returns a segment four to six away and the frame walks another part of the track
// entirely. Asking about the CAR's position instead restores what flat GPL does, keeps all four
// outputs consistent with each other, and needs no threshold.
//
// ONLY WHEN THE CAMERA'S OWN ANSWER FAILS, and that qualification was paid for. Substituting on
// every lookup centred the drawn segment range on a point metres from the camera; where a
// boundary fell between the two the camera's own segment left the range and a distant terrain
// tile filled the frame below its own horizon. See walkLookupBody.
//
// Both call sites are redirected: 0x487D73 for the eye pass, 0x4877D9 for the mirror pass, so the
// whole frame is centred on the same point.
void* g_walkLookupOriginal = nullptr;
unsigned g_walkSubstituted = 0;   // calls where the car's position was used
unsigned g_walkPassedThrough = 0; // calls with no viewed car, left alone

// THE DEFECT THIS GUARDS, as measured in report 84.
//
// The walkmark trace caught it directly: for ONE frame -- both eye passes -- the dlong this
// search writes jumps 32 m and comes straight back, while the eye's and the car's own dlongs in
// the interpolation ring are smooth to a tenth of a metre across the whole window. So the input
// is clean and the answer is not.
//
// The mechanism is in the search. It steps one segment an iteration from its seed and exits on
// `while (v8 & v9)` -- it stops as soon as two consecutive probes stop agreeing on which way to
// go -- and it then writes the dlong of the point measured against WHATEVER SEGMENT IT STOPPED
// ON. Stop one segment early and the dlong is out by a segment length, which at Spa is tens of
// metres. gpl_sceneRender hands that dlong straight to gpl_worldRenderSegmentRange as the centre
// of the walk, so that frame draws a different stretch of track: terrain over everything, no
// road, the bare ground showing through.
//
// THE SEED IS NOT THE VARIABLE, and this is measured, not assumed. An earlier instrument reseated
// the seed from the blended position and reported `hint reseated 0, corrected 0` -- it never fired
// -- and the decisive case had the SAME seed (12) with two positions 0.46 m apart answering 12 and
// 17. A search that steps one segment an iteration cannot legitimately move five segments for
// 0.46 m of input. What decides where it stops is the ORDER of the probes, not the seed, so any
// fix that supplies a better seed is a no-op wearing a new hat.
//
// So the seed is left exactly as gpl_sceneRender passes it, and the ANSWER is checked instead: a
// dlong cannot advance further than the car did. When it has, the car's own dlong replaces it --
// record byte 12, the value the sim maintains, which the mark ring shows moving smoothly to a
// tenth of a metre through the very frames where this search spiked 32 m. Live authoritative
// data, not a stale value and not a guess about the search.
constexpr uintptr_t kTrackLengthDlong = 0x006B3884;
constexpr uintptr_t kSegmentCount = 0x006AFB08;
constexpr uintptr_t kSegmentTable = 0x006AEA1C;  // float rows: +4 start m, +12 scale, +16 length
constexpr int       kSegmentRowWords = 360;      // 1440 bytes a row

// GPL'S OWN CONTAINMENT ANSWER, which its search asks for and then throws away.
//
// gpl_segmentPlacePoint 0x4608D0 projects the point onto one segment and returns:
//     0  the point is INSIDE this segment -- the dlong it wrote is real
//     1  the point is past its end
//     2  the point is before its start
// On 1 and 2 it still writes a dlong: `segment.baseDlong + projection`, the point EXTRAPOLATED
// along that segment's axis. That number is only meaningful if the track continues the way this
// segment does.
//
// gpl_cameraTrackPositionFromWorld loops it, stepping one segment towards the point, and exits on
// `while (v8 & v9)` -- as soon as two consecutive probes stop sharing a bit. A 1 followed by a 2
// (stepped forward and overshot) shares nothing, so the loop stops there and PUBLISHES THE
// EXTRAPOLATION from a segment the point is not in. There is no step-back-and-accept.
//
// So the failure has a name and a test: the point fell in a seam between two segments'
// longitudinal spans, and the answer is an extrapolation. Asking this function directly about the
// segment the search settled on is exactly the check the engine omits -- no threshold in it.
constexpr uintptr_t kSegmentArray = 0x006B7FD8;   // dword_6B7FD8, the array 0x460B40 indexes
constexpr uintptr_t kSegmentArrayCount = 0x006AFB08;
constexpr uintptr_t kSegmentPlacePoint = 0x004608D0;
using PlacePointFn = int(__cdecl*)(const void* segment, int x, int y, int* dlong, int* dlat);

// 0 inside, 1 past the end, 2 before the start, -1 when it could not be asked.
//
// Both globals go through rebase because this reads a pointer out of one, indexes it, and hands
// the element to the engine as an object pointer. The non-null and range tests below cannot tell
// a wrong global from a right one -- a plausible pointer and a count of a few hundred are common
// shapes -- and the install gate is two code sites that are identical in every link of 1.2.0.2.
int placementOf(int segment, int x, int y) {
    uintptr_t arrayAt = rebase(kSegmentArray);
    uintptr_t countAt = rebase(kSegmentArrayCount);
    if (!arrayAt || !countAt) return -1;
    auto base = *reinterpret_cast<const void* const* const*>(arrayAt);
    int  count = *reinterpret_cast<const int*>(countAt);
    if (!base || count <= 0 || count > 8192 || segment < 0 || segment >= count) return -1;
    const void* node = base[segment];
    if (!node) return -1;
    int dlong = 0;
    int dlat = 0;
    return reinterpret_cast<PlacePointFn>(kSegmentPlacePoint)(node, x, y, &dlong, &dlat);
}

int      g_walkLastDlong = 0;
float    g_walkLastPosition[3] = {};
bool     g_walkHaveLast = false;
unsigned g_walkCorrected = 0;      // answers replaced with the car's own dlong
unsigned g_walkCorrectedBig = 0;   // of those, the ones past kBigExcess
unsigned g_walkOutside = 0;        // GPL settled on a segment the point is not in
unsigned g_walkOverMargin = 0;     // the dlong moved further than the car did
unsigned g_walkNoDlong = 0;        // rejected, but the car's dlong was unavailable
float    g_walkWorstExcess = 0.0f; // biggest rejected overshoot, metres

// A HYSTERESIS AND A FRAME-TO-FRAME JUMP COUNTER USED TO BE HERE. Both are gone.
//
// The hysteresis held the car-derived answer until GPL's own came back into agreement, to stop the
// switch between two disagreeing sources being the jump. Measured with an exact-agreement
// window: 38 re-asks, 26 answers held, ONE release on agreement. The two sources do not reconverge,
// so it suppressed a steady disagreement rather than smoothing a transient -- and a jump still got
// through. The table lookup removes the second source instead of filtering it.
//
// The counter compared each frame's accepted segment with the last frame's and flagged changes of
// more than one, which is a flip rather than motion: a car at 300 km/h advances 0.93 m in a 90 Hz
// frame and segments run to tens of metres. It caught the defect red-handed -- seven segments on a
// frame where the car moved 22 cm, in five consecutive clean windows -- and it is out because the
// table lookup makes that structurally impossible, which the driver has confirmed by eye.
//
// The one thing still worth saying is a REFUSAL: if the table cannot answer, the answer falls back
// to GPL's search and the artifact can return. That is a silent failure, so it says so once.
bool g_walkTableSaidRefused = false;

// HOW OFTEN THE TABLE ANSWERS, counted per lookup rather than announced once.
//
// The notice above fires on the first refusal of a track and then stays quiet, so a refusal on one
// frame in several hundred reads exactly like no refusal at all -- and one frame of GPL's own
// search is one frame of the artifact. A one-frame artifact that gets more frequent as the frame
// rate rises is what a per-lookup fallback looks like: its chance is per draw, so drawing more
// often multiplies it while nothing about the simulation changes.
//
// The two causes are separated because they want different fixes. No view dlong means the blend
// had nothing to offer for this frame; a dlong outside the table means the table itself did not
// cover where the camera is. A track change legitimately produces a burst of either while the new
// table loads, so the session totals are read against the per-window counts: a burst in the window
// after a mode change is a load, a trickle spread over every window while driving is the defect.
unsigned g_walkTableAnswered = 0;      // the dlong decided the segment
unsigned g_walkTableNoViewDlong = 0;   // the blend had no dlong for the view
unsigned g_walkTableOutside = 0;       // a dlong, but outside the table
unsigned g_walkTableRefusedTotal = 0;  // both causes, over the session
unsigned g_walkTableWorstWindow = 0;   // most refusals in any one window

// BOTH EYES ASK ABOUT THE HEAD CENTRE, and this is the fix for the one-frame eye-segment
// artifact. MEASURED AND CLOSED.
//
// gpl_sceneRender picks the segment range from the eye POSITION, and each pass handed it its
// own -- so the question had an answer per eye rather than per frame, and nothing made the two
// halves of one picture agree. The fix is to ask once, about the point between the eyes: the
// answer is then the same int by construction, with no threshold and no comparison deciding it.
//
// WHAT THE INSTRUMENT FOUND BEFORE IT WAS REMOVED. A shadow lookup asked the search about each
// eye's own position as well, purely to record what it WOULD have said. Over 510 seconds it
// caught three frames -- left 12 against right 17, left 224 against right 223, left 225 against
// right 229 -- and the drawn ranges never once differed. **Four and five segments apart for
// 65 mm of separation**: that is the search's stopping point flipping, report 84's mechanism,
// not the eyes being in genuinely different parts of the track. Both answers also passed the
// containment test, so no check could have caught it -- only asking one question could.
//
// That is also why picking either answer is correct rather than merely consistent: the two
// positions are 65 mm apart, so a segment range right for one is right for the other.
//
// Only the eye site is redirected. The mirror site asks about the mirror camera, which is a
// different place and a different question.
int  g_walkCentre[3] = {};
bool g_walkHaveCentre = false;
int  g_walkPass = -1;  // which eye pass is rendering, -1 outside the loop

void walkSetCentre(float x, float y, float z) {
    auto toUnits = [](float metres) {
        return static_cast<int>(static_cast<double>(metres) * kDlongUnitsPerMetre);
    };
    g_walkCentre[0] = toUnits(x);
    g_walkCentre[1] = toUnits(y);
    g_walkCentre[2] = toUnits(z);
    g_walkHaveCentre = true;
}

// TWO DIFFERENT THINGS LOOK THE SAME TO THE CONTRACT, so they are counted apart.
//
// A one-frame excursion of tens of metres moves the walk's centre by more than a segment for a
// single frame: the frames either side draw the correct range, that one does not, and the change
// is what the eye sees. That is the artifact.
//
// A SUSTAINED disagreement of a few metres is a static shift of a fraction of a segment, held for
// hundreds of frames. It cannot be seen, and it may not even be the lookup failing: the car
// record's dlong (byte 12) and its position (bytes 16..27) may simply be measured from different
// points on the car, with the gap opening in corners. If the big bucket alone is what stops the
// artifact, the margin should be raised and the small corrections dropped rather than overriding
// GPL on a quarter of all passes for no visible gain.
constexpr float kBigExcess = 10.0f;

// Signed difference the short way round the lap, in dlong units.
// THE SEGMENT FOR A DLONG, by table lookup instead of by search.
//
// gpl_cameraTrackPositionFromWorld answers the same question by walking from a hint segment, and
// that walk is not a continuous function of its input: it can settle on a segment the point is not
// inside (report 84) and which one it settles on moves with the hint and with sub-centimetre
// changes in the position. The segment sets the origin of the drawn range, so a multi-segment
// change shifts the whole world for a frame.
//
// The table at 0x006AEA1C is an ordered array of 1440-byte rows whose start dlong is at +4 and
// whose length is +16 / +12, so this is monotone: the answer moves by one segment when the dlong
// crosses a boundary and cannot move by four unless the DLONG moved by four segments' worth. It
// therefore still follows a camera cut, a car reset or a lap crossing exactly, because those move
// the dlong; what it cannot do is invent a jump the dlong did not make.
//
// Returns -1 when the table cannot be placed or read, or the dlong is outside it.
int segmentForDlong(float metres) {
    // Through rebase: the table pointer is dereferenced and then indexed by up to 4096 rows of
    // 1440 bytes, so a global belonging to another link reads megabytes away from anything.
    uintptr_t countAt = rebase(kSegmentCount);
    uintptr_t tableAt = rebase(kSegmentTable);
    if (!countAt || !tableAt) return -1;
    int          count = *reinterpret_cast<const int*>(countAt);
    const float* table = *reinterpret_cast<const float* const*>(tableAt);
    if (count <= 0 || count > 4096 || !table) return -1;

    int low = 0;
    int high = count - 1;
    while (low <= high) {
        int          mid = low + (high - low) / 2;
        const float* row = table + kSegmentRowWords * mid;
        float        start = row[1];
        if (!(row[3] > 0.0f)) return -1;
        float end = start + row[4] / row[3];
        if (metres < start) {
            high = mid - 1;
        } else if (metres >= end) {
            low = mid + 1;
        } else {
            return mid;
        }
    }
    return -1;
}

// ANYTHING COMPARING TWO SEGMENT INDICES needs the shortest way round the ring, the way dlongDelta
// below does for the dlong: the count is at kSegmentCount, and without the wrap a lap crossing reads
// as a jump of the whole track. A helper for that lived here and went with the jump counter.

int dlongDelta(int from, int to) {
    // A zero length means no wrap, which is also what a global this image cannot be shown to
    // keep should produce: the delta passes through unwrapped rather than being folded around a
    // track length read from somewhere else.
    uintptr_t lengthAt = rebase(kTrackLengthDlong);
    int       length = lengthAt ? *reinterpret_cast<const int*>(lengthAt) : 0;
    int       delta = to - from;
    if (length <= 0) return delta;
    while (delta > length / 2) delta -= length;
    while (delta < -length / 2) delta += length;
    return delta;
}

int walkLookupBody(int hint, int* out, int x, int y, int z) {
    auto original = reinterpret_cast<WalkSegmentFn>(g_walkLookupOriginal);

    // THE EYE'S OWN ANSWER FIRST, and kept whenever it is sound.
    //
    // Substituting the car unconditionally centred the whole drawn segment range on a point
    // metres from the camera, and where a segment boundary fell between the two the camera's
    // own segment dropped out of the range: the road under the camera was not drawn and a
    // distant terrain tile filled the frame below its own horizon. Measured at
    // dlong 13875-13877, and the size of it tracked the separation -- a notch out of one tyre
    // at 1.19 m in the cockpit, half the frame at 4.98 m in the chase camera.
    //
    // The containment test decides. Report 84 measured 308 corrections in 66,160 passes, so
    // the eye's answer is sound on better than 99% of them and the car is a fallback, not a
    // replacement.
    int  segment = original(hint, out, x, y, z);
    bool sound = placementOf(segment, x, y) == 0;

    // THE DLONG DECIDES THE SEGMENT, when the table can answer.
    //
    // The search above stays because out[1], the dlat, is its work and is a level-of-detail input.
    // What is replaced is the segment index and the dlong, taken together from one smooth source so
    // the pair cannot disagree with each other.
    float viewDlong = 0.0f;
    bool  viewSnapped = false;
    bool  haveViewDlong = interpolationViewDlong(viewDlong, viewSnapped);
    int   fromTable = haveViewDlong ? segmentForDlong(viewDlong) : -1;
    if (fromTable >= 0) {
        ++g_walkTableAnswered;
        out[0] = static_cast<int>(static_cast<double>(viewDlong) * kDlongUnitsPerMetre);
        return fromTable;
    }

    if (haveViewDlong)
        ++g_walkTableOutside;
    else
        ++g_walkTableNoViewDlong;
    ++g_walkTableRefusedTotal;

    if (!g_walkTableSaidRefused) {
        g_walkTableSaidRefused = true;
        logf("walk: the segment table could not answer for the view's dlong, so the camera's "
             "segment falls back to GPL's own search for as long as that lasts -- which is the "
             "unstable answer the table exists to replace. Either the view dlong is unavailable, "
             "or the dlong is outside the table, or this image's globals could not be placed "
             "(deltas %s).",
             rebaseSummary());
    }

    if (sound) {
        ++g_walkPassedThrough;
        g_walkHaveLast = false;  // the memory is about the substituted point, not this one
        return segment;
    }

    // The eye's answer is an extrapolation from a segment it is not in -- report 84's defect.
    // Ask again about the car, whose position the sim maintains and which flat GPL would have
    // asked about anyway.
    float blended[3] = {}, tick[3] = {};
    if (!interpolationViewedCarPosition(blended, tick)) {
        // Nothing to substitute, so GPL's answer goes out as it always did.
        ++g_walkPassedThrough;
        g_walkHaveLast = false;
        return segment;
    }

    auto toUnits = [](float metres) {
        return static_cast<int>(static_cast<double>(metres) * kDlongUnitsPerMetre);
    };

    ++g_walkSubstituted;
    int ux = toUnits(blended[0]);
    int uy = toUnits(blended[1]);
    int uz = toUnits(blended[2]);

    segment = original(hint, out, ux, uy, uz);

    // How far the car moved since the last accepted answer. All four calls in a frame -- two eyes,
    // and the mirror pass with them -- use the same substituted point, so within a frame this is
    // zero and the two eyes have to agree.
    float dx = blended[0] - g_walkLastPosition[0];
    float dy = blended[1] - g_walkLastPosition[1];
    float dz = blended[2] - g_walkLastPosition[2];
    float moved = std::sqrt(dx * dx + dy * dy + dz * dz);

    // THE PRIMARY TEST: did the search settle on a segment the point is actually in?
    int placement = placementOf(segment, ux, uy);
    bool extrapolated = placement > 0;
    if (extrapolated) ++g_walkOutside;

    // The secondary net, for the one failure the containment test cannot see. A segment whose
    // type is neither 1 nor 2 makes gpl_segmentPlacePoint return 0 -- "inside" -- WITHOUT writing
    // either out-parameter, and gpl_cameraTrackPositionFromWorld then publishes the values its
    // in/out arguments arrived with: the seed as the dlong and the y coordinate as the dlat.
    // Nothing about that is detectable from the return value, but a dlong cannot advance further
    // than the car did.
    //
    // NOT A TUNING KNOB. Measured over 66,160 passes: containment triggered all 308 corrections
    // and this test caught nothing containment had not, so there is nothing here for anyone to
    // tune. It is fixed well above any legitimate advance -- a car at 300 km/h covers 0.93 m in a
    // 90 Hz frame -- because the case it exists for produces a dlong of the SEED, a couple of
    // hundred units, against a real position thousands of metres along the lap.
    constexpr float kMargin = 10.0f;
    constexpr float kGiveUp = 10.0f;  // a restart or teleport really did move; re-learn from it
    bool impossible = false;
    float excess = 0.0f;
    if (g_walkHaveLast && moved < kGiveUp) {
        excess = std::fabs(static_cast<float>(dlongDelta(g_walkLastDlong, out[0]))) /
                     static_cast<float>(kDlongUnitsPerMetre) -
                 moved;
        impossible = excess > kMargin;
        if (impossible) ++g_walkOverMargin;
    }

    if (extrapolated || impossible) {
        if (excess > g_walkWorstExcess) g_walkWorstExcess = excess;

        // Only out[0] is replaced. out[1], the dlat, feeds gpl_selectorCameraDlat, a
        // level-of-detail input, and out[2] is the z we passed in, echoed back.
        float carDlong = 0.0f;
        if (interpolationViewedCarDlong(carDlong)) {
            ++g_walkCorrected;
            if (excess >= kBigExcess) ++g_walkCorrectedBig;
            out[0] = toUnits(carDlong);
        } else {
            ++g_walkNoDlong;
        }
    }

    g_walkLastDlong = out[0];
    g_walkLastPosition[0] = blended[0];
    g_walkLastPosition[1] = blended[1];
    g_walkLastPosition[2] = blended[2];
    g_walkHaveLast = true;
    return segment;
}

// One thunk per site. The two sites are the two lookups gpl_sceneRender makes -- one for the
// main camera, one for the mirror's -- and only the main one is the eye's own walk, which is
// the one the head centre replaces.
//
// ANYTHING MEASURING THIS ANSWER MUST NOT USE out[0] AS ITS CONTROL. out[0] changes SOURCE between
// the paths below -- the search writes the dlong for the camera on one and for the car on the other,
// and the table writes the view's -- so it jumps exactly when the source switches, which is the
// event such a measurement would be looking for. A jump counter built that way reported 5.47 m of
// advance on a single 90 Hz frame, or 492 m/s. Use the view's own dlong.
int __cdecl walkLookupMainThunk(int hint, int* out, int x, int y, int z) {
    if (g_walkHaveCentre && g_walkPass >= 0 && g_walkPass < 2) {
        x = g_walkCentre[0];
        y = g_walkCentre[1];
        z = g_walkCentre[2];
    }
    return walkLookupBody(hint, out, x, y, z);
}

int __cdecl walkLookupMirrorThunk(int hint, int* out, int x, int y, int z) {
    return walkLookupBody(hint, out, x, y, z);
}

void walkPassBegin(int pass) { g_walkPass = pass; }

void walkFrameEnd() { g_walkPass = -1; }

// Both sites are checked against their stock bytes before either is written, and neither is
// written unless both match -- half a substitution would centre the eye pass on the car and the
// mirror pass on the mirror, which is worse than leaving both alone.
bool installWalkLookup() {
    struct Site {
        uintptr_t     address;
        unsigned char stock[5];
        void*         thunk;
    };
    static const Site kSites[] = {
        {0x00487D73, {0xE8, 0xC8, 0x8D, 0xFD, 0xFF},
         reinterpret_cast<void*>(&walkLookupMainThunk)},  // the eye pass
        {0x004877D9, {0xE8, 0x62, 0x93, 0xFD, 0xFF},
         reinterpret_cast<void*>(&walkLookupMirrorThunk)},  // the mirror pass
    };
    static bool tried = false;
    if (tried) return g_walkLookupOriginal != nullptr;
    tried = true;

    for (const Site& site : kSites) {
        auto bytes = reinterpret_cast<const uint8_t*>(site.address);
        if (std::memcmp(bytes, site.stock, sizeof(site.stock)) == 0) continue;
        logf("walk: %08X does not hold its stock call (%02X %02X %02X %02X %02X) -- the segment "
             "lookup is left as the game makes it",
             static_cast<unsigned>(site.address), bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4]);
        return false;
    }

    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const uint8_t*>(kSites[0].address) + 1, sizeof(rel));
    g_walkLookupOriginal = reinterpret_cast<void*>(kSites[0].address + 5 + rel);

    for (const Site& site : kSites) {
        int32_t offset = static_cast<int32_t>(reinterpret_cast<uintptr_t>(site.thunk) -
                                              (site.address + 5));
        if (patchBytes(reinterpret_cast<void*>(site.address + 1), &offset, sizeof(offset)))
            continue;
        logf("walk: could not redirect the segment lookup at %08X",
             static_cast<unsigned>(site.address));
        g_walkLookupOriginal = nullptr;
        return false;
    }

    logf("walk: the camera's segment lookup at 004877D9 and 00487D73 goes through gplvr. GPL's "
         "answer for the camera's own position is checked against its containment test and kept "
         "whenever the camera is in the segment the search settled on; only when it is not is "
         "the question asked again about the VIEWED CAR (report 84). No setting: it is on in VR "
         "and in flat mode with interpolation, which is wherever a blended car position exists "
         "to fall back to. Original at %p.",
         g_walkLookupOriginal);
    return true;
}

bool ensurePolyGate() {
    void** slot = g_entries.polyDrawSlot;
    if (!slot || !*slot) return false;
    void* thunk = reinterpret_cast<void*>(&paneDropThunk);
    if (*slot == thunk) return true;

    g_paneNextPolyDraw = *slot;
    if (!patchBytes(slot, &thunk, sizeof(thunk))) return false;
    static int reports = 4;
    if (reports > 0) {
        --reports;
        logf("mirror: polyDraw gate on global %p, forwarding to %p",
             reinterpret_cast<void*>(slot), g_paneNextPolyDraw);
    }
    return true;
}

// No C++ objects in here, so structured exception handling is allowed. -2 if it faulted.
int callDriverGetTeamIndex(int driver) {
    __try {
        return reinterpret_cast<int(__cdecl*)(int)>(kDriverGetTeamIndex)(driver);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// The viewed car's glass geometry. Lotus when the team cannot be read, with the reason in
// paneTeamFallback. `changed` is set when the driver index moved, which is the only time the
// game is asked, or when the car set changed under the same driver. Each ask also selects the
// car's mirror aim (MirrorConfig); without a viewed car the last car's aim stays.
const PaneGeometry& viewedCarGlass(bool& changed) {
    State& s = g_state;
    changed = false;
    if (mirrorConfigCarSetChanged()) s.paneDriver = -1000;

    // Through rebase: nothing about the value read says whether it is a driver index. On the
    // plain link this global is 8 lower and the read returned a float -- around 61.8, moving
    // every frame -- so the team lookup refused for every car and the Lotus glasses stood in,
    // 7650 times in one session.
    uintptr_t driverAt = rebase(kViewCarDriverIndex);
    if (!driverAt) {
        if (!s.paneDriverRefusedSaid) {
            s.paneDriverRefusedSaid = true;
            logf("mirror: the viewed car's driver index %08X cannot be placed in this image "
                 "(deltas %s), so every car gets the Lotus glasses",
                 static_cast<unsigned>(kViewCarDriverIndex), rebaseSummary());
        }
        return kPaneGeometry[s.paneTeam];
    }
    int driver = *reinterpret_cast<const int*>(driverAt);
    if (driver == s.paneDriver) return kPaneGeometry[s.paneTeam];

    s.paneDriver = driver;
    changed = true;

    if (!s.teamCodeChecked) {
        s.teamCodeChecked = true;
        s.teamCodeMatches = std::memcmp(reinterpret_cast<const void*>(kDriverGetTeamIndex),
                                        kDriverGetTeamIndexCode,
                                        sizeof(kDriverGetTeamIndexCode)) == 0;
    }

    const char* fallback = nullptr;
    int         team = -1;
    if (!s.teamCodeMatches) {
        fallback = "gpl_driverGetTeamIndex (0x985AA6) does not hold the expected code";
    } else if (driver < 0 || driver >= kDriverSlots) {
        fallback = "there is no viewed car";
    } else if (!rebase(kDriverRecords)) {
        fallback = "the driver records cannot be placed in this image";
    } else {
        auto record = reinterpret_cast<const unsigned char*>(rebase(kDriverRecords) +
                                                             kDriverRecordSize * driver);
        if (!record[0] && !record[1]) {
            fallback = "the viewed car's driver record is empty";
        } else {
            team = callDriverGetTeamIndex(driver);
            if (team < 0 || team >= kTeamCount)
                fallback = "the team index is outside the 1967 table (another carset?)";
        }
    }

    if (driver >= 0 && driver < kDriverSlots) mirrorConfigViewedTeam(team, fallback);

    s.paneTeam = fallback ? kTeamLotus : team;
    s.paneTeamFallback = fallback;
    if (fallback)
        logf("mirror: driver %d -> team index %d: %s -- using the Lotus glasses", driver, team,
             fallback);
    return kPaneGeometry[s.paneTeam];
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
    g_entries.polyDrawSlot = binding.raster.polyDraw;
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
    // In a mirror-only render the only camera bound other than a pane is the main one, and
    // everything drawn from here to the end of the call would land in the eye.
    if (g_state.mirrorOnly && camera && camera != g_state.pane[0].camera &&
        camera != g_state.pane[1].camera)
        g_paneDropPolys = 1;

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

bool stereoMirrorOnlyPass() { return g_state.mirrorOnly; }

void stereoInstallWalkLookup() {
    // NO SETTING. The defect is GPL's own search answering from a segment the camera is not in,
    // and the fallback is the viewed car's blended position -- so this is wanted wherever such a
    // position exists, which is VR always and flat mode whenever the blend is on. Anywhere else
    // the thunk would find no car to ask about and hand every lookup straight back, so
    // installing it would buy one containment probe per lookup and nothing at all.
    if (g_options.vr || g_options.frameRate > 0) installWalkLookup();
}

bool stereoDroppingPolygons() { return g_paneDropPolys != 0; }

bool stereoWalkFormat(char* out, size_t size) {
    unsigned refused = g_walkTableNoViewDlong + g_walkTableOutside;
    if (refused > g_walkTableWorstWindow) g_walkTableWorstWindow = refused;
    if (!g_walkTableAnswered && !refused && !g_walkSubstituted && !g_walkPassedThrough)
        return false;

    // The table's record first, and it prints on a window where nothing went wrong: a refusal
    // rate is only readable against the number of lookups that asked.
    // _TRUNCATE returns -1 rather than failing, so a line that outgrows its buffer silently loses
    // whatever was added last -- which is always the newest instrument. Say so instead.
    int written = _snprintf_s(out, size, _TRUNCATE,
                              "walk: the table answered %u of %u, refused %u (%u with no view "
                              "dlong, %u with a dlong outside it) | session refusals %u, worst "
                              "window %u",
                              g_walkTableAnswered, g_walkTableAnswered + refused, refused,
                              g_walkTableNoViewDlong, g_walkTableOutside,
                              g_walkTableRefusedTotal, g_walkTableWorstWindow);

    // What a refusal then led to, so the two are read together. Silent while the table is
    // answering every lookup, which is when GPL's search is never reached.
    if (written >= 0 && (g_walkSubstituted || g_walkPassedThrough))
        written = _snprintf_s(
            out + written, size - static_cast<size_t>(written), _TRUNCATE,
            " || GPL's search was used: the camera's own answer stood %u times, asked again about "
            "the car %u | of those, %u still OUTSIDE the segment GPL settled on, %u impossible "
            "for the distance moved, %u CORRECTED from the car's own dlong (%u of them 10 m or "
            "more, the kind that is visible), worst %.1f m over, %u with no car dlong",
            g_walkPassedThrough, g_walkSubstituted, g_walkOutside, g_walkOverMargin,
            g_walkCorrected, g_walkCorrectedBig, static_cast<double>(g_walkWorstExcess),
            g_walkNoDlong);
    if (written < 0) {
        static bool said = false;
        if (!said) {
            said = true;
            logf("walk: THE LINE ABOVE IS TRUNCATED -- it does not fit %zu bytes, so its last "
                 "fields are missing. Grow the buffer in Hooks.cpp.",
                 size);
        }
    }
    g_walkSubstituted = 0;
    g_walkPassedThrough = 0;
    g_walkCorrected = 0;
    g_walkCorrectedBig = 0;
    g_walkOutside = 0;
    g_walkOverMargin = 0;
    g_walkNoDlong = 0;
    g_walkWorstExcess = 0.0f;
    g_walkTableAnswered = 0;
    g_walkTableNoViewDlong = 0;
    g_walkTableOutside = 0;
    return true;
}

int stereoMirrorCopy(int writeBitmap, int readSurface, int x) {
    State& s = g_state;
    if (!s.frameMirrorSurface) return x;

    // The three surfaces a mirror copy-out can read from: either pane when the glasses render
    // separately, GPL's own frame-corner instance when they do not. Everything else going
    // through bitmapDraw in a frame -- and the HUD is most of a hundred of them -- leaves here
    // on this comparison.
    auto handleOf = [](void* pointer) {
        return static_cast<int>(reinterpret_cast<intptr_t>(pointer));
    };
    bool fromPane0 = s.pane[0].surface && readSurface == handleOf(s.pane[0].surface);
    if (!fromPane0 && !(s.pane[1].surface && readSurface == handleOf(s.pane[1].surface)) &&
        readSurface != handleOf(s.frameMirrorSurface))
        return x;

    // The first argument is where the pixels LAND -- mirror.mip's level-0 bitmap, which is the
    // one thing in this process that names the texture the two glasses are read out of. It is a
    // handle, like everything roglv2's C API passes; the accessors here take one, and so does
    // the cache.
    void* handle = reinterpret_cast<void*>(static_cast<intptr_t>(writeBitmap));
    mirrorCacheNoteTexture(writeBitmap, bitmapWidth(handle), bitmapHeight(handle));

    if (!fromPane0 || !s.mirrorOnly) return x;
    ++s.paneCopies;
    return x + s.paneMipWidth / 2;
}

void stereoRelease() {
    // A track change may legitimately put the dlong outside a table that has not loaded yet, so the
    // one-shot refusal notice is armed again for the new one.
    g_walkTableSaidRefused = false;

    // The mirror's bitmap may not outlive the mode change, so the cache is told to forget it
    // before anything here can look at it again.
    mirrorCacheRelease();
    releasePanes();
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

namespace {

// GPL's mirror view for this frame, as gpl_mirrorViewCompute left it. Every aim starts from
// it, so aiming once per eye in the same frame never compounds.
struct MirrorStock {
    float eye[3];
    float basis[9];
    float fov;
};

// -- EACH GLASS, FROM WHERE THE VIEWER ACTUALLY IS ----------------------------------------
//
// GPL's mirror is a rear-facing CAMERA whose picture is pasted on the glass. For a planar
// mirror that camera belongs at the viewer's eye REFLECTED through the glass plane, looking
// along the view direction reflected about the glass normal, with a field bounded by the
// glass's edges as seen from the eye:
//
//     u  = normalize(C - E)          where you are looking at the glass
//     r  = u - 2 (u.n) n             what comes back at you
//     Ev = E - 2 ((E - C).n) n       the camera
//     fov = 2 atan(half / |C - E|)   widens as you lean in, narrows as you sit back
//
// with C, n and half the glass's own centre, normal and half-size. Everything is done in the
// car's frame, where the glasses are fixed, then taken back to the world.
//
// The car frame is recovered from GPL's own mirror view for the frame. gpl_mirrorViewCompute
// builds the mirror basis from eulers (car yaw + pi, -pitch, -roll), and rotating that by pi
// about its own up axis gives back (yaw, pitch, roll): columns 0 and 1 negated. The body
// origin is the stock eye less the mount offset along that up column.
struct PaneFrame {
    float R[9];
    float origin[3];
};

void paneFrameFromStock(const MirrorStock& stock, PaneFrame& frame) {
    for (int row = 0; row < 3; ++row) {
        frame.R[row * 3 + 0] = -stock.basis[row * 3 + 0];
        frame.R[row * 3 + 1] = -stock.basis[row * 3 + 1];
        frame.R[row * 3 + 2] = stock.basis[row * 3 + 2];
    }
    for (int i = 0; i < 3; ++i)
        frame.origin[i] = stock.eye[i] - kMirrorMountHeight * frame.R[i * 3 + 2];
}

void worldToLocal(const PaneFrame& frame, const float world[3], float local[3]) {
    float rx = world[0] - frame.origin[0];
    float ry = world[1] - frame.origin[1];
    float rz = world[2] - frame.origin[2];
    for (int column = 0; column < 3; ++column)
        local[column] = frame.R[0 * 3 + column] * rx + frame.R[1 * 3 + column] * ry +
                        frame.R[2 * 3 + column] * rz;
}

// What one aim came to, for the log: the eye's distance to the glass in metres, the field in
// degrees, and the look direction in CAR axes (yaw 180 = straight astern, pitch up positive).
struct PaneAim {
    float distance = 0.0f;
    float fov = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

// Aims p[43..83] as the reflection in one glass of `eye` (world), and sets that pane camera's
// aspect so the glass's vertical extent gets its own angle. `side` 0 is the +Y glass, 1 the
// -Y glass. Starts from the stock view every time, so aims never compound.
void aimPane(SceneParams* params, const MirrorStock& stock, const PaneFrame& frame,
             const PaneGeometry& glass, int side, const float eye[3], void* camera,
             PaneAim& aim) {
    std::memcpy(params->mirrorEye, stock.eye, sizeof(stock.eye));
    std::memcpy(params->mirrorOrientation, stock.basis, sizeof(stock.basis));
    params->mirrorFov = stock.fov;
    aim = PaneAim{};

    float sign = side == 0 ? 1.0f : -1.0f;
    float c[3] = {glass.centre[0], sign * glass.centre[1], glass.centre[2]};
    float n0[3] = {glass.normal[0], sign * glass.normal[1], glass.normal[2]};

    // The driver's adjustment of the glass on its mount. Yaw turns the normal about the car's
    // up axis so its sideways component points at the centreline -- toward -Y for the +Y glass,
    // +Y for the -Y glass, hence the sign -- and pitch then turns it about the car's sideways
    // axis so a positive angle lifts the face.
    float yaw = sign * g_mirrorAim.yaw * 0.0174532925f;
    float pitch = g_mirrorAim.pitch * 0.0174532925f;
    float cy = std::cos(yaw), sy = std::sin(yaw);
    float cp = std::cos(pitch), sp = std::sin(pitch);
    float yx = n0[0] * cy - n0[1] * sy;
    float yy = n0[0] * sy + n0[1] * cy;
    float n[3] = {yx * cp - n0[2] * sp, yy, -yx * sp + n0[2] * cp};
    float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int i = 0; i < 3; ++i) n[i] /= length;

    float e[3];
    worldToLocal(frame, eye, e);

    float t[3] = {c[0] - e[0], c[1] - e[1], c[2] - e[2]};
    float distance = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (distance < 1e-4f) return;

    // The camera: the eye reflected through the glass plane.
    float along = -(t[0] * n[0] + t[1] * n[1] + t[2] * n[2]);  // (E - C).n
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    localToWorld(frame.R, e[0] - 2.0f * along * n[0], e[1] - 2.0f * along * n[1],
                 e[2] - 2.0f * along * n[2], wx, wy, wz);
    params->mirrorEye[0] = frame.origin[0] + wx;
    params->mirrorEye[1] = frame.origin[1] + wy;
    params->mirrorEye[2] = frame.origin[2] + wz;

    // The direction: the view of the glass reflected about its normal. Yaw and pitch come
    // from it through gpl_viewRecordSetEuler's own convention; roll is the stock mirror's,
    // which is the car's attitude and not ours to change.
    float u[3] = {t[0] / distance, t[1] / distance, t[2] / distance};
    float un = u[0] * n[0] + u[1] * n[1] + u[2] * n[2];
    float r[3] = {u[0] - 2.0f * un * n[0], u[1] - 2.0f * un * n[1], u[2] - 2.0f * un * n[2]};
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    localToWorld(frame.R, r[0], r[1], r[2], rx, ry, rz);
    if (rz > 1.0f) rz = 1.0f;
    if (rz < -1.0f) rz = -1.0f;

    float stockYaw = 0.0f, stockPitch = 0.0f, stockRoll = 0.0f;
    gplMatrixToEuler(stock.basis, stockYaw, stockPitch, stockRoll);
    gplEulerToMatrix(std::atan2(ry, rx), -std::asin(rz), stockRoll,
                     params->mirrorOrientation);

    // The field: the glass's angular size from the eye. The pane camera's half extents are
    // the texture half's, and the glass maps that half onto its whole face, so the aspect
    // makes the vertical angle the glass's own: tan(v/2) = tan(h/2) * aspect * height / width.
    float span = distance < kMirrorMinDistance ? kMirrorMinDistance : distance;
    float fov = 2.0f * std::atan(glass.halfWidth / span) * g_mirrorAim.fovScale;
    float fovV = 2.0f * std::atan(glass.halfHeight / span) * g_mirrorAim.fovScale;
    if (fov < kMirrorMinFov) fov = kMirrorMinFov;
    if (fov > kMirrorMaxFov) fov = kMirrorMaxFov;
    if (fovV < kMirrorMinFov) fovV = kMirrorMinFov;
    if (fovV > kMirrorMaxFov) fovV = kMirrorMaxFov;
    params->mirrorFov = fov;

    float halfW = cameraFloat(camera, kCameraHalfWExtent);
    float halfH = cameraFloat(camera, kCameraHalfHExtent);
    if (halfW > 0.0f && halfH > 0.0f) {
        auto bytes = static_cast<unsigned char*>(camera);
        float aspect = std::tan(fovV * 0.5f) / std::tan(fov * 0.5f) * halfW / halfH;
        *reinterpret_cast<float*>(bytes + kCameraAspect) = aspect;
        bytes[kCameraDirty] = 1;
    }

    float rzLocal = r[2] > 1.0f ? 1.0f : (r[2] < -1.0f ? -1.0f : r[2]);
    aim.distance = distance;
    aim.fov = fov * 57.2957795f;
    aim.yaw = std::atan2(r[1], r[0]) * 57.2957795f;
    aim.pitch = std::asin(rzLocal) * 57.2957795f;
}

// When the team changes, or the first time: the glass data in use and whether the derived
// car frame is believable. The seat eye in car axes is the check report 41 T1 asks for.
void reportPaneGeometry(const PaneGeometry& glass, const PaneFrame& frame,
                        const MirrorStock& stock, const float viewOrientation[9],
                        const float seat[3]) {
    float seatLocal[3];
    worldToLocal(frame, seat, seatLocal);
    float ox = stock.eye[0] - frame.origin[0];
    float oy = stock.eye[1] - frame.origin[1];
    float oz = stock.eye[2] - frame.origin[2];
    float forward = frame.R[0] * viewOrientation[0] + frame.R[3] * viewOrientation[3] +
                    frame.R[6] * viewOrientation[6];
    float up = frame.R[2] * viewOrientation[2] + frame.R[5] * viewOrientation[5] +
               frame.R[8] * viewOrientation[8];
    forward = forward > 1.0f ? 1.0f : (forward < -1.0f ? -1.0f : forward);
    up = up > 1.0f ? 1.0f : (up < -1.0f ? -1.0f : up);
    logf("mirror: %s glasses (driver %d%s) -- +Y glass centre %+.3f %+.3f %+.3f, normal "
         "%+.3f %+.3f %+.3f, half-size %.4f x %.4f m; the -Y glass is its Y mirror. Car "
         "origin = stock mirror eye less %.2f m along the car's up axis (%.3f m from it). "
         "The seat eye is at %+.3f %+.3f %+.3f in car axes (expect roughly 0..0.1, 0, "
         "0.55..0.65). The car frame's forward and up axes are %.1f and %.1f deg from the "
         "cockpit view's, which carry the head lean.",
         glass.team, g_state.paneDriver,
         g_state.paneTeamFallback ? ", FALLBACK" : "", glass.centre[0], glass.centre[1],
         glass.centre[2], glass.normal[0], glass.normal[1], glass.normal[2],
         glass.halfWidth, glass.halfHeight, kMirrorMountHeight,
         std::sqrt(ox * ox + oy * oy + oz * oz), seatLocal[0], seatLocal[1], seatLocal[2],
         std::acos(forward) * 57.2957795f, std::acos(up) * 57.2957795f);
}

// Both glasses' aims from one viewpoint, at most every two seconds and only after the
// viewpoint has moved 2 cm in the car.
void reportPaneAim(const PaneFrame& frame, const float eye[3], const PaneAim aim[2],
                   const char* viewpoint) {
    State& s = g_state;
    float local[3];
    worldToLocal(frame, eye, local);

    static LARGE_INTEGER frequency = {};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    float moved = std::fabs(local[0] - s.paneAimReportEye[0]) +
                  std::fabs(local[1] - s.paneAimReportEye[1]) +
                  std::fabs(local[2] - s.paneAimReportEye[2]);
    if (s.paneAimReportQpc &&
        (now.QuadPart - s.paneAimReportQpc < 2 * frequency.QuadPart || moved < 0.02f))
        return;
    s.paneAimReportQpc = now.QuadPart;
    std::memcpy(s.paneAimReportEye, local, sizeof(local));

    logf("mirror: from the %s at %+.3f %+.3f %+.3f in car axes -- left glass %.3f m away, "
         "fov %.1f deg, looking yaw %+.1f pitch %+.1f | right glass %.3f m, fov %.1f, yaw "
         "%+.1f pitch %+.1f (car axes, yaw +/-180 = astern)",
         viewpoint, local[0], local[1], local[2], aim[0].distance, aim[0].fov, aim[0].yaw,
         aim[0].pitch, aim[1].distance, aim[1].fov, aim[1].yaw, aim[1].pitch);
}

// WHICH GLASS PUTS THE WHITE IN THE SKY. A temporary bisect, not a setting.
//
// The two glasses are rendered by different paths, which is worth knowing here: glass 0 (+Y) is
// a whole extra gpl_sceneRender of its own with mirrorOnly set, and glass 1 (-Y) rides the eye's
// own gpl_sceneRender as params->mirrorCamera.
//
// Both were rendered alone in turn to find where the white mirror sky above ~13 degrees of glass
// pitch comes from, and both showed it, so it is common to the two paths and not in either.

}  // namespace

// The mirror on one frame in mirrorUpdateRate, counting from the frame that latched the tick.
//
// A frame drawn between two sim ticks is one of GPL's own and redraws everything, the mirror
// included -- measured at frameRate = 2, 231 mirror-only renders a second against a 36 Hz
// simulation, which doubles the cost of every frame and so halves the rate. What a rear-view
// mirror shows changes with the tick and with the head, and neither needs every frame.
//
// With perEyeMirror on, each eye's pass renders the panes from its own eye into the SAME mirror
// texture, so the image in it at the end of a frame is the right eye's, and a frame that skipped
// the render left both eyes sampling that one -- the left eye alternating between its own
// reflection and the right's, which is a visible flicker and was reported as one. MirrorCache
// gives each eye a copy and puts it back on the frames that do not render; until both copies
// exist there is nothing to put back, so the frame renders.
// Whether the glasses show anything at all. One switch now: mirrorUpdateRate = 0 is off, and
// it comes out as a dark glass rather than a frozen one. perEyeMirror used to be a second way
// to say no -- it was 0/1/2 with 0 meaning off -- and having two was the reason to fold it
// down to what it is actually about, which is whether each eye gets its own reflection.
bool stereoMirrorOn() { return g_options.mirrorUpdateRate > 0; }

bool stereoMirrorDueThisFrame() {
    if (!stereoMirrorOn()) return false;
    if (g_options.mirrorUpdateRate <= 1) return true;
    if (g_options.perEyeMirror && !mirrorCacheReady()) return true;
    return (flatFramesTickFrameIndex() % g_options.mirrorUpdateRate) == 0;
}

bool stereoRender(SceneParams* params, void(__cdecl* original)(void*)) {
    if (!stereoAvailable() || !params || !params->camera) return false;
    if (!ensureEyes(params->camera)) return false;

    void* const savedCamera = params->camera;
    void* const savedMirror = params->mirrorCamera;

    // TURNING THE MIRROR OFF HAS TO WIPE IT. Nothing renders it once it is off, but the car
    // still samples mirror.mip, so the glasses keep whatever reflection was last written into
    // them -- a frozen image that reads as a fault rather than an off switch. Done here because
    // this is a place with the game's GL context current and the setting can change at any time
    // from the menu.
    static int lastMirrorOn = -1;
    int        mirrorOn = stereoMirrorOn() ? 1 : 0;
    if (mirrorOn != lastMirrorOn) {
        if (lastMirrorOn == 1)
            logf("stereo: the car mirror is off -- mirror.mip %s",
                 mirrorCacheBlackout()
                     ? "filled with black, so the glasses are dark rather than frozen"
                     : "could NOT be cleared, so the glasses keep their last reflection");
        lastMirrorOn = mirrorOn;
    }

    void* const mirrorWanted = stereoMirrorDueThisFrame() ? savedMirror : nullptr;

    // Whether each eye's reflection is worth keeping. Only mode 2 gives the two eyes different
    // images, and only a mirrorUpdateRate above 1 ever skips a render -- with neither, the
    // copy would be made every frame and never read.
    //
    // Deliberately not conditional on there being a mirror camera this frame. A frame with no
    // camera is one that will not render the mirror and still draws a car that samples it --
    // an extra render through the present hook is exactly that -- so it wants the restore for
    // the same reason a skipped frame does. With nothing saved yet the restore costs a compare.
    bool cacheEyes = g_options.perEyeMirror && g_options.mirrorUpdateRate > 1;
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

    // The mirror is aimed inside the pass loop, from the stock view saved here and the car's
    // orientation before the horizon lock levels it. See aimPane.
    MirrorStock mirrorStock;
    std::memcpy(mirrorStock.eye, params->mirrorEye, sizeof(mirrorStock.eye));
    std::memcpy(mirrorStock.basis, params->mirrorOrientation, sizeof(mirrorStock.basis));
    mirrorStock.fov = params->mirrorFov;
    float carOrientation[9];
    std::memcpy(carOrientation, savedOrientation, sizeof(carOrientation));
    bool mirrorMoved = false;

    // The mirror panes, when every piece they depend on is in place. Otherwise the mirror is
    // GPL's own single image, unaimed, and the reason is logged when it changes.
    bool                panes = false;
    PaneFrame           paneFrame = {};
    const PaneGeometry* glass = nullptr;
    if (savedMirror && stereoMirrorOn()) {
        if (!(params->mirrorFlags & 1u)) {
            reportPanes("GPL renders the mirror straight into mirror.mip, so there are no "
                        "halves to fill -- one stock image");
        } else if (!hooksMainWalkRedirected()) {
            reportPanes("the main world walk call is not redirected -- one stock image");
        } else if (!hudBitmapDrawHooked()) {
            reportPanes("the bitmapDraw thunk is not installed, so the left glass's copy "
                        "cannot be moved -- one stock image");
        } else if (!ensurePolyGate()) {
            reportPanes("the polyDraw gate could not be installed -- one stock image");
        } else if (!ensurePanes(savedMirror)) {
            reportPanes("the pane cameras are not built -- one stock image");
        } else {
            panes = true;
            reportPanes("each glass renders its own reflection");
            paneFrameFromStock(mirrorStock, paneFrame);
            bool teamChanged = false;
            glass = &viewedCarGlass(teamChanged);
            if (teamChanged || !g_state.paneGeometryReported) {
                g_state.paneGeometryReported = true;
                float seat[3] = {savedX, savedY, savedZ};
                reportPaneGeometry(*glass, paneFrame, mirrorStock, carOrientation, seat);
            }
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

    g_state.frameMirror = savedMirror;
    g_state.frameMirrorSurface =
        savedMirror ? cameraPointer(savedMirror, kCameraSurface) : nullptr;

    // The one point both eye passes ask the segment search about. Built from the same offsets
    // the passes use and from savedOrientation as the horizon lock has just left it, so it is
    // the midpoint of the two eye positions written below.
    float centreX = 0.0f, centreY = 0.0f, centreZ = 0.0f;
    localToWorld(savedOrientation, (vr[0].offset[0] + vr[1].offset[0]) * 0.5f,
                 (vr[0].offset[1] + vr[1].offset[1]) * 0.5f,
                 (vr[0].offset[2] + vr[1].offset[2]) * 0.5f, centreX, centreY, centreZ);
    walkSetCentre(savedX + centreX, savedY + centreY, savedZ + centreZ);

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

        // WHICH PART OF THE TRACK THIS PASS WILL WALK.
        //
        // gpl_sceneRender picks the segment range from the eye POSITION, not from the view
        // transform: sub_460B40(hint, x, y, z) at 0x487D73, with the position we have just
        // written. A frame whose selection lands somewhere else draws that part of the track
        // instead of this one -- distant terrain over everything, no road, the ground plane
        // showing through -- while the transform, built from the orientation, still looks
        // right. That is the artifact as the user describes it, so this records the segment
        // each pass is about to walk and reports jumps.
        //
        // The redirect goes in here too, on the render thread between passes, because the sites
        // it patches are inside the function this call is about to enter.
        installWalkLookup();
        walkPassBegin(pass);

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

        // The mirrors. A pass that renders them renders BOTH glasses from its viewpoint: the
        // +Y glass in a mirror-only sceneRender first, copied into the right half of
        // mirror.mip, then the -Y glass in the eye's own render, copied into the left half
        // just before that render draws the car whose glasses sample both halves. 1 does this
        // on the left eye's pass only, from the head centre, and the right eye samples what
        // it left; 2 does it on both passes, each from its own eye. Without panes the same
        // passes render GPL's single stock image.
        bool mirrorThisPass = mirrorWanted && (g_options.perEyeMirror || pass == 0);
        params->mirrorCamera = mirrorThisPass ? savedMirror : nullptr;
        if (mirrorThisPass && panes) {
            float eye[3] = {params->eyeX, params->eyeY, params->eyeZ};
            if (!g_options.perEyeMirror) {
                // vr[i].offset is each eye's own displacement from the recentre pose, in GPL
                // local axes, so their midpoint is the head's.
                float hx = 0.0f, hy = 0.0f, hz = 0.0f;
                localToWorld(savedOrientation, (vr[0].offset[0] + vr[1].offset[0]) * 0.5f,
                             (vr[0].offset[1] + vr[1].offset[1]) * 0.5f,
                             (vr[0].offset[2] + vr[1].offset[2]) * 0.5f, hx, hy, hz);
                eye[0] = savedX + hx;
                eye[1] = savedY + hy;
                eye[2] = savedZ + hz;
            }

            PaneAim aim[2];
            aimPane(params, mirrorStock, paneFrame, *glass, 0, eye, g_state.pane[0].camera,
                    aim[0]);
            params->mirrorCamera = g_state.pane[0].camera;

            int copies = g_state.paneCopies;
            g_state.mirrorOnly = true;
            original(params);
            g_state.mirrorOnly = false;
            g_paneDropPolys = 0;
            if (g_state.paneCopies == copies && !g_state.reportedNoCopy) {
                g_state.reportedNoCopy = true;
                logf("mirror: the left glass's copy-out did not pass through the bitmapDraw "
                     "thunk, so it landed in the right glass's half of mirror.mip");
            }

            aimPane(params, mirrorStock, paneFrame, *glass, 1, eye, g_state.pane[1].camera,
                    aim[1]);
            params->mirrorCamera = g_state.pane[1].camera;
            mirrorMoved = true;

            if (pass == 0)
                reportPaneAim(paneFrame, eye, aim,
                              !g_options.perEyeMirror ? "head centre" : "left eye");
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

        // This pass is not rendering the mirror, so mirror.mip still holds whichever eye wrote
        // it last. Put this eye's own reflection back before the render that samples it.
        //
        // FLUSH FIRST. roglv2 does not rasterise when polyDraw is called -- it appends to the
        // ZBuffer drawer's bins and issues them at a destination change or a 2D call. The eye
        // before this one can still have the car queued, and that car samples mirror.mip, so
        // replacing the texture underneath it hands it the other eye's reflection: the glasses
        // read as swapped, and intermittently, because it depends on what is still in the bin.
        // GPL's own copy-out has exactly this hazard and bitmapDraw flushes on its way in,
        // which is why the stock mirror is never stale. This blit is not a bitmapDraw.
        if (cacheEyes && !mirrorThisPass) {
            rasterFlushPending();
            mirrorCacheRestore(pass);
        }

        original(params);

        // Both halves of mirror.mip are this eye's only now: the +Y glass was copied in before
        // the render and the -Y glass during it, just ahead of the car that samples them.
        if (cacheEyes && mirrorThisPass) mirrorCacheSave(pass);

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

    walkFrameEnd();
    g_state.frameMirrorSurface = nullptr;

    params->camera = savedCamera;
    params->mirrorCamera = savedMirror;
    if (mirrorMoved) {
        std::memcpy(params->mirrorEye, mirrorStock.eye, sizeof(mirrorStock.eye));
        std::memcpy(params->mirrorOrientation, mirrorStock.basis, sizeof(mirrorStock.basis));
        params->mirrorFov = mirrorStock.fov;
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
    frameBudgetBindBegin();
    if (g_entries.cameraBind) g_entries.cameraBind(savedCamera);
    frameBudgetBindDone();

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
