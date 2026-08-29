#pragma once

#include "Discovery.h"

#include <windows.h>

namespace gplvr {

struct Options {
    // Render each eye into one half of the back buffer: two cameras aimed at
    // bitmapInstanceAlloc sub-rects, eyes half an IPD either side of the head.
    bool stereo = false;

    // Interpupillary distance. A world unit is a metre -- measured, see STATUS.md -- so
    // this is a real human IPD and needs no scaling.
    float stereoIpd = 0.064f;

    // Swap which half of the buffer gets which eye. Invisible on a flat screen and
    // immediately sickening in a headset, so it needs to be adjustable without a build.
    bool stereoSwapEyes = false;

    // The mirror pass, p[43]. Its camera targets a fixed rect of the full render
    // surface, so it does not follow an eye into its half of the buffer.
    //   0 = off in both eyes   1 = left eye only (default)   2 = both eyes
    int stereoMirror = 1;

    // Toggle stereo at runtime. '1' by default; 0 disables.
    //
    // The number row rather than a function key: pribluda and the in-game cameras have
    // F1-F12 between them, and there is no VK_ constant for a digit -- the virtual-key
    // code for '1' is simply 0x31.
    int stereoKey = '1';

    // Stop the rasterizer forcing its window above everything else. With
    // blockDisplayChange on, roglv2 still believes it is exclusive fullscreen and makes
    // its window topmost, which hides even the alt-tab switcher.
    bool allowWindowTopmost = false;

    // Make the render window one pixel short of the desktop.
    //
    // On by default, because this -- not WS_EX_TOPMOST -- turned out to be why the
    // render window sat above everything: a borderless window exactly covering the
    // screen gets handed fullscreen exclusivity by Windows, after which nothing
    // composites over it, alt-tab included. Losing one pixel is worth being able to
    // switch to another window while testing.
    bool shrinkWindowByOnePixel = true;

    // Swallow ChangeDisplaySettings[Ex] so the rasterizer stops switching the
    // desktop between the menu mode and the sim mode.
    //
    // Off by default: it works (roglv2 uses the ExW variant), but on its own it
    // leaves roglv2 with a 640x480 window sized for a mode that was never set. The
    // window fight is documented in the README -- turn this on as a separate experiment.
    bool blockDisplayChange = false;

    // Swallow ClipCursor, which is what traps the pointer inside that window.
    bool blockClipCursor = false;

    // Present in a headset. Launches gplvr-xr.exe, hands it the back buffer through a
    // shared D3D11 texture, and lets it own the OpenXR session.
    //
    // Off by default. It starts a second process and brings the runtime up, which is a
    // lot to do to someone who only wanted the flat stereo mod. Everything it needs is
    // logged, and any failure leaves the game running exactly as it would without it.
    bool vr = false;

    // Take the current head pose as the seat. '3' by default; 0 disables.
    //
    // Not optional comfort: OpenXR's LOCAL space turns out to have its origin on the
    // floor rather than at the head -- measured, see STATUS.md -- so without a recentre
    // the view sits a standing height above the driver.
    int vrRecentreKey = '3';

    // Dump the head matrix, whether it is still a proper rotation, the eye offsets and
    // the frustum written to the cameras. '4' by default; 0 disables.
    int vrDumpKey = '4';

    // Fire the one-shot fov probe: one frame with both halves drawn from the same
    // viewpoint and only the field of view differing, dumped to disk and compared
    // bitwise. See stereoRequestFovProbe.
    int vrFovProbeKey = '5';

    // Diagnostic: replace the head rotation with a FIXED one, in degrees.
    //
    // Rotation is rotation -- if GPL's view transform shears the world at a fixed 45
    // degrees of yaw, it does so whether that yaw came from a head or from here, and
    // the whole VR pipeline is off the hook. Both stay 0 for normal play; either being
    // non-zero freezes head tracking so the picture is judged on the flat monitor,
    // deterministically, with nothing moving.
    //
    // Yaw is about GPL's up axis, positive turns left. Pitch is positive looking up.
    // These are the STARTING angles; the keys below sweep from here.
    float vrDebugYawDegrees = 0.0f;
    float vrDebugPitchDegrees = 0.0f;

    // Turn the mode on with no starting offset. A still image cannot show a distortion
    // that only reads as motion -- you would be comparing two unfamiliar pictures from
    // memory -- so the angles sweep while a key is held and the shear appears the same
    // way it does when a head turns.
    bool  vrDebugRotate = false;
    float vrDebugRotateRate = 45.0f;  // degrees per second while held

    // Arrows by default, as asked. The game sees them too -- GetAsyncKeyState does not
    // consume anything -- so sweep while stationary, or rebind these, if the car
    // steering away would confuse the picture.
    int vrDebugYawLeftKey = 0x25;   // VK_LEFT
    int vrDebugYawRightKey = 0x27;  // VK_RIGHT
    int vrDebugPitchUpKey = 0x26;   // VK_UP
    int vrDebugPitchDownKey = 0x28; // VK_DOWN
    int vrDebugRotateResetKey = 0x24;  // VK_HOME, back to the starting angles

    // Hand the runtime the pose each frame was drawn from, so it can reproject 36 Hz
    // into a 90 Hz compositor. Setting this to 0 submits the pose located at submission
    // time instead, which head-locks the layer and disables reprojection entirely.
    // Costs judder; isolates whether the reprojection warp is the distortion.
    bool vrSubmitRenderedPose = true;

