#pragma once

#include "Discovery.h"

#include <windows.h>

namespace gplvr {

// The scene parameter block of the last frame rendered, or null before the first one. GPL reuses
// one block, so this is the same pointer every frame; p[0] leads to the track object the segment
// lookups take.
void* hooksLastSceneParams();

// Where the HUD panel hangs in one mode, in metres: its centre ahead, below and to the
// right, and its width.
struct HudPlacement {
    float ahead;
    float down;
    float right;
    float width;
};

struct Options {
    // Render each eye into one half of the back buffer: two cameras aimed at
    // bitmapInstanceAlloc sub-rects, eyes half an IPD either side of the head.
    bool stereo = false;

    // The mirror pass, p[43]. The car's two mirror glasses sample opposite halves of
    // mirror.mip, and each half is rendered by a camera aimed as that glass's reflection.
    //   0 = no mirror
    //   1 = both panes rendered once a frame, on the left eye's pass, from the head centre;
    //       the right eye samples the same texture (2 mirror renders a frame)
    //   2 = both panes rendered on each eye's pass from that eye, so each glass has stereo
    //       depth (4 mirror renders a frame, default)
    // Each pane after the first in a pass costs a sceneRender whose main pass is skipped.
    int stereoMirror = 2;

    // vrMirrorFovScale, vrMirrorYaw and vrMirrorPitch are the defaults for every car; a car's
    // section in gplvr_cars.ini overrides each one it holds (MirrorConfig.h).
    //
    // Field of view of each mirror pane as a multiple of the angle its glass subtends from
    // the eye. 1.0 is life-size; above 1 shows more of the world, smaller. 0.25 to 6.
    float vrMirrorFovScale = 1.0f;

    // Turn each mirror glass on its mount, in degrees, the way a driver adjusts a real one.
    // Yaw positive turns the glass face inboard, toward the driver; pitch positive tilts it
    // up. Mirrored between the two glasses, applied yaw first, and the view swings by twice
    // the angle, as reflection does. -45 to 45.
    float vrMirrorYaw = 0.0f;
    float vrMirrorPitch = 0.0f;

    // Render the mirror pass into a framebuffer of its own instead of the top-left corner
    // of the frame, where a second mirror pass leaves a square in the left eye.
    bool vrMirrorOffscreen = true;

    // Mirror tweak mode, for aiming the glasses from the headset. 'M' by default; 0 disables.
    // VR only. While on, Left/Right change vrMirrorYaw, Up/Down vrMirrorPitch and Page Up/Down
    // vrMirrorFovScale, live in both glasses, and those keys do not reach the game; the key
    // again leaves and, if anything changed, writes the three values to the viewed car's
    // section of gplvr_cars.ini. gplvr.ini is never written. Entering it leaves HUD tweak
    // mode, and entering HUD tweak mode leaves it (saving the same way).
    int vrMirrorTweakKey = 'M';

    // How fast mirror tweak mode turns the glasses, in degrees per second.
    float vrMirrorTweakAngleSpeed = 10.0f;

    // How fast mirror tweak mode changes vrMirrorFovScale, in doublings per second: 0.5
    // doubles or halves it every two seconds, the same share of the field at any scale.
    float vrMirrorTweakFovSpeed = 0.5f;

    // Switch stereoMirror between 1 (head centre) and 2 (per eye) while the game runs. 'N' by
    // default; 0 disables. Does nothing when stereoMirror is 0. VR only.
    int vrMirrorModeKey = 'N';


    // Toggle stereo at runtime. '1' by default; 0 disables.
    //
    // The number row rather than a function key: pribluda and the in-game cameras have
    // F1-F12 between them, and there is no VK_ constant for a digit -- the virtual-key
    // code for '1' is simply 0x31.
    int stereoKey = '1';

