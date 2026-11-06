#include "Stereo.h"

#include "DataRebase.h"
#include "FlatFrames.h"
#include "FrameBudget.h"
#include "FrameTrace.h"
#include "HeadAudio.h"
#include "Hooks.h"
#include "Hud.h"
#include "Interpolation.h"
#include "Log.h"
#include "MenuScale.h"
#include "MirrorCache.h"
#include "MirrorConfig.h"
#include "MirrorGeometry.h"
#include "MirrorShared.h"
#include "RenderProfile.h"
#include "MirrorUv.h"
#include "Pe.h"
#include "RenderTarget.h"
#include "SegmentSearch.h"
#include "StereoEuler.h"
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

namespace {

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

    // THE MIRROR PANES: ONE CAMERA PER SURFACE, each on an instance of the source surface the
    // size of the rect that surface's reflection is copied into.
    //
    // One per derived glass, each in the region of mirror.mip MirrorUv gave it. A car it refused
    // has two, both mipWidth/2 x mipHeight -- [0] the +Y (left) glass copied to x = mipWidth/2,
    // [1] the -Y (right) glass copied to x = 0.
    //
    // Rebuilt with the eye targets, since rasterizer handles are recycled across mode changes.
    Half              pane[kMaxMirrorSurfaces];
    MirrorTextureRect paneRect[kMaxMirrorSurfaces];
    int               paneCount = 0;
    void* paneSource = nullptr;
    int   paneMipWidth = 0;
    int   paneMipHeight = 0;
    bool  panesGaveUp = false;
    int   paneBuildReports = 8;
    alignas(16) unsigned char paneStorage[kMaxMirrorSurfaces][512] = {};

    // A shared-pane mirrorMode in force this frame: one pane over the whole of mirror.mip, laid
    // out per eye by MirrorShared, every glass's UVs on its part of it.
    bool sharedView = false;

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
    // Report 105 section 4.45: panes rendered, and panes not rendered because no eye that samples
    // them could see their glass (kMirrorSkipOutOfView), since stereoTakePaneCounts.
    unsigned panesRendered = 0;
    unsigned panesOutOfView = 0;

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
    // while the driver was still in the menu, and vrGplMenuScale stopped applying because
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
                     viewport ? "a MENU viewport (mono, vrGplMenuScale applies)"
                              : "a WORLD view (split into two eyes)");
            }
            if (viewport) {
                // GPL'S VIEW HAS LEFT THE SURFACE THE EYES WERE CARVED FROM, so the handles
                // describing it are on borrowed time. The replay screen draws its 3D view on a
                // 2560x1140 instance between its bands, GPL frees the race's frame surface on
                // the way back, and the handle came back as the mirror camera's 512x512 surface
                // -- which matched mirror.mip's size, so the mirror copy-out was split into the
                // dead eyes and roglv2 faulted reading a framebuffer as memory (crash
                // gplc67.exe.18520.dmp, 2026-10-18). Forgetting touches nothing of roglv2's; the
                // next world frame carves new eyes from the surface it actually renders to.
                if (g_state.sourceSurface && source != g_state.sourceSurface)
                    stereoForgetSurfaces("GPL's 3D view is a viewport on another surface");
                return false;
            }
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
    char why[96];
    sprintf_s(why, "the eye targets were rebuilt from a %dx%d surface", width, height);
    frameTraceArm(12, why);
    return true;
}

void releasePanes() {
    for (int i = 0; i < kMaxMirrorSurfaces; ++i) {
        releaseHalf(g_state.pane[i]);
        g_state.paneRect[i] = MirrorTextureRect{};
    }
    g_state.paneCount = 0;
    g_state.paneSource = nullptr;
    g_state.paneMipWidth = 0;
    g_state.paneMipHeight = 0;
}

void reportPanes(const char* reason) {
    if (g_state.paneReason == reason) return;
    g_state.paneReason = reason;
    logf("mirror: %s", reason);
}

// Where each pane's reflection lands in mirror.mip. Every glass is given a disjoint region of it
// by MirrorUv, which moved that glass's own UVs there; a car it refused falls back to the two
// halves, which is the layout every stock glass was authored for.
int planePanes(int mipWidth, int mipHeight, MirrorTextureRect rect[kMaxMirrorSurfaces]) {
    int regions = mirrorUvCount();
    if (g_state.sharedView && regions > 0) {
        rect[0] = MirrorTextureRect{0, 0, mipWidth, mipHeight};
        return 1;
    }
    if (regions > 0) {
        bool usable = true;
        for (int i = 0; i < regions && usable; ++i) usable = mirrorUvRect(i, rect[i]);
        if (usable) return regions;
    }

    // [0] is the +Y (left) glass in the right half, [1] the -Y (right) glass at x = 0.
    int half = mipWidth / 2;
    for (int i = 0; i < 2; ++i) {
        rect[i].x = i == 0 ? half : 0;
        rect[i].y = 0;
        rect[i].w = half;
        rect[i].h = mipHeight;
    }
    return 2;
}

void publishPaneGlasses();