    // Draw a world-locked reference ring over the picture, at eye height, projected
    // with the frustum we declare to the runtime. GPL's horizon should stay locked to
    // it at every head angle. Diagnostic: it draws over the game.
    bool vrOverlayMarkers = false;

    // Level the seat before composing the head rotation onto it.
    //
    // GPL's cockpit camera carries a built-in downward pitch -- recentring aims at the
    // wheel, not the horizon. Composing the head rotation onto that pitched frame makes
    // a head YAW rotate about a tilted axis, which injects roll: none looking forward,
    // rising with yaw, peaking at 90 degrees where it equals the pitch offset. That is
    // the "ladder", and the "distortion that grows as you turn away from forward".
    //
    // With this on, the pitch is removed from the seat first, so the head yaws about
    // true vertical and supplies all the looking -- which is what a headset is for.
    // Measured and rejected: GPL's camera pitch runs -5 to +3 degrees and drifts
    // continuously through zero, which is the CAR's attitude over terrain, not a fixed
    // view offset. Levelling it deletes the car's real pitch and buys nothing, so this
    // stays off. Kept because it is one line and re-measuring beats re-deriving.
    bool vrLevelSeat = false;

    // Keep only the heading when recentring: discard the head's pitch and roll.
    //
    // A seat captured with pitch in it turns head YAW into roll -- measured at 5.2
    // degrees of roll from a level head at 89 degrees of yaw. This is the fix for that;
    // 0 restores the old behaviour for comparison.
    bool vrLevelRecentre = true;

    // GPL world units per metre.
    //
    // Every angle in this mod has now been measured and is correct, and the distortion
    // is still only visible in a headset. Scale is the one quantity that behaves that
    // way: it cannot show on a flat monitor, which has neither stereo nor head
    // translation, and it survives every test about angles because it is about
    // distance.
    //
    // It multiplies the eye separation and the head translation together, which is
    // what "world scale" means in VR. Above 1.0 the world looks SMALLER (your head
    // moves further through it, your eyes are further apart relative to it); below
    // 1.0 it looks larger. 1.0 asserts that one GPL unit is one metre, which is an
    // assumption nothing has yet verified.
    float vrWorldScale = 1.0f;

    // How much of the eye a menu covers, 0.05 to 1.0. Menus are flat 2D images with no
    // world render behind them, so at 1.0 they fill the whole 104-degree view a few
    // centimetres from your face. The layer sits at infinity either way -- angular size
    // is the only "distance" a projection layer has.
    float vrMenuScale = 0.5f;

    // Multiply the runtime's field of view before handing it to the eye cameras.
    //
    // 1.0 renders exactly what the runtime asks for, which is the geometrically correct
    // answer and should be left alone unless the picture says otherwise. Above 1.0
    // renders wider than the headset shows, which wastes pixels but gives the runtime
    // margin to reproject into on fast turns.
    float vrFovScale = 1.0f;

    // Diagnostic: the same multiplier for the right eye only, so the two halves render
    // visibly different fields of view. 0 (the default) means "use vrFovScale for both".
    // Set it to 0.5 and look at the flat monitor -- if both halves still show the same
    // extent of world, the fov we write is not reaching the renderer.
    float vrFovScaleRight = 0.0f;

    // Scale the swapchain away from the runtime's recommendation, 0.25 to 1.0.
    //
    // SteamVR recommends 3172x3172 per eye for a Pico 4. That is what a natively
    // rendered app should use -- but GPL gives us 1280x1440 per eye, so we upscale by
    // six times and then make the runtime encode and stream ten megapixels an eye of
    // pixels that carry no extra detail. On a streamed headset those pixels are real
    // bandwidth and real encoder time.
    //
    // 1.0 keeps the recommendation. Lower trades headroom the source does not have for
    // encode and transmit load it does.
    float vrEyeScale = 1.0f;

    // Run the Phase 2 transport probe: time both candidate routes out of roglv2's
    // OpenGL back buffer -- WGL_NV_DX_interop2 into a shared D3D11 texture, and plain
    // glReadPixels -- and check that the interop route really delivers the pixels.
    //
    // One shot. It measures 60 frames, writes a summary, releases everything and goes
    // quiet, so leaving it on costs one burst at startup. Off by default because it
    // pulls d3d11 into the game process.
    bool glProbe = false;

    // Install nothing -- run discovery, log what it found, and leave the game alone.
    // The first thing to try if a run misbehaves: it separates "discovery is wrong"
    // from "a hook is wrong".
    bool installHooks = true;

};

extern Options g_options;

void loadOptions(const char* fileName);

// Replaces the screenDisplay function pointer. That pointer is a plain writable
// global, so this is a single store -- no code patching involved.
bool installFrameCounter(Binding& binding);

// Queues the sceneRender call-site redirection. It is applied from inside the
// frame hook rather than here, because that runs on the game's own render thread
// at a moment when sceneRender is provably not executing -- patching a rel32 from
// a worker thread while the render thread is stepping through it would be a race.
// Requires installFrameCounter() to have succeeded.
void requestSceneRenderHook(const Binding& binding);

// Remembers our own module so the IAT patcher never redirects our own imports back
// into our own hooks. Must be called before any patching.
void setSelfModule(HMODULE module);

// Rewrites ChangeDisplaySettings* entries in one module's import table.
// Call once per loaded module; safe to call repeatedly.
void patchDisplaySettingsImports(HMODULE module);

void patchDisplaySettingsImportsEverywhere();

}  // namespace gplvr