    // KEEP DRAWING AND PRESENTING WHILE ANOTHER APPLICATION HAS THE FOREGROUND.
    //
    // GPL has two draw gates, roglv2's `WindowManager + 0x19` and gplc67's
    // `gpl_isForeground` (0x582550), and both are driven by WM_ACTIVATEAPP -- which
    // DirectInput's subclass swallows, so neither ever clears on its own. gplvr therefore
    // drives them by hand from a polled focus edge, and this says which way.
    //
    // Off is the right default on a monitor: a backgrounded game rendering at 144 fps is
    // heat and fan noise for pixels nobody is looking at. On is for anyone who wants the
    // picture live while they read something over the top of it.
    //
    // FORCED ON IN VR, whatever this says -- clicking another window must not black out
    // the headset. Read `keepDrawingWhileUnfocused()` rather than this field.
    //
    // A per-frame AUTOPAUSE, single-player only, would make the whole question go away:
    // pause the sim on focus loss and there is nothing to keep drawing. Not built.
    bool renderWhenUnfocused = false;

    // THE WINDOW, in pixels. 0x0 means exactly the current mode of the monitor it is on,
    // at that monitor's origin -- which is the normal setting and the fast one.
    //
    // gplvr always owns the window and the display mode: borderless, the desktop mode
    // never changed, and the world mode the engine is told about is the window's own
    // size, so this file rather than core.ini decides what resolution GPL renders at.
    // There is no option for handing that back, because the game's own policy -- switch
    // the desktop to 640x480 and back twice a race -- is the thing this exists to stop.
    int windowWidth = 0;
    int windowHeight = 0;

    // Where that window sits, relative to the monitor. -1 centres it. Ignored at 0x0,
    // because an exact cover has only one place to be.
    int windowX = -1;
    int windowY = -1;

    // How the 640x480 front end is scaled up to the window.
    //
    //   0  whole-number scale, nearest-neighbour -- every source pixel becomes an exact
    //      NxN block and the art keeps the shape it was drawn with, at the cost of a
    //      wider border, since the smaller of the two integer factors wins
    //   1  aspect-fit, nearest-neighbour -- as large as it goes, hard-edged, with uneven
    //      block sizes because the scale is fractional
    //   2  aspect-fit, linear (default) -- as large as it goes, smooth
    //
    // The menu is ALWAYS scaled. It cannot be laid out at any other size -- it is 85
    // fixed 640x480 images with absolute widget coordinates and
    // gpl_startupScreenInit_640x480 refuses to start on anything else -- so the moment
    // the window stops being 640x480 the only question left is how.
    int menuFilter = 2;

    // Show the game's own black bars.
    //
    // They are a UI LAYOUT, not a clip: `bar = 50 * height / 480`, three panels, and the
    // 3D view is one bitmapInstanceAlloc of the middle panel's rect. Off -- the default
    // -- widens that rect through a rasterizer global gplvr already owns, grows the
    // middle panel to the whole mode and hides the two bar panels through the screen's
    // own layout virtual. No exe patch: three writable vtable slots.
    //
    // On installs none of it and the game keeps its 4:3 band.
    bool enableLetterbox = false;

    // Correct the field of view for a surface wider than 4:3.
    //
    // GPL fixes the HORIZONTAL angle and derives the vertical from the surface's shape,
    // so a wider window shows LESS, not more -- a 2560x1440 window keeps three quarters
    // of the design vertical field with the bars in, 95% with them out, and a 21:9 keeps
    // far less than either. On widens the horizontal angle so the vertical field is the
    // one the cockpit art and the mirrors were drawn for; off leaves the game's own,
    // which on a wide window is the vanilla 4:3 view with less of it visible.
    bool widescreenFix = false;

    // Break the frame budget into scene / present / gplvr / other, once a second.
    //
    // On by default while the frame cost is still being chased. It exists because the
    // render timing stopped explaining the frame rate the moment vsync came off, and
    // `other` -- the part the hooks cannot see, which is GPL's own per-frame work
    // outside the world pass -- turned out to be four to six milliseconds.
    bool profileFrame = true;

    // Drain the GPU with a glFinish at the end of the scene render, and time it.
    //
    // The measurement that decides whether `present` is CPU work or a GPU queue. It
    // stalls the pipeline once a frame, so it costs frame rate and its frame rate is not
    // comparable with a run without it. Off by default; turn it on for one session.
    bool profileGpuWait = false;