// Builds the pane cameras from the live mirror camera, whose surface is GPL's
// mipWidth x mipHeight instance for this frame. Cheap after the first frame: it only rebuilds
// when the source surface, the mirror's size, or the set of rects changes.
bool ensurePanes(void* mirrorCamera) {
    State& s = g_state;
    if (s.panesGaveUp || !s.sourceSurface) return false;

    void* mirrorSurface = cameraPointer(mirrorCamera, kCameraSurface);
    int   mipWidth = bitmapWidth(mirrorSurface);
    int   mipHeight = bitmapHeight(mirrorSurface);
    if (mipWidth < 2 || mipHeight < 1) return false;

    // mirror.mip's own size is what `RasterV2:MirrorSize` decided, so it is the size the regions
    // are laid out in -- read from the thing that setting configured rather than from the ini
    // it was written in. This also re-asserts the moved UVs.
    mirrorUvEnsure(mipWidth, mipHeight);

    MirrorTextureRect rect[kMaxMirrorSurfaces] = {};
    int               count = planePanes(mipWidth, mipHeight, rect);
    if (count < 1) return false;

    bool same = s.paneCount == count && s.paneSource == s.sourceSurface &&
                s.paneMipWidth == mipWidth && s.paneMipHeight == mipHeight;
    for (int i = 0; same && i < count; ++i)
        same = s.pane[i].camera && s.paneRect[i].x == rect[i].x &&
               s.paneRect[i].y == rect[i].y && s.paneRect[i].w == rect[i].w &&
               s.paneRect[i].h == rect[i].h;
    if (same) return true;

    releasePanes();

    auto alloc = reinterpret_cast<BitmapInstanceAllocFn>(*g_entries.instanceAllocSlot);
    auto release = reinterpret_cast<BitmapInstanceFreeFn>(*g_entries.instanceFreeSlot);
    if (!alloc || !release) return false;

    float fov = cameraFloat(mirrorCamera, kCameraFov);
    float nearZ = cameraFloat(mirrorCamera, kCameraNear);

    for (int i = 0; i < count; ++i) {
        int width = rect[i].w;
        int height = rect[i].h;
        if (width < 1 || height < 1) {
            logf("mirror: pane %d would be %dx%d, which is not a rect to render into -- the "
                 "mirror stays one stock image",
                 i, width, height);
            releasePanes();
            s.panesGaveUp = true;
            return false;
        }

        void* instance = nullptr;
        if (!alloc(&instance, s.sourceSurface, 0, 0, width, height) || !instance ||
            bitmapWidth(instance) != width || bitmapHeight(instance) != height) {
            logf("mirror: pane %d instance (%p, 0,0 %dx%d) failed or came back %dx%d -- the "
                 "mirror stays one stock image",
                 i, s.sourceSurface, width, height, bitmapWidth(instance),
                 bitmapHeight(instance));
            if (instance) release(instance);
            releasePanes();
            s.panesGaveUp = true;
            return false;
        }
        // aspect = w/h makes the horizontal and vertical angles equal
        // (tan(v/2) = tan(h/2) * aspect * height / width); aimPane refines it per glass.
        std::memset(s.paneStorage[i], 0, kCameraSize);
        void* camera = g_entries.cameraCreate(
            s.paneStorage[i], instance, fov, 0.0f, 0.0f, nearZ,
            static_cast<float>(width) / static_cast<float>(height));
        if (!camera) {
            logf("mirror: pane %d camera could not be created -- the mirror stays one stock "
                 "image",
                 i);
            release(instance);
            releasePanes();
            s.panesGaveUp = true;
            return false;
        }
        s.pane[i].surface = instance;
        s.pane[i].camera = camera;
        s.paneRect[i] = rect[i];
    }

    s.paneCount = count;
    s.paneSource = s.sourceSurface;
    s.paneMipWidth = mipWidth;
    s.paneMipHeight = mipHeight;
    publishPaneGlasses();
    if (s.paneBuildReports > 0) {
        --s.paneBuildReports;
        logf("mirror: %d pane camera%s built on surface %p -- mirror.mip is %dx%d, near %.4f",
             count, count == 1 ? "" : "s", s.sourceSurface, mipWidth, mipHeight, nearZ);
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
// about to use. Returns the segment index the walk is centred on. SegmentSearch.h holds its type
// and the checks on its answers.
constexpr uintptr_t kWalkSegmentOf = 0x00460B40;

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

// THE DEFECT THIS GUARDS, as measured in report 84.
//
// A walk trace (since removed) caught it directly: for ONE frame -- both eye passes -- the dlong this
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
// THE SEED IS NOT THE VARIABLE for this defect, measured: an earlier instrument reseated the seed
// from the blended position and reported `hint reseated 0, corrected 0` -- it never fired -- and
// the decisive case had the SAME seed (12) with two positions 0.46 m apart answering 12 and 17.
// What decides where it stops is the ORDER of the probes. (A search that runs AWAY from its seed
// past a curve's start is a different defect, and a reseat does fix that one: SegmentSearch.cpp
// askNearHint.)
//
// So the seed is left exactly as gpl_sceneRender passes it, and the ANSWER is checked instead: a
// dlong cannot advance further than the car did. When it has, the car's own dlong replaces it --
// record byte 12, the value the sim maintains, which the mark ring shows moving smoothly to a
// tenth of a metre through the very frames where this search spiked 32 m. Live authoritative
// data, not a stale value and not a guess about the search. The containment test on GPL's answer
// is SegmentSearch.cpp's placementOf.
constexpr uintptr_t kTrackLengthDlong = 0x006B3884;

int      g_walkLastDlong = 0;
float    g_walkLastPosition[3] = {};
bool     g_walkHaveLast = false;

// MODE 3: ONE ANSWER PER FRAME, AND A SELF-CONSISTENCY TEST ON GPL'S PAIR.
//
// The two things mode 2 bought were inter-eye agreement and continuity, and mode 2 paid for
// both by moving the question metres away to the car. Neither actually needs that:
//
//  - AGREEMENT IS A CACHING QUESTION. The measured defect was "each eye pass asks the segment
//    lookup about its OWN position, so at a boundary the two eyes walk different ranges" -- 6
//    one-frame disagreements in a 322 s session. Answering once a frame and handing every pass
//    the same answer removes it by construction, and the answer is then one eye's own position
//    rather than the car's: tens of millimetres off the other eye instead of metres.
//  - CONTINUITY IS WHAT THE CONSISTENCY TEST IS FOR. `gpl_segmentPlacePoint` returns 0, which
//    reads as "inside", for a segment whose type is neither 1 nor 2 -- and in that case it
//    writes NEITHER out-parameter, so gpl_cameraTrackPositionFromWorld publishes the seed as
//    the dlong. The containment test cannot see it. But the table can: a dlong that is really
//    inside `segment` must land back in `segment` when looked up, so when the table answers a
//    DIFFERENT segment, GPL's (segment, dlong) pair disagrees with itself and is not sound.
//
// The frame key is hooksFrameCount(), which advances in the present hook, between renders.
unsigned g_walkFrameKey = 0;        // which frame the cached answer belongs to
bool     g_walkFrameValid = false;
int      g_walkFrameSegment = 0;
int      g_walkFrameDlong = 0;
int      g_walkFrameDlat = 0;

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
uint32_t g_walkFrame = 0;  // stereo frames that reached the eye loop, counted at the first eye

void walkSetCentre(float x, float y, float z) {
    auto toUnits = [](float metres) {
        return static_cast<int>(static_cast<double>(metres) * kDlongUnitsPerMetre);
    };
    g_walkCentre[0] = toUnits(x);
    g_walkCentre[1] = toUnits(y);
    g_walkCentre[2] = toUnits(z);
    g_walkHaveCentre = true;
}

int walkLookupCompute(int hint, int* out, int x, int y, int z) {
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
    //
    // A runaway from the hint (askNearHint) is never sound, however its own tests come out.
    bool runaway = false;
    int  segment = askNearHint(original, hint, out, x, y, z, runaway);
    bool sound = !runaway && placementOf(segment, x, y) == 0;

    // THE SECOND TEST: is GPL's own (segment, dlong) pair consistent with itself? A dlong
    // really inside `segment` must look up to `segment`. When the table answers a different
    // one, the pair disagrees -- which is the failure `placementOf` is blind to, because
    // gpl_segmentPlacePoint returns 0 for a segment whose type is neither 1 nor 2 while writing
    // neither out-parameter, leaving the seed in out[0]. A refusal from the table (-1) proves
    // nothing and is left alone.
    if (sound) {
        int fromDlong = segmentForDlong(static_cast<float>(
            static_cast<double>(out[0]) / kDlongUnitsPerMetre));
        if (fromDlong >= 0 && fromDlong != segment) sound = false;
    }

    if (sound) {
        g_walkHaveLast = false;  // the memory is about the substituted point, not this one
        return segment;
    }

    // The eye's answer is an extrapolation from a segment it is not in -- report 84's defect.
    // Ask again about the car, whose position the sim maintains and which flat GPL would have
    // asked about anyway.
    float blended[3] = {}, tick[3] = {};
    if (!interpolationViewedCarPosition(blended, tick)) {
        // Nothing to substitute, so GPL's answer goes out as it always did.
        g_walkHaveLast = false;
        return segment;
    }

    auto toUnits = [](float metres) {
        return static_cast<int>(static_cast<double>(metres) * kDlongUnitsPerMetre);
    };

    int ux = toUnits(blended[0]);
    int uy = toUnits(blended[1]);
    int uz = toUnits(blended[2]);

    // The car is drawn a tick behind the hint as the eye is, so its search can run away the same
    // way and is asked again the same way.
    bool carRunaway = false;
    segment = askNearHint(original, hint, out, ux, uy, uz, carRunaway);

    // How far the car moved since the last accepted answer. All four calls in a frame -- two eyes,
    // and the mirror pass with them -- use the same substituted point, so within a frame this is
    // zero and the two eyes have to agree.
    float dx = blended[0] - g_walkLastPosition[0];
    float dy = blended[1] - g_walkLastPosition[1];
    float dz = blended[2] - g_walkLastPosition[2];
    float moved = std::sqrt(dx * dx + dy * dy + dz * dz);

    // THE PRIMARY TEST: did the search settle on a segment the point is actually in? A runaway
    // that no reseat placed counts as not.
    bool extrapolated = carRunaway || placementOf(segment, ux, uy) > 0;

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
    if (g_walkHaveLast && moved < kGiveUp) {
        float excess =
            std::fabs(static_cast<float>(stereoDlongDelta(g_walkLastDlong, out[0]))) /
                static_cast<float>(kDlongUnitsPerMetre) -
            moved;
        impossible = excess > kMargin;
    }

    if (extrapolated || impossible) {
        // Only out[0] is replaced. out[1], the dlat, feeds gpl_selectorCameraDlat, a
        // level-of-detail input, and out[2] is the z we passed in, echoed back. A runaway's dlat
        // is measured from a far part of the track (-634 m on monza10k), so it goes to the
        // centreline instead.
        float carDlong = 0.0f;
        if (interpolationViewedCarDlong(carDlong)) out[0] = toUnits(carDlong);
        if (carRunaway) out[1] = 0;
    }

    g_walkLastDlong = out[0];
    g_walkLastPosition[0] = blended[0];
    g_walkLastPosition[1] = blended[1];
    g_walkLastPosition[2] = blended[2];
    g_walkHaveLast = true;
    return segment;
}

// ONE ANSWER PER FRAME, so that the two eyes cannot walk different ranges. The cache is read
// before GPL's search runs, so a later pass in the same frame costs nothing at all; the dlat goes
// in it too, because half of one pass's pair and half of another's is the same disagreement by a
// quieter route.
int walkLookupBody(int hint, int* out, int x, int y, int z) {
    unsigned frame = hooksFrameCount();
    if (g_walkFrameValid && g_walkFrameKey == frame) {
        out[0] = g_walkFrameDlong;
        out[1] = g_walkFrameDlat;
        return g_walkFrameSegment;
    }

    int segment = walkLookupCompute(hint, out, x, y, z);
    g_walkFrameKey = frame;
    g_walkFrameSegment = segment;
    g_walkFrameDlong = out[0];
    g_walkFrameDlat = out[1];
    g_walkFrameValid = true;
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

// THE MIRROR PASS DOES NOT SHARE THE EYES' ANSWER.
//
// It used to call walkLookupBody, which is the one-answer-per-frame cache -- and a cache has no
// idea which pass seeded it. The mirror camera looks astern from a point metres
// from the head and is NOT head-centred (the thunk above substitutes the centre; this one passes
// the caller's own position through), so on a frame where the mirror asked first both eyes were
// centred on the mirror's answer, and on every other frame on the head's. With
// mirrorUpdateInterval = 4 that alternates one frame in four, which is a submission order that
// alternates, which is a seam artifact that flips -- measured 2026-10-07: mode 3 flipped every
// frame where mode 1 was steady, with `0 of GPL's answers disagreed` proving the consistency
// test was contributing nothing and the cache was the only difference left.
//
// The cache still does its job: making the two EYES agree, which is what it was built for.
int __cdecl walkLookupMirrorThunk(int hint, int* out, int x, int y, int z) {
    return walkLookupCompute(hint, out, x, y, z);
}

void walkPassBegin(int pass) {
    if (pass == 0) ++g_walkFrame;
    g_walkPass = pass;
}

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
//
// The derived surfaces, the two glasses the pair of panes is aimed from when they share one
// mirror.mip, and the one glass per surface they are aimed from when each has a texture.
MirrorSurfaceSet g_derivedSet;
// The team the set above was walked for, which is the REPORTED team and not the clamped one a
// car outside the 1967 table falls back to. Kept so a model reload can be noticed: it moves the
// cockpit node, and with it every polygon address MirrorUv has rewritten.
int              g_derivedTeam = -1;
// The left/right pair, for a car whose glasses were derived but could not each be given a region:
// it is then drawn with the two halves of mirror.mip, so it has exactly two panes to aim.
PaneGeometry     g_derivedGlass[2];
// One glass per derived surface, nothing synthesised: one pane each, in its own region.
PaneGeometry     g_surfaceGlass[kMaxMirrorSurfaces];
bool             g_derivedActive = false;

// Pick the surface for each side. Sorted outermost first, so the first with a positive Y is the
// left glass and the first with a negative Y the right.
void buildDerivedGlasses() {
    for (int i = 0; i < g_derivedSet.count && i < kMaxMirrorSurfaces; ++i) {
        const MirrorSurface& source = g_derivedSet.surface[i];
        PaneGeometry&        g = g_surfaceGlass[i];
        g.team = source.label;
        for (int axis = 0; axis < 3; ++axis) {
            g.centre[axis] = source.centre[axis];
            g.normal[axis] = source.normal[axis];
        }
        g.halfWidth = source.halfWidth;
        g.halfHeight = source.halfHeight;
    }

    const MirrorSurface* chosen[2] = {nullptr, nullptr};
    for (int i = 0; i < g_derivedSet.count; ++i) {
        const MirrorSurface& s = g_derivedSet.surface[i];
        int side = s.centre[1] >= 0.0f ? 0 : 1;
        if (!chosen[side]) chosen[side] = &s;
    }

    // A single-mirror car, or three with both wings on one side of the sort. The missing side
    // falls back to the Y mirror of the one there is -- the same assumption the table makes,
    // now applied to a glass that is at least in the right place.
    for (int side = 0; side < 2; ++side) {
        const MirrorSurface* source = chosen[side] ? chosen[side] : chosen[1 - side];
        if (!source) continue;
        float flip = chosen[side] ? 1.0f : -1.0f;
        PaneGeometry& g = g_derivedGlass[side];
        g.team = source->label;
        g.centre[0] = source->centre[0];
        g.centre[1] = flip * source->centre[1];
        g.centre[2] = source->centre[2];
        g.normal[0] = source->normal[0];
        g.normal[1] = flip * source->normal[1];
        g.normal[2] = source->normal[2];
        g.halfWidth = source->halfWidth;
        g.halfHeight = source->halfHeight;
    }
}

// The glass a pane is aimed from: the surface's own when each glass has a region, the left/right
// pair when the two halves are in use, the table row when nothing was derived. In table mode
// both sides get the same row and aimPane negates Y for side 1; in derived mode each pane
// already holds its own glass and nothing is negated.
//
// g_derivedGlass HAS TWO ENTRIES, so a pane past 1 must never read it: the regions path has one
// pane per surface, and a third pane once read past its end and aimed the 330P4's centre mirror
// with the left wing's geometry.
const PaneGeometry& paneGlass(const PaneGeometry& table, int pane) {
    if (pane < 0) return table;
    if (mirrorUvCount() > 0) return g_surfaceGlass[pane < kMaxMirrorSurfaces ? pane : 0];
    if (!g_derivedActive) return table;
    return g_derivedGlass[pane < 2 ? pane : 0];
}

static_assert(kMaxMirrorGlasses == kMaxMirrorSurfaces,
              "MirrorConfig keeps a name per pane, so it needs room for every surface");

// NAMES FOR THE GLASSES THE PANES ARE DRAWN FOR, which is what each glass's aim is keyed on in
// gplvr_mirrors.ini. A pane per surface takes the surface's own label, numbered from the second
// of a kind; the two shared halves are the left and right glass whichever path drew them, so a
// car aimed with one keeps its aim with the other.
void publishPaneGlasses() {
    const State& s = g_state;
    const char*  labels[kMaxMirrorGlasses] = {};
    char         names[kMaxMirrorGlasses][kMirrorGlassNameBytes] = {};
    const char*  pointers[kMaxMirrorGlasses] = {};
    int          sides[kMaxMirrorGlasses] = {};
    bool         perSurface = mirrorUvCount() > 0;
    // The shared view has one pane and still a glass, and an aim, per surface.
    int          glasses = perSurface ? mirrorUvCount() : s.paneCount;
    int          count = glasses < kMaxMirrorGlasses ? glasses : kMaxMirrorGlasses;

    for (int i = 0; i < count; ++i) {
        labels[i] = perSurface ? g_surfaceGlass[i].team : (i == 0 ? "left" : "right");
        if (!labels[i] || !labels[i][0]) labels[i] = "glass";
        int same = 1;
        for (int j = 0; j < i; ++j)
            if (std::strcmp(labels[j], labels[i]) == 0) ++same;
        if (same > 1)
            _snprintf_s(names[i], sizeof(names[i]), _TRUNCATE, "%s%d", labels[i], same);
        else
            strncpy_s(names[i], labels[i], _TRUNCATE);
        pointers[i] = names[i];
        sides[i] = std::strcmp(labels[i], "left") == 0    ? 1
                   : std::strcmp(labels[i], "right") == 0 ? -1
                                                          : 0;
    }
    mirrorConfigSetGlasses(pointers, sides, count);
}

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
    if (driver == s.paneDriver) {
        // THE SAME CAR IS NOT THE SAME MODEL. mod_teamModelLoadIfNeeded can reload a team's
        // model under a driver index that never moves, which puts a different cockpit node in
        // the team record and leaves every material node address this derived pointing at
        // memory that has been freed. One dword a frame says so, and the walk is redone.
        if (!g_derivedActive || g_derivedTeam < 0 ||
            mirrorGeometryCockpit(g_derivedTeam) == g_derivedSet.cockpit)
            return kPaneGeometry[s.paneTeam];
        logf("mirror: team %d's model was reloaded -- its cockpit node moved, so the glasses "
             "and the mirror textures are derived again",
             g_derivedTeam);
    }

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

    // THE GLASSES THE CAR ACTUALLY HAS, when the model will say.
    //
    // The team index is still what names the ini section and what the table falls back to, but
    // it is NOT what decides the geometry any more. Note this runs on the REPORTED team, not on
    // the clamped one: a car outside the 1967 table has a model of its own to read even though
    // it has no row.
    //
    // The authored UVs go back BEFORE the model is read. The derive takes each glass's UV span
    // from the polygons, and a model that stayed loaded since the last derive (a loaded replay
    // switching between cars of one team) still holds the regions written for it. Read as
    // authored, they map the real authored UVs onto themselves: a car whose regions are stacked
    // (the 1967 Ferrari, 512x256 each) then showed half of each reflection, stretched.
    mirrorUvRelease();
    g_derivedActive = false;
    g_derivedTeam = -1;
    if (team >= 0) {
        if (mirrorGeometryDerive(team, g_derivedSet)) {
            buildDerivedGlasses();
            g_derivedActive = true;
            g_derivedTeam = team;
        }
        mirrorGeometryReport(team, g_derivedSet);
    }
    // The regions follow the geometry: a car whose surfaces were not derived has nothing to lay
    // out, and the UVs of the car before it are put back either way.
    mirrorUvSetSurfaces(team, g_derivedActive ? &g_derivedSet : nullptr);
    releasePanes();
    return kPaneGeometry[s.paneTeam];
}

}  // namespace

int stereoDlongDelta(int from, int to) {
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
    if (g_state.mirrorOnly && camera) {
        bool pane = false;
        for (int i = 0; i < g_state.paneCount; ++i)
            if (camera == g_state.pane[i].camera) pane = true;
        if (!pane) g_paneDropPolys = 1;
    }

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

bool stereoSurfaceSize(void* surface, int& width, int& height) {
    width = bitmapWidth(surface);
    height = bitmapHeight(surface);
    return width > 0 && height > 0;
}

// THE RACE IS OVER, SO THE HANDLES IT LEFT ARE NOT ANSWERS ANY MORE.
//
// A rasterizer handle carries no generation. Once GPL tears a session down, roglv2 recycles the
// slot, and a handle gplvr is still holding reads as an ordinary small number that resolves to
// a freed slot -- a NULL POINTER out of a non-null handle. That is the 2026-10-09 crash
// (`roglv2!bitmapWidth+0x27`, handle 0x6F, table slots 0x6D..0x73 zeroed by the lobby-to-main-
// menu teardown) and the three before it. Nothing downstream can validate the handle, so the
// only fix is to stop handing it out.
//
// DELIBERATELY NOT stereoRelease(). That also frees cameras and panes and puts the cars'
// material nodes back, and by the time the front end is visible the model those nodes live in
// may already be gone -- restoring a pointer into freed memory to avoid a dangling mip is no
// trade at all. This touches gplvr's own State and nothing else, so it cannot fault. The heavy
// teardown still happens where it always did, on the rebuild path, which this forces by
// clearing the handles the "same source, same size" check compares against.
void stereoForgetSurfaces(const char* why) {
    State& s = g_state;
    if (!s.sourceSurface && !s.half[0].surface && !s.half[1].surface) return;
    logf("stereo: %s -- forgetting the source surface %p and the eyes %p/%p. They are roglv2 "
         "handles into a table GPL recycles, and a recycled one resolves to a null pointer or to "
         "somebody else's surface rather than to nothing.",
         why, s.sourceSurface, s.half[0].surface, s.half[1].surface);
    s.sourceSurface = nullptr;
    s.sourceWidth = 0;
    s.sourceHeight = 0;
    for (int eye = 0; eye < 2; ++eye) {
        s.half[eye].surface = nullptr;
        s.half[eye].camera = nullptr;
        for (int i = 0; i < 4; ++i) s.eyeRect[eye][i] = 0;
    }
}

bool stereoSourceCarvedSize(int& width, int& height) {
    if (!g_state.sourceSurface) return false;
    width = g_state.sourceWidth;
    height = g_state.sourceHeight;
    return width > 0 && height > 0;
}

bool stereoEyeGeometry(float& fov, float& separation) {
    if (!g_state.geometryValid) return false;
    fov = g_state.eyeFov;
    separation = g_state.eyeSeparation;
    return fov > 0.0f && separation > 0.0f;
}

bool stereoMirrorOnlyPass() { return g_state.mirrorOnly; }

int stereoMirrorPaneOf(const void* camera) {
    for (int i = 0; camera && i < g_state.paneCount; ++i)
        if (camera == g_state.pane[i].camera) return i;
    return -1;
}

bool stereoEyePass(uint32_t& frame, int& eye) {
    if (g_walkPass < 0) return false;
    frame = g_walkFrame;
    eye = g_walkPass;
    return true;
}

void stereoInstallWalkLookup() {
    // The defect is GPL's own search answering from a segment the camera is not in, and the
    // fallback is the viewed car's blended position -- so this is wanted wherever such a
    // position exists, which is VR always and flat mode whenever the blend is on. Anywhere else
    // the thunk would find no car to ask about and hand every lookup straight back, so
    // installing it would buy one containment probe per lookup and nothing at all.
    //
    // `fixEyeSegment` chose between GPL's lookup untouched (0) and three designs until
    // 2026-10-19; this is its mode 3, which cleared every artifact it was built for.
    if (g_options.vr || g_options.frameRate > 0) installWalkLookup();
}

bool stereoDroppingPolygons() { return g_paneDropPolys != 0; }

bool stereoMirrorCopy(int& writeBitmap, int readSurface, int& x, int& y) {
    State& s = g_state;
    if (!s.frameMirrorSurface) return false;

    // The surfaces a mirror copy-out can read from: any pane when the glasses render
    // separately, GPL's own frame-corner instance when they do not. Everything else going
    // through bitmapDraw in a frame -- and the HUD is most of a hundred of them -- leaves here
    // on this comparison.
    auto handleOf = [](void* pointer) {
        return static_cast<int>(reinterpret_cast<intptr_t>(pointer));
    };
    int pane = -1;
    for (int i = 0; i < s.paneCount; ++i)
        if (s.pane[i].surface && readSurface == handleOf(s.pane[i].surface)) pane = i;
    if (pane < 0 && readSurface != handleOf(s.frameMirrorSurface)) return false;

    // `writeBitmap` is where the pixels LAND. GPL always passes mirror.mip's level-0 bitmap,
    // the one thing in this process that names the texture every glass is read out of. It is a
    // handle, like everything roglv2's C API passes; the accessors here take one, and so does
    // the cache.
    void* handle = reinterpret_cast<void*>(static_cast<intptr_t>(writeBitmap));
    mirrorCacheNoteTexture(writeBitmap, bitmapWidth(handle), bitmapHeight(handle));

    if (pane < 0) return true;
    x = s.paneRect[pane].x;
    y = s.paneRect[pane].y;
    ++s.paneCopies;
    return true;
}

void stereoRelease() {
    // The mirror's bitmap may not outlive the mode change, so the cache is told to forget it
    // before anything here can look at it again. The car's authored UVs go back too, and the
    // regions are laid out again on the next frame that has a mirror.
    mirrorCacheRelease();
    mirrorUvRelease();
    releasePanes();
    releaseHalf(g_state.half[0]);
    releaseHalf(g_state.half[1]);
    for (int eye = 0; eye < 2; ++eye)
        for (int i = 0; i < 4; ++i) g_state.eyeRect[eye][i] = 0;
    g_state.sourceSurface = nullptr;
    g_state.sourceWidth = 0;
    g_state.sourceHeight = 0;
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

// Points p[43..83] and a pane camera from `position` along `direction` (both car axes) with
// fields `fov` x `fovV`, and fills `aim` for the log.
void aimCamera(SceneParams* params, const MirrorStock& stock, const PaneFrame& frame,
               const float position[3], const float direction[3], float fov, float fovV,
               float distance, void* camera, PaneAim& aim) {
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    localToWorld(frame.R, position[0], position[1], position[2], wx, wy, wz);
    params->mirrorEye[0] = frame.origin[0] + wx;
    params->mirrorEye[1] = frame.origin[1] + wy;
    params->mirrorEye[2] = frame.origin[2] + wz;

    // Yaw and pitch come from the direction through gpl_viewRecordSetEuler's own convention;
    // roll is the stock mirror's, which is the car's attitude and not ours to change.
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    localToWorld(frame.R, direction[0], direction[1], direction[2], rx, ry, rz);
    if (rz > 1.0f) rz = 1.0f;
    if (rz < -1.0f) rz = -1.0f;
    float stockYaw = 0.0f, stockPitch = 0.0f, stockRoll = 0.0f;
    gplMatrixToEuler(stock.basis, stockYaw, stockPitch, stockRoll);
    gplEulerToMatrix(std::atan2(ry, rx), -std::asin(rz), stockRoll, params->mirrorOrientation);

    // The pane camera's half extents are its region's, and the glass maps that region onto its
    // whole face, so the aspect makes the vertical angle the glass's own:
    // tan(v/2) = tan(h/2) * aspect * height / width.
    params->mirrorFov = fov;
    float halfW = cameraFloat(camera, kCameraHalfWExtent);
    float halfH = cameraFloat(camera, kCameraHalfHExtent);
    if (halfW > 0.0f && halfH > 0.0f) {
        auto bytes = static_cast<unsigned char*>(camera);
        float aspect = std::tan(fovV * 0.5f) / std::tan(fov * 0.5f) * halfW / halfH;
        *reinterpret_cast<float*>(bytes + kCameraAspect) = aspect;
        bytes[kCameraDirty] = 1;
    }

    float up = direction[2] > 1.0f ? 1.0f : (direction[2] < -1.0f ? -1.0f : direction[2]);
    aim.distance = distance;
    aim.fov = fov * 57.2957795f;
    aim.yaw = std::atan2(direction[1], direction[0]) * 57.2957795f;
    aim.pitch = std::asin(up) * 57.2957795f;
}

// One glass in car axes as the panes aim it. THE Y FLIP IS THE TABLE'S, NOT THE CAR'S:
// kPaneGeometry holds one glass and synthesises the other by negating Y, which is false for any
// car whose mirrors are not symmetric and cannot express one on the centreline at all. A derived
// glass already carries its own Y, so only the driver's yaw keeps a side.
MirrorGlass mirrorGlassOf(const PaneGeometry& glass, int side) {
    float       geomSign = g_derivedActive || side == 0 ? 1.0f : -1.0f;
    MirrorGlass out = {{glass.centre[0], geomSign * glass.centre[1], glass.centre[2]},
                       {glass.normal[0], geomSign * glass.normal[1], glass.normal[2]},
                       glass.halfWidth,
                       glass.halfHeight};
    return out;
}

// Aims p[43..83] as the reflection in one glass of `eye` (world), with the driver's aim of THIS
// glass from gplvr_mirrors.ini. `side` 0 is the +Y glass, 1 the -Y glass. Starts from the stock
// view every time, so aims never compound.
void aimPane(SceneParams* params, const MirrorStock& stock, const PaneFrame& frame,
             const PaneGeometry& glass, int side, const float eye[3], void* camera,
             PaneAim& aim) {
    std::memcpy(params->mirrorEye, stock.eye, sizeof(stock.eye));
    std::memcpy(params->mirrorOrientation, stock.basis, sizeof(stock.basis));
    params->mirrorFov = stock.fov;
    aim = PaneAim{};

    float e[3];
    worldToLocal(frame, eye, e);
    MirrorGlassView view;
    if (!mirrorGlassView(mirrorGlassOf(glass, side), mirrorAimFor(side), e, view)) return;
    aimCamera(params, stock, frame, view.camera, view.direction, view.fov, view.fovV,
              view.distance, camera, aim);
}

// A shared-pane mirrorMode for this frame (MirrorShared.h): the derived glasses with their aims,
// laid out from GPL's own eye `seat` (world) for MirrorUv's first apply. The per-glass mode, or a
// car it cannot lay out, and the panes render one per glass.
void planShared(const PaneFrame& frame, const float seat[3]) {
    State& s = g_state;
    s.sharedView = false;
    if (!mirrorSharedPane()) {
        mirrorSharedOff();
        return;
    }
    int count = g_derivedActive ? g_derivedSet.count : 0;
    if (count > kMaxMirrorSurfaces) count = kMaxMirrorSurfaces;
    MirrorGlass glasses[kMaxMirrorSurfaces];
    MirrorAim   aims[kMaxMirrorSurfaces];
    for (int i = 0; i < count; ++i) {
        glasses[i] = mirrorGlassOf(g_surfaceGlass[i], i);
        aims[i] = mirrorAimFor(i);
    }
    float e[3];
    worldToLocal(frame, seat, e);
    s.sharedView = mirrorSharedPlan(glasses, aims, count, e);
}

// The shared view's camera for pass `pass`, whose viewpoint is `eye` (world), with the glasses'
// UVs moved onto that eye's layout before the pass draws them.
void aimShared(SceneParams* params, const MirrorStock& stock, const PaneFrame& frame,
               const float eye[3], int pass, void* camera, PaneAim& aim) {
    float e[3];
    worldToLocal(frame, eye, e);
    float                     position[3];
    const MirrorSharedLayout& layout = mirrorSharedForEye(e, pass, position);
    float d[3] = {position[0] - e[0], position[1] - e[1], position[2] - e[2]};
    float distance = 0.5f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    aimCamera(params, stock, frame, position, layout.forward, layout.fov, layout.fovV, distance,
              camera, aim);
}

// The cone a pane's glass is tested against (report 105 section 4.45): the eye's frustum held in a
// cone of half-angle atan(kPaneFrustumDiagonal x tan(horizontal half field)), which covers a
// vertical extent up to 1.5x the horizontal one, widened by kPaneViewMargin for the runtime's
// reprojection and the next frame's head turn.
constexpr float kPaneFrustumDiagonal = 1.8f;
constexpr float kPaneViewMargin = 0.26f;  // 15 degrees

// Whether glass `side` can show in the view of an eye at `eye` (world) looking along the first
// column of `orientation`, with full horizontal field `fov`: the glass's bounding sphere against
// the eye's cone. As aimPane places it.
bool paneInView(const PaneFrame& frame, const PaneGeometry& glass, int side, const float eye[3],
                const float orientation[9], float fov) {
    float sign = side == 0 ? 1.0f : -1.0f;
    float geomSign = g_derivedActive ? 1.0f : sign;
    float c[3] = {glass.centre[0], geomSign * glass.centre[1], glass.centre[2]};
    float e[3];
    worldToLocal(frame, eye, e);
    float t[3] = {c[0] - e[0], c[1] - e[1], c[2] - e[2]};
    float distance = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    float radius = std::sqrt(glass.halfWidth * glass.halfWidth + glass.halfHeight * glass.halfHeight);
    if (distance <= radius) return true;
    // The view direction in car axes (frame.R's columns are the car's axes in world).
    float forward[3] = {orientation[0], orientation[3], orientation[6]};
    float f[3];
    for (int column = 0; column < 3; ++column)
        f[column] = frame.R[0 * 3 + column] * forward[0] + frame.R[1 * 3 + column] * forward[1] +
                    frame.R[2 * 3 + column] * forward[2];
    float length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (length < 1e-6f || !(fov > 0.0f)) return true;
    float cosine = (t[0] * f[0] + t[1] * f[1] + t[2] * f[2]) / (distance * length);
    cosine = cosine > 1.0f ? 1.0f : (cosine < -1.0f ? -1.0f : cosine);
    float half = std::atan(kPaneFrustumDiagonal * std::tan(fov * 0.5f));
    return std::acos(cosine) - std::asin(radius / distance) < half + kPaneViewMargin;
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

// Every glass's aim from one viewpoint, at most every two seconds and only after the
// viewpoint has moved 2 cm in the car.
void reportPaneAim(const PaneFrame& frame, const float eye[3], const PaneAim aim[], int count,
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

    logf("mirror: from the %s at %+.3f %+.3f %+.3f in car axes, %d pane%s", viewpoint, local[0],
         local[1], local[2], count, count == 1 ? "" : "s");
    for (int i = 0; i < count; ++i)
        logf("  %s glass: %.3f m away, fov %.1f deg, looking yaw %+.1f pitch %+.1f (car axes, "
             "yaw +/-180 = astern)",
             s.sharedView ? "shared" : mirrorConfigGlassName(i), aim[i].distance, aim[i].fov,
             aim[i].yaw, aim[i].pitch);
}

// The panes are rendered by two different paths -- all but the last in a mirror-only
// gpl_sceneRender of their own, the last riding the eye's as params->mirrorCamera -- and the
// white mirror sky above ~13 degrees of glass pitch was bisected across both. Each was rendered
// alone in turn and both showed it, so it is common to the two paths and in neither.

}  // namespace

// Whether the glasses show anything at all. One switch: mirrorUpdateInterval = 0 is off, and it
// comes out as a dark glass rather than a frozen one, because the car keeps sampling whatever
// was last written.
//
// A frame drawn between two sim ticks is one of GPL's own and redraws everything, the mirror
// included -- measured at frameRate = 2, 231 mirror-only renders a second against a 36 Hz
// simulation, which doubles the cost of every frame and so halves the rate. What a rear-view
// mirror shows changes with the tick and with the head, and neither needs every frame.
//
// With the mirror from each eye (mirrorMode), each eye's pass renders the panes from its own eye
// into the same textures, so what is in them at the end of a frame is the right eye's, and a
// frame that skipped the render left both eyes sampling that -- the left eye alternating between
// its own reflection and the right's, which is a visible flicker and was reported as one.
// MirrorCache gives each eye a copy and puts it back on the frames that do not render; until both
// copies exist there is nothing to put back, so the frame renders.
bool stereoMirrorOn() { return g_options.mirrorUpdateInterval > 0; }

void stereoTakePaneCounts(unsigned& rendered, unsigned& outOfView) {
    rendered = g_state.panesRendered;
    outOfView = g_state.panesOutOfView;
    g_state.panesRendered = 0;
    g_state.panesOutOfView = 0;
}

// Every glass is a region of the one shared mirror.mip, so they are all rendered on the same
// frames: the cache restores the whole texture, and restoring it after writing one region would
// put the other eye's reflection back on the region that had just been drawn.
bool stereoMirrorDueThisFrame() {
    if (!stereoMirrorOn()) return false;
    if (g_options.mirrorUpdateInterval <= 1) return true;
    if (mirrorFromEachEye() && !mirrorCacheReady()) return true;
    return (flatFramesTickFrameIndex() % g_options.mirrorUpdateInterval) == 0;
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
    // images, and only a mirrorUpdateInterval above 1 ever skips a render -- with neither, the
    // copy would be made every frame and never read.
    //
    // Deliberately not conditional on there being a mirror camera this frame. A frame with no
    // camera is one that will not render the mirror and still draws a car that samples it --
    // an extra render through the present hook is exactly that -- so it wants the restore for
    // the same reason a skipped frame does. With nothing saved yet the restore costs a compare.
    bool cacheEyes = mirrorFromEachEye() && g_options.mirrorUpdateInterval > 1;
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
    if (!tracked) {
        g_state.pendingValid[0] = g_state.pendingValid[1] = false;
        headAudioForget();
    }

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
        } else {
            // THE CAR FIRST, THE PANES AFTER IT. A change of car releases the previous car's
            // mirror textures, and a pane built before that would be holding a bitmap handle
            // belonging to a mip that has just been freed -- which the copy-out would then
            // write through. ensurePanes rebuilds whenever the rects or the destinations move,
            // so asking about the car first costs nothing and closes that window. The shared
            // view's regions go to MirrorUv before ensurePanes applies them.
            bool teamChanged = false;
            glass = &viewedCarGlass(teamChanged);
            paneFrameFromStock(mirrorStock, paneFrame);
            float seat[3] = {savedX, savedY, savedZ};
            planShared(paneFrame, seat);
            if (!ensurePanes(savedMirror)) {
                reportPanes("the pane cameras are not built -- one stock image");
                glass = nullptr;
            } else {
                panes = true;
                // MirrorUv refusing the regions leaves the two halves, which are aimed per glass.
                if (mirrorUvCount() == 0 || g_state.paneCount != 1) g_state.sharedView = false;
                reportPanes(g_state.sharedView
                                ? "every glass reads its part of one shared view (mirrorMode)"
                                : "each glass renders its own reflection");
                if (teamChanged || !g_state.paneGeometryReported) {
                    g_state.paneGeometryReported = true;
                    reportPaneGeometry(*glass, paneFrame, mirrorStock, carOrientation, seat);
                }
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

    // The head this frame is drawn from, for the sim thread's sound listener.
    headAudioPublish(carOrientation, levelled, vr);

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

    // Which glasses each eye can see (report 105 section 4.45), from the orientation and position
    // each pass below writes. A pane no eye that samples it can see is not rendered this frame;
    // its region of mirror.mip keeps an older reflection nobody looks at.
    bool paneSeen[2][kMaxMirrorSurfaces];
    for (int pass = 0; pass < 2; ++pass)
        for (int surface = 0; surface < kMaxMirrorSurfaces; ++surface)
            paneSeen[pass][surface] = true;
    if (kMirrorSkipOutOfView && tracked && panes) {
        for (int pass = 0; pass < 2; ++pass) {
            float orientation[9];
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    orientation[row * 3 + column] =
                        savedOrientation[row * 3 + 0] * vr[pass].rotation[0 * 3 + column] +
                        savedOrientation[row * 3 + 1] * vr[pass].rotation[1 * 3 + column] +
                        savedOrientation[row * 3 + 2] * vr[pass].rotation[2 * 3 + column];
            float dx = 0.0f, dy = 0.0f, dz = 0.0f;
            localToWorld(savedOrientation, vr[pass].offset[0], vr[pass].offset[1],
                         vr[pass].offset[2], dx, dy, dz);
            float eye[3] = {savedX + dx, savedY + dy, savedZ + dz};
            // The shared view's one pane is wanted while any of the glasses reading it is.
            if (g_state.sharedView) {
                bool any = false;
                for (int i = 0; i < mirrorUvCount() && !any; ++i)
                    any = paneInView(paneFrame, g_surfaceGlass[i], i, eye, orientation,
                                     vr[pass].fov);
                paneSeen[pass][0] = any;
                continue;
            }
            for (int surface = 0; surface < g_state.paneCount && surface < kMaxMirrorSurfaces;
                 ++surface)
                paneSeen[pass][surface] = paneInView(paneFrame, paneGlass(*glass, surface),
                                                     surface, eye, orientation, vr[pass].fov);
        }
    }

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

        // The mirrors. A pass that renders them renders every glass from its viewpoint: all but
        // the last in a mirror-only sceneRender of its own, copied into its region, and the last
        // riding the eye's own render so its copy lands just before that render draws the car
        // whose glasses sample it. kMirrorSharedHeadCentre does this on the left eye's pass only,
        // from the head centre, and the right eye samples what it left; the other modes do it on
        // both passes, each from its own eye. Without panes the same passes render GPL's single
        // stock image.
        //
        // N glasses cost N-1 EXTRA gated sceneRenders per pass, at about 2.1 ms each: a car with
        // one mirror saves one of the two a pair costs, and a car with three pays one more.
        bool mirrorThisPass = mirrorWanted && (mirrorFromEachEye() || pass == 0);
        params->mirrorCamera = mirrorThisPass ? savedMirror : nullptr;

        int dueCount = mirrorThisPass && panes ? g_state.paneCount : 0;

        // The viewpoint the mirror is rendered from: this eye, or the head centre when the eyes
        // share one render.
        float mirrorFrom[3] = {params->eyeX, params->eyeY, params->eyeZ};
        if (!mirrorFromEachEye()) {
            // vr[i].offset is each eye's own displacement from the recentre pose, in GPL local
            // axes, so their midpoint is the head's.
            float hx = 0.0f, hy = 0.0f, hz = 0.0f;
            localToWorld(savedOrientation, (vr[0].offset[0] + vr[1].offset[0]) * 0.5f,
                         (vr[0].offset[1] + vr[1].offset[1]) * 0.5f,
                         (vr[0].offset[2] + vr[1].offset[2]) * 0.5f, hx, hy, hz);
            mirrorFrom[0] = savedX + hx;
            mirrorFrom[1] = savedY + hy;
            mirrorFrom[2] = savedZ + hz;
        }

        bool sharedAimed = false;
        if (dueCount > 0) {
            // The panes to render: those an eye sampling this pass's writes can see
            // (kMirrorSharedHeadCentre renders here for both eyes). The last of them rides the
            // eye's render.
            int rendered[kMaxMirrorSurfaces];
            int renderCount = 0;
            for (int surface = 0; surface < dueCount; ++surface) {
                bool seen = mirrorFromEachEye()
                                ? paneSeen[pass][surface]
                                : paneSeen[0][surface] || paneSeen[1][surface];
                if (seen)
                    rendered[renderCount++] = surface;
                else
                    ++g_state.panesOutOfView;
            }
            g_state.panesRendered += static_cast<unsigned>(renderCount);
            if (renderCount == 0) params->mirrorCamera = nullptr;

            PaneAim aim[kMaxMirrorSurfaces] = {};
            for (int k = 0; k < renderCount; ++k) {
                int surface = rendered[k];
                if (g_state.sharedView) {
                    aimShared(params, mirrorStock, paneFrame, mirrorFrom, pass,
                              g_state.pane[surface].camera, aim[surface]);
                    sharedAimed = true;
                } else
                    aimPane(params, mirrorStock, paneFrame, paneGlass(*glass, surface), surface,
                            mirrorFrom, g_state.pane[surface].camera, aim[surface]);
                params->mirrorCamera = g_state.pane[surface].camera;
                if (k + 1 >= renderCount) break;

                int copies = g_state.paneCopies;
                g_state.mirrorOnly = true;
                original(params);
                g_state.mirrorOnly = false;
                g_paneDropPolys = 0;
                if (g_state.paneCopies == copies && !g_state.reportedNoCopy) {
                    g_state.reportedNoCopy = true;
                    logf("mirror: pane %d's copy-out did not pass through the bitmapDraw thunk, "
                         "so it landed wherever GPL addressed it rather than in this surface's "
                         "own rect",
                         surface);
                }
            }
            mirrorMoved = true;

            if (pass == 0)
                reportPaneAim(paneFrame, mirrorFrom, aim, dueCount,
                              !mirrorFromEachEye() ? "head centre" : "left eye");
        }
        // A pass that samples the shared view without rendering it reads the picture its own eye
        // left on an earlier frame, or at kMirrorSharedHeadCentre the left pass's.
        if (panes && g_state.sharedView && !sharedAimed)
            mirrorSharedRegionsFor(mirrorFromEachEye() ? pass : 0);

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

        // mirror.mip, when this pass is not rendering it, still holds whichever eye wrote it last.
        // Put this eye's own reflection back before the render that samples it.
        //
        // FLUSH FIRST. roglv2 does not rasterise when polyDraw is called -- it appends to the
        // ZBuffer drawer's bins and issues them at a destination change or a 2D call. The eye
        // before this one can still have the car queued, and that car samples these textures,
        // so replacing one underneath it hands it the other eye's reflection: the glasses read
        // as swapped, and intermittently, because it depends on what is still in the bin.
        // GPL's own copy-out has exactly this hazard and bitmapDraw flushes on its way in,
        // which is why the stock mirror is never stale. This blit is not a bitmapDraw.
        if (cacheEyes && !mirrorThisPass) {
            rasterFlushPending();
            mirrorCacheRestore(pass);
        }

        original(params);
        // Inside a frame-trace window: the eye's bins are flushed now rather than at the next
        // bind, so the GL viewport and scissor read are the ones its polygons were drawn with.
        if (frameTraceArmed()) {
            rasterFlushPending();
            int   eye = pass & 1;
            void* surface = stereoEyeSurface(eye);
            int   size[2] = {};
            int   rect[4] = {};
            if (surface) stereoSurfaceSize(surface, size[0], size[1]);
            stereoEyeRect(eye, rect);
            frameTraceEye(eye, surface, size[0], size[1], rect);
        }

        // mirror.mip is this eye's own now, if this pass wrote it: the panes before the last were
        // copied in ahead of the render and the last during it, just before the car that
        // samples them was drawn.
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
