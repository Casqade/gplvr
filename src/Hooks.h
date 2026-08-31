#pragma once

#include "Discovery.h"

#include <windows.h>

namespace gplvr {

struct Options {
    // Render each eye into one half of the back buffer: two cameras aimed at
    // bitmapInstanceAlloc sub-rects, eyes half an IPD either side of the head.
    bool stereo = false;

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

    // Stop the rasterizer forcing its window above everything else.
    //
    // roglv2's WindowManager::SetSize calls SetWindowPos(hwnd, HWND_TOPMOST, ...) on
    // every mode change; that is the only source, and demoting it there is enough.
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

    // Drive the camera with the MOUSE, and stop head tracking driving it.
    //
    // The point is to be able to judge the distortion on a flat monitor. In the headset
    // every candidate explanation has now been ruled out by measurement and the picture
    // is still wrong, and the desktop mirror cannot settle it -- the preview is
    // letterboxed and horizontally shrunk, so it is already distorted before you start
    // and there is no reasoning about what is left on top of that.
    //
    // With vr = 0 this is the whole mechanism: no presenter, no runtime, no streaming
    // layer, no optics. Just GPL's own renderer being asked to look in a direction, on a
    // monitor, where a straight line is straight. If the shear and the vertical stretch
    // appear here too they are GPL's or ours and the headset was never involved; if the
    // picture stays clean however far you turn, everything above the runtime is
    // exonerated and the fault is below it.
    //
    // With vr = 1 it INHIBITS head tracking and steers the same override instead, so the
    // two can be compared at identical angles rather than from memory.
    bool vrMouseLook = false;

    // Degrees of view per raw mouse count. Device counts, not pixels -- no acceleration
    // and no Windows pointer speed in the way, which is what makes a sweep repeatable.
    // A 1600 DPI mouse moving 10 cm is about 6300 counts, so the default sweeps a little
    // over half a turn across a mousepad.
    float vrMouseLookSensitivity = 0.08f;

    // Mouse forward looks up by default, as a head does.
    bool vrMouseLookInvertPitch = false;

    // Toggle it in game. '6' by default, since 1-5 are taken above and the in-game
    // cameras have the function keys; 0 disables the toggle.
    int vrMouseLookKey = '6';

    // HORIZON LOCK, 0 to 1. How much of the car's pitch is taken out of the view.
    //
    // 0 -- the view pitches with the car, which is what a driver's head actually does
    //      and the physically correct answer.
    // 1 -- the view stays level with the world horizon while the car pitches under it.
    // between -- blended, which is what most people who want this actually want.
    //
    // This is the setting iRacing, ACC and AMS2 all ship, and they expose it as a
    // percentage for the same reason: enough of it to take the edge off kerbs and
    // braking, not so much that the car slides around underneath you.
    //
    // It blends the car's ROLL and its PITCH, roll first. Roll is the half that
    // matters: a viewer can counteract a pitched camera by tilting their own head and
    // nobody can counteract roll, which is why Meta's VR guidance is blunt about never
    // rotating the camera in roll, and why every sim horizon lock aims at banking,
    // kerbs and body lean before it worries about braking dive.
    //
    // The pitch it removes is the CAR'S ATTITUDE over terrain -- measured at -5 to +3
    // degrees, drifting continuously through zero. GPL's cockpit FRAMING, the thing that
    // used to make the view aim at the wheel, was the principal point, and that is fixed
    // unconditionally at bind time now. These are different quantities; do not confuse
    // this setting with that fix.
    float vrHorizonLock = 0.0f;

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

    // Extra whole-scene renders per game frame, 0 to 3.
    //
    // The 36 Hz is the sim TASK RATE, not a rendering limit: sceneRender costs about a
    // millisecond an eye out of an 11 ms budget at 90 Hz, and the game thread spends
    // the rest of each tick idle. Rendering the scene again from a fresh head pose
    // produces a real head-tracked frame rather than a reprojected one -- the world
    // still animates at 36 Hz, but head motion, which is what causes discomfort, gets
    // smooth.
    //
    // 1 gives 72 Hz of head tracking, 2 gives 108. Off by default because it spends
    // idle time that the game may want back on a slower machine, and because it is new.
    int vrExtraRenders = 0;

    // How much of the eye a menu covers, 0.05 to 1.0. Menus are flat 2D images with no
    // world render behind them, so at 1.0 they fill the whole 104-degree view a few
    // centimetres from your face. The layer sits at infinity either way -- angular size
    // is the only "distance" a projection layer has.
    float vrMenuScale = 0.5f;

    // Multiply the runtime's RECOMMENDED per-eye render size.
    //
    // 1.0 keeps the recommendation, which for a Pico 4 is around 3172x3172. Below 1.0
    // trades resolution the source does not have for encode time and link bandwidth it
    // does -- GPL hands us a 1280x1440 half, so at 1.0 we are already upscaling six
    // times into pixels that carry no extra detail.
    //
    // There is no ceiling of 1.0. xrEnumerateViewConfigurationViews reports the
    // recommended size and the MAXIMUM side by side -- 3172 against 8192 here -- and the
    // presenter clamps to that maximum, which is the only figure that is a real limit.
    // Above 1.0 buys little while the source is a game half, but the ceiling should not
    // be an invented number, and it should not be the thing in the way once the source
    // resolution rises.
    float vrRenderScale = 1.0f;

    // Render the whole frame into an off-screen buffer sized by vrRenderScale instead of
    // by the game window.
    //
    // This is what makes vrRenderScale mean anything above 1.0. With it off the eyes are
    // halves of roglv2's back buffer, so their size is a Windows display mode and every
    // pixel above 1280x1440 is invented by the presenter's upscale. With it on the game
    // rasterizes at the headset's own resolution and the flat window becomes a scaled
    // monitor of the result.
    //
    // OFF by default for its first release, and it should not stay an option: the two
    // things to look at first are whether the menus, which are laid out in surface
    // pixels, still place their cursor correctly, and whether GPL's own HUD follows the
    // larger surface. Both are visible in one session. Once they are answered this
    // becomes the behaviour and the switch goes -- see AGENTS.md on options.
    bool vrOffscreen = false;

    // What the flat window shows while VR is on: 0 both eyes, 1 the left eye, 2 the
    // right. Only meaningful with vrOffscreen on, because without an off-screen frame
    // the window IS the render and there is nothing to choose.
    int vrMonitorEye = 0;


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