    // Count polygons handed to the rasterizer, by thunking the polyDraw entry point.
    //
    // Turns the scene time into a cost per polygon, which is what says whether the
    // renderer is submitting too many primitives or spending too long on each. Off by
    // default: it writes one of the game's rasterizer globals.
    bool profilePolys = false;

    // Open the two boxes `profileFrame` only weighs: GPL's own geometry counters, and a
    // breakdown of `present` into driver submission, GPU drain and the swap.
    //
    // Both halves are close to free. The counters run whether anything reads them, and
    // the glFinish being timed is one roglv2 issues itself -- so unlike `profileGpuWait`
    // this changes nothing about the frame it measures. Off by default because it writes
    // three of roglv2's import slots. See RenderProfile.h.
    bool profileStages = false;

    // Report every millisecond of the frame as CPU WORK or CPU WAIT, in both modes
    // (docs/audit-2026-09-18/72-work-vs-wait.md).
    //
    // `pre` and `present` are the two largest terms in the flat frame line, and neither says
    // how much of itself is the render thread doing something and how much is it blocked on
    // the sim tick, the BG lock or the driver. The answer decides whether VR fits: time spent
    // waiting is budget a ring-buffer hand-off could give back, time spent working is not.
    //
    // It installs the same `pre` markers and roglv2 import thunks profileStages installs, and
    // it is the part of them that flat mode can have: profileStages also owns the frame
    // budget's GPU marks, which in flat mode belong to profilePost. profileStages implies it.
    bool profileWorkWait = false;

    // A glDrawArrays call at least this long is counted as a driver stall rather than as
    // submission work, on the work/wait line. 0 turns the attribution off and charges every
    // glDrawArrays to WORK.
    int profileStallUs = 50;

    // The headset rate the work/wait headroom line measures the frame against.
    int profileTargetHz = 90;

    // Break `post` of the flat frame line into the scene, GPL's 2D drawing entry point by entry
    // point and the present, on the CPU and with GPU timestamps, and say whether the frame is
    // CPU- or GPU-bound (PostProfile.h). Flat mode. Off by default: the GPU timer reads
    // GL_TIMESTAMP once a frame.
    bool profilePost = false;

    // Draw GPL's stamped text ourselves instead of letting roglv2 blit it a glyph at a time
    // (FastText.h, docs/audit-2026-09-18/71-fast-text.md).
    //
    // roglv2 draws every glyph with glRasterPos2i + glDrawPixels of an 8-bit colour-index image,
    // rebuilding the four GL_PIXEL_MAP_I_TO_* tables first. That is about 103 us a glyph on this
    // driver: at 2560x1440 the 56 stamps of a flat frame cost 5.78 ms of an 8.83 ms frame, more
    // than the scene, the rasterizer's flush and the present together. With this on, each glyph
    // is uploaded once into a GPU atlas and a run of glyphs is one quad list, drawn in the
    // rasterizer's own context under its own 2D transform, with the palette it would have built.
    // Stamps into anything that is not the GL framebuffer stay the rasterizer's and are counted.
    // Flat mode only: in VR the same entry points belong to Hud.cpp.
    bool fastText = true;

    // Switch it off and on in a race, to compare the picture and the frame time. 0 disables.
    // '9': '6' is the mouse-look toggle and '8' the memo mark.
    int fastTextKey = '9';

    // Write each EYE's own rectangle to a .tga next to the game, at the moment the frame
    // is handed to the headset. Not the combined side-by-side the monitor shows -- the
    // actual clipped halves, which is the picture that settles whether something is in
    // the frame or only appears to be. 0 = off.
    int dumpEyesKey = 0;


    // The OpenGL swap interval. 0 disables vsync, 1 enables it, -1 leaves the driver
    // default alone.
    //
    // 0 by default, because the driver default is 1 and NOTHING IN GPL EVER CHANGES IT
    // -- roglv2 resolves wglSwapIntervalEXT and never calls it, and its `[RasterV2]
    // VSync` key does not exist. With a 144 Hz display that turns the 6.944 ms frame
    // budget into a cliff: overrun it and the frame costs 13.889 ms, so the rate steps
    // straight from 144 to 72 with nothing in between. See SwapControl.h.
    int swapInterval = 0;

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

    // Gamma the presenter applies to everything it draws in the headset: the eyes, the menus
    // and the HUD panel. 1.0 leaves the game's colours exactly as the monitor shows them;
    // above 1.0 lifts mid-tones and shadows, with black and white staying put. 0.5 to 3.0.
    float vrGamma = 1.0f;

    // How far in front of the driver GPL's own in-race HUD appears, in metres. 0 puts it
    // at infinity.
    //
    // The HUD is drawn once per eye at the same eye-local coordinates, which is zero
    // disparity -- and for a block of 8-pixel text that is the uncomfortable case: the
    // eyes are converged on cockpit geometry a metre away and have to diverge to infinity
    // to fuse it. A few pixels of convergent disparity pulls it back to a dash distance
    // and costs one addition per item.
    //
    // Converted to pixels through the runtime's OWN eye separation and the eye's focal
    // length, so the answer is right for the person wearing the headset rather than for
    // an assumed 63 mm. Without a tracked frame there is no separation to use, and the
    // two copies stay identical.
    float vrHudDepth = 0.0f;

    // Where pribluda's overlay hangs in the headset: 0 not at all, 1 a panel that follows
    // the head, 2 a panel fixed to the seat -- still while you look around, so it is read
    // by looking at it, in the sharp middle of the lens.
    //
    // The panel is an OpenXR quad layer the runtime draws itself on every display frame and
    // samples once, straight to the display, so it holds still and stays sharp however the
    // game's own frames are paced. Its layout is gplvr_pribluda.ini. The seat is the pose
    // at the last recentre, so a seat panel appears after the first one.
    int vrHudLayer = 2;

    // Where the panel hangs, one set per mode: its centre this many metres ahead, below and
    // to the right, and its width in metres (the height follows the monitor's shape). It
    // turns to face the eye from wherever it hangs. vrHudHead* are measured from the eyes and
    // move with them, so a head panel sits at the same place in the lens whichever way you
    // look; vrHudSeat* are measured from the head pose at the last recentre and stay in the
    // car.
    HudPlacement vrHudHead = {0.70f, 0.10f, 0.0f, 0.45f};
    HudPlacement vrHudSeat = {0.70f, 0.25f, 0.0f, 0.60f};

    // How dark pribluda's backdrop boxes are on the panel: 0 invisible, 1 solid black.
    // pribluda's own boxes darken what is behind them to 70%, which is 0.30. The dash's
    // boxes, solid black in pribluda, stay solid.
    float vrHudBackdropOpacity = 0.30f;

    // Text size on the panel, RELATIVE to pribluda's own: 1.0 keeps the share of the panel
    // the text has of the monitor. Raise it if it is too small at this size and distance.
    float vrHudTextScale = 1.0f;

    // Switch the panel between following the head (vrHudLayer 1) and fixed to the seat (2)
    // while the game runs. Home by default; 0 disables. With the panel off it turns it on,
    // following the head, which needs no seat.
    int vrHudLayerKey = VK_HOME;

    // HUD tweak mode, for placing pribluda's elements on the panel from the headset. End by
    // default; 0 disables. Pressed in a race with the panel drawn it outlines one element:
    // the arrows move it, Page Up/Down select another, and the key again leaves and writes
    // the positions to gplvr_pribluda.ini. Independent of vrHudLayerKey.
    int vrHudTweakKey = VK_END;

    // How fast the arrows move an element in tweak mode, in fractions of the panel per second.
    float vrHudTweakSpeed = 0.10f;

    // The most times a second pribluda redraws the HUD panel. 0 redraws it every frame. Between
    // redraws the headset and the monitor keep showing the last image. pribluda's text changes a
    // few times a second at most, so 72 loses nothing at 144 fps and halves the panel's cost.
    // Values from 1 to 9 are raised to 10, so a panel still counts as drawn within the 0.25 s
    // the lobby hide and tweak mode wait for.
    int vrHudPanelRate = 72;

    // Which eye GPL's OWN in-race text is drawn into: 0 both, 1 the left only, 2 the right
    // only.
    //
    // Both is only comfortable when the two copies fuse, and that depends on vrHudDepth
    // putting them at a distance the eyes are already converged near. One eye cannot be
    // fused wrongly because there is nothing to fuse; the brain overlays it the way it
    // does a monocle. Turns the disparity off, since a single copy has none. Pribluda's
    // overlay is on the panel instead; see vrHudLayer.
    int vrHudEye = 0;

    // Move each eye to where it will be when its frame is actually displayed.
    //
    // GPL's frame follows its sim tick, not the headset, so a frame reaches a display one
    // or two periods after the one its pose was predicted for, varying frame to frame. The
    // runtime reprojects orientation but not position, so near geometry steps as the head
    // moves sideways. With this on, the presenter publishes the head's velocity and its
    // display clock, and each eye is moved by its velocity over the displays in between.
    bool vrPredictPosition = true;

    // LOCK THE FRAME RATE TO THE HEADSET'S REFRESH RATE. Each frame's latch waits until the
    // frame will be published just before the presenter's latch, so every displayed frame is the
    // same number of sim ticks newer than the last, and the presenter locates the views for the
    // display the frame reaches. Without it the game free-runs and the presenter drops whatever
    // it cannot show. Only while the previous frame was a tracked stereo world frame and the
    // presenter publishes its display clock. vrExtraRenders is held at 0 while it locks.
    // VR only. RenderPacing.h.
    //
    // IT TURNS vrInterpolate ON when it is off, because it cannot work without it unless the
    // display period is a whole number of sim ticks: 144 Hz into 90 Hz is 1.6 ticks a frame, and
    // holding a frame to a display deadline at a fractional ratio would show one tick twice and
    // skip the next. Interpolation costs one sim tick of latency.
    bool vrLockToHeadset = true;

    // How long before the presenter's latch a paced frame aims to be published, in
    // milliseconds, on top of the measured latch-to-publish cost. 0 to 10.
    float vrPaceMarginMs = 2.0f;

    // Interpolate the world to the frame's display time. Every car and the view -- the cockpit,
    // the own car, the mirror and trackside cameras -- are drawn blended between the two newest
    // latched sim ticks, so the world moves evenly at any display rate, and render pacing also
    // engages when the display period is not a whole number of ticks. Adds one sim tick of
    // latency (6.94 ms at 144 Hz). VR only. Interpolation.h.
    bool vrInterpolate = true;

    // Toggle vrInterpolate while the game runs, for an A/B in one race. '7' by default; 0
    // disables. VR only.
    int interpolateKey = '7';

    // Centre the world walk on the CAR rather than on the eye.
    //
    // gpl_sceneRender asks sub_460B40 where the camera is on the track (0x487D73 for the eye
    // pass, 0x4877D9 for the mirror pass) and uses the answer to pick the segment range to walk
    // and to set gpl_selectorCameraDlong/Dlat/Height. That search steps one segment at a time
    // from a hint and stops when it stops getting a consistent answer, so a point it cannot place
    // sends it several segments away: measured at the bottom of Eau Rouge returning segment 17
    // against a hint of 12, for one frame, with the eye 0.46 m from where it had just been. The
    // frame then walks another part of the track -- terrain over everything, no road, the ground
    // plane showing through. In flat GPL the camera sits in the cockpit and the point is the
    // car's; in VR the head pose moves it off. This asks about the car's position instead, which
    // is what flat GPL asks, and keeps all four of the lookup's outputs consistent. 0 to compare.
    bool fixEyeSegment = true;

    // The sim/render state ring (SimRing.h, docs/audit-2026-09-18/72-work-vs-wait.md section 6).
    // The sim thread publishes each finished tick's car sources into one of four slots.
    // 0 off. 1 publishes and VERIFIES: the render thread rebuilds each car's render record from
    // the ring and compares it with what GPL's own latch wrote, and nothing drawn changes.
    // 2 writes the record from the ring instead of reading live sim state. GPL's tick wait and
    // its lock bracket are still GPL's in both, so neither reclaims the wait yet -- that is
    // stage 3, which needs the rest of the latch in the ring. Flat mode and VR.
    int simRing = 0;

    // FLAT MODE: draw this many extra frames between two simulation ticks (FlatFrames.h).
    //
    // gpl_fgEventLoop opens every frame with a poll that sleeps until the simulation has
    // produced a new tick, so stock flat GPL draws exactly one frame per tick -- 36 a second on
    // a stock install, whatever the monitor can do. With this set, a frame whose tick has not
    // arrived yet is drawn anyway from the last latched tick, with the blend advanced to that
    // instant, so the world moves at the display rate while the simulation goes on ticking at
    // its own. 3 gives 144 frames a second on a 36 Hz sim; 7 is the ceiling.
    //
    // The simulation rate is NOT changed, so nothing the AI or the physics depend on moves. The
    // cost is the sim tick of latency interpolation has always cost, and the GPU time for the
    // extra frames. 0 leaves the loop exactly as GPL wrote it.
    int flatExtraFrames = 0;

    // Blend the world on those extra frames. With 0 they redraw the last tick unchanged, which
    // is the control that says how much of the smoothness is the blend and how much is merely
    // presenting more often.
    bool flatInterpolate = true;

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

    // What the flat window shows while VR is on:
    //
    //   0  nothing from gplvr: no blit and no clear. roglv2 still presents the window, so
    //      it shows whatever its back buffer held -- black, or the last picture blitted
    //      there -- with pribluda's monitor overlay drawn over it every frame, which
    //      accumulates because nothing wipes it. Saves the blit's GPU time.
    //   1  both eyes
    //   2  the left eye
    //   3  the right eye
    //
    // Only meaningful with vrOffscreen on, because without an off-screen frame the window
    // IS the render and there is nothing to choose. Flat mode (vr = 0) never reads it.
    int vrMonitorEye = 1;

    // Drain GL with glFinish after the frame transfer's wglDXUnlockObjectsNV, before the frame
    // is published. 1 (default) keeps it. 0 skips it and relies on the unlock, which hands the
    // textures to Direct3D and on NVIDIA waits for the GL work on them to complete. If 0 shows
    // torn, half-drawn or stale frames in the headset, the unlock does not synchronise here.
    bool vrTransferFinish = true;

    // Drop GPL's stamped text (stampDraw) that cannot reach the headset. VR only.
    //
    // In a race GPL stamps up to a few thousand glyphs a frame into the display's own
    // 2560x1440 back-buffer surface (handle 1) and screen-wide strips of it. With the
    // off-screen target active that surface is not the frame the headset receives, so the
    // text is drawn and never seen, and it cost 2-3 ms of CPU and GPU a frame when it
    // appeared. With this on, a stamp is dropped only when ALL of these hold:
    //   - the off-screen target is active and stereo has eye surfaces this frame;
    //   - the destination is not an eye, and does not span the side-by-side buffer (those
    //     are re-issued once per eye, as before);
    //   - the destination is at least 7/8 as wide as the display's back buffer, i.e. a
    //     screen-space layer and never a texture;
    //   - the destination has not appeared as either argument of a bitmapDraw since the
    //     engine last changed mode, so a surface whose pixels are copied anywhere is kept.
    // Everything else, the front end included, is drawn exactly as before.
    bool vrSkipStampText = false;

    // Draw the mouse pointer into the VR image.
    //
    //   0  off
    //   1  draw it while Windows says the pointer is showing
    //   2  draw it always, whatever Windows says
    //
    // Windows composites the pointer onto the DESKTOP, never into the game's back
    // buffer, so the picture the presenter copies has no cursor in it and GPL's whole
    // front end is unusable in a headset -- the menus answer a pointer nobody can see.
    //
    // 1 is the useful setting: it follows the game's own ShowCursor, so nothing is
    // painted over the driving view. 2 exists because it has NOT been verified that GPL
    // leaves CURSOR_SHOWING set in its menus, and if it does not then the whole feature
    // would silently do nothing. Which of the two happened is in gplvr.log either way,
    // so the answer is one ini digit rather than a rebuild.
    //
    // The parser default is 0: new behaviour does not arrive on somebody's existing ini
    // without them asking for it. The ini shipped alongside this has it on.
    //
    // What is drawn is the real HCURSOR -- the user's own arrow, their size, their
    // theme -- and it follows the shape as it changes. A cursor whose pixels cannot be
    // read draws nothing and says so in the log; there is no stand-in shape.
    int vrCursor = 0;
};

extern Options g_options;

void loadOptions(const char* fileName);

// The game's own window, from its exported GetMainWindowHandle, falling back to
// enumerating this thread's visible windows.
//
// Public because the cursor overlay has to turn a SCREEN position into a position in
// this window's client area, and that is only meaningful for exactly this HWND.
HWND gameWindow();

// Leave mirror tweak mode if it is on, saving a changed aim to the viewed car's section of
// gplvr_cars.ini. `why` is appended to the log line. HUD tweak mode calls it on entering, since the two share the
// arrows and Page Up/Down.
void mirrorTweakExit(const char* why);

// Replaces the screenDisplay function pointer. That pointer is a plain writable
// global, so this is a single store -- no code patching involved.
bool installFrameCounter(Binding& binding);

// Queues the sceneRender call-site redirection. It is applied from inside the
// frame hook rather than here, because that runs on the game's own render thread
// at a moment when sceneRender is provably not executing -- patching a rel32 from
// a worker thread while the render thread is stepping through it would be a race.
// Requires installFrameCounter() to have succeeded.
void requestSceneRenderHook(const Binding& binding);

// True once gpl_sceneRender's main world walk (the call at 0x487FAA) goes through gplvr's
// thunk, which skips it during a mirror-only render. Without it the mirror panes are off.
bool hooksMainWalkRedirected();

// Remembers our own module so the IAT patcher never redirects our own imports back
// into our own hooks. Must be called before any patching.
void setSelfModule(HMODULE module);

// Is the pointer currently clipped to a rectangle at the game's request?
//
// roglv2 issues ClipCursor with the window rect once per mode activate and never
// revisits it, so a window that moves afterwards leaves the pointer trapped in the rect
// the window used to have. Anything that moves the window has to say so.
void setCursorClip(const RECT& rect);

// Take our window proc back out of the chain, if it is still the outermost one. Called
// at DLL_PROCESS_DETACH; the proc also unchains itself at WM_NCDESTROY, whichever comes
// first. Forwarding into roglv2 after it unloads is the shutdown crash at
// rogl_windowProc, and leaving our own proc installed while this DLL unloads would be
// the same fault with the modules swapped.
void windowProcUnchain();

// True once this frame's pixels have been handed to the headset. Anything drawn after
// that reaches the monitor and not the eyes, which is the difference between "the HUD is
// in the wrong place" and "the HUD is drawn too late to be seen".
bool frameAlreadyPublished();
void releaseCursorClip();

// Whether the game should keep rendering and presenting without the foreground.
//
// `renderWhenUnfocused`, or VR, which implies it. One function rather than the expression
// written out at each of the four sites that need it -- they must agree, and four copies
// of a condition eventually do not.
bool keepDrawingWhileUnfocused();

// Rewrites ChangeDisplaySettings* entries in one module's import table.
// Call once per loaded module; safe to call repeatedly.
void patchDisplaySettingsImports(HMODULE module);

void patchDisplaySettingsImportsEverywhere();

// Redirect ONE named import of ONE module, keeping the original in `original`.
//
// Separate from the sweep above because some hooks must not be applied everywhere:
// ScreenToClient is rewritten in gplc67 only, where it has a single call site inside
// gpl_mouseReadClientDelta, and rewriting the rasterizer's copy as well would corrupt
// coordinate arithmetic that has nothing to do with the front end.
bool patchModuleImport(HMODULE module, const char* name, void* replacement,
                       void** original);

}  // namespace gplvr
