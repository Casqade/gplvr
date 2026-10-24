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
    // STEREO IS WHAT `vr` MEANS. Each eye is rendered into one half of the frame -- two
    // cameras aimed at bitmapInstanceAlloc sub-rects, eyes half an IPD either side of the
    // head. It was a `stereo` key with a runtime toggle until 2026-09-27, documented as "VR
    // only, and required for it", which is a setting whose only legal value was the one vr
    // already implied.

    // The mirror pass, p[43]. The car's two mirror glasses sample opposite halves of
    // mirror.mip, and each half is rendered by a camera aimed as that glass's reflection.
    //
    // ON: both panes are rendered on EACH eye's pass, from that eye, so the reflection has
    // stereo depth like everything else in the frame -- 4 mirror renders a frame.
    // OFF: both panes are rendered once, on the left eye's pass, from the head centre, and
    // the right eye samples the same texture -- 2 renders, and a reflection that is subtly
    // wrong for both eyes instead of right for each.
    //
    // Each pane after the first in a pass costs a sceneRender whose main pass is skipped.
    //
    // It was 0/1/2 with 0 meaning "no mirror" until 2026-09-27. Turning the mirror off now
    // lives in `mirrorUpdateRate = 0`, where it belongs -- off is the far end of how often,
    // not a third mode of how -- and this went down to the one question it is really about.
    bool perEyeMirror = true;

    // Render the mirror on one frame in N, counting from the frame that latched the sim tick.
    //
    // With `frameRate` above 0 the frames between two ticks are GPL's own frames, so they redraw
    // everything the tick frame did -- the mirror included. Measured at frameRate = 2: 463
    // renders a second of which 231 were mirror-only, against a simulation ticking 36 times.
    // That doubles the cost of every frame, and since the cost of one frame is exactly what
    // limits the rate, it halves the rate.
    //
    // What the mirror shows changes with the sim tick and with your head. The tick is 36 Hz, and
    // a reflection in a rear-view mirror is the last place a few milliseconds of head lag is
    // noticed -- so skipping it on most of the frames between ticks costs little and buys a lot.
    // ONE FRAME IN N. 1 renders it on every frame, 2 on one in two, and so on -- which is why
    // the menu shows it as 1, 1/2, 1/3. 0 is off: nothing renders it and mirror.mip is filled
    // with black, because the car goes on sampling that texture and a frozen reflection reads
    // as a fault rather than an off switch. `mirror N/s +M restored` on the fps line says what
    // the setting actually produced.
    //
    // With `perEyeMirror = 2` the frames that skip get their reflection from a copy of
    // mirror.mip kept per eye (MirrorCache.h). There is one mirror texture for the whole
    // carset, so without the copies a skipped frame would leave the left eye sampling the
    // right eye's reflection and flickering between the two. The copies are built on the first
    // frame that renders the mirror; if anything refuses them the log says what, and every
    // frame renders as it did before.
    int mirrorUpdateRate = 1;

    // THE MIRROR AIM LIVES IN gplvr_mirrors.ini AND NOWHERE ELSE. vrMirrorYaw, vrMirrorPitch and
    // vrMirrorFovScale were options here until 2026-09-27, holding a fallback for cars with no
    // section of their own -- the same quantity in two files, with only the per-car copy
    // reachable from the headset. A car nobody has aimed now gets the glasses where GPL points
    // them, and aiming it in VR saves against that car (MirrorConfig.h).
    //
    // The mirror pass always renders into a framebuffer of its own rather than the top-left
    // corner of the frame, where a second pass leaves a square in the left eye. That was
    // vrMirrorOffscreen and there was never a reason to turn it off.

    // Mirror tweak mode, for aiming the glasses from the headset. 'M' by default; 0 disables.
    // VR only. While on, Left/Right turn the glasses, Up/Down tilt them and Page Up/Down
    // widen them, live in both, and those keys do not reach the game; the key again leaves
    // and, if anything changed, writes the three values to the viewed car's section of
    // gplvr_mirrors.ini. Entering it leaves HUD tweak mode, and entering HUD tweak mode leaves
    // it (saving the same way).
    int vrMirrorTweakKey = 'M';

    // How fast mirror tweak mode turns the glasses, in degrees per second.
    float vrMirrorTweakAngleSpeed = 10.0f;

    // How fast mirror tweak mode widens the glasses, in doublings per second: 0.5
    // doubles or halves it every two seconds, the same share of the field at any scale.
    float vrMirrorTweakFovSpeed = 0.5f;

    // Toggle perEyeMirror while the game runs, for an A/B from the seat. 'N' by default;
    // 0 disables. VR only. (It read 1/2 with a 0 that did nothing until perEyeMirror
    // became a plain on/off on 2026-09-27.)
    int vrMirrorModeKey = 'N';

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

    // Quit straight out instead of sitting through the credits roll. Flat mode too: the credits
    // are a screen like any other and nothing about them is VR-specific, they are just a much
    // worse thing to be shown in a headset you are trying to take off.
    //
    // One byte, not a code patch -- see creditsSkipInstall.
    bool skipCredits = true;

    // Turn on GPL's mirror "driver arms" option, which has no ini key or checkbox of its own and
    // is off. Off, the mirror draws a stock car's low-detail steering wheel in its flat colour,
    // an opaque yellow square, once RasterV2:MirrorSize is large enough to pick that LOD. On, it
    // is textured as in the forward view. Flat mode too. See MirrorDriverArms.h.
    bool mirrorDriverArms = true;

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
    //
    // NO SETTING, as of 2026-09-27. It was `fastText` with a toggle key for the A/B, and the
    // A/B has been run: it is faster and the picture is the same. The implementation stays
    // exactly as it was; only the two ways of turning it off are gone.

    // Write each EYE's own rectangle to a .tga in gplvr's folder, at the moment the frame
    // is handed to the headset. Not the combined side-by-side the monitor shows -- the
    // actual clipped halves, which is the picture that settles whether something is in
    // the frame or only appears to be. 0 = off.
    int dumpEyesKey = 0;

    // DRAW THE EXTRA (INTERPOLATED) RENDERS. 1 as always; 0 keeps blending and stops them.
    //
    // THIS EXISTS TO SEPARATE TWO THINGS frameRate = 0 TURNS OFF TOGETHER. A blending-only
    // artifact can live in the blended VALUES or in what an extra render fails to redraw --
    // it re-enters gpl_sceneRender alone, paints the world over a frame that already had its
    // 2D in it, and gplvr puts back only the HUD, the cursor and the VR ray. Testing against
    // frameRate = 0 cannot tell those apart, because it removes both at once; the 2026-10-09
    // run did exactly that and came back "artifact absent", which fits either.
    //
    // With this at 0 and frameRate unchanged: an artifact that survives is in the values, one
    // that goes is in the redraw. The `extra:` line says `SUPPRESSED on purpose` so a log can
    // never be read as if the renders merely failed to find room.
    int   vrExtraRenders = 1;

    // WHERE THE WORLD WALK IS CENTRED is no longer a setting. `fixEyeSegment` chose between
    // GPL's own lookup (0), report 84's containment fallback (1), the segment table on the
    // interpolated dlong (2) and mode 1 plus one answer a frame and a self-consistency test (3);
    // 3 cleared every artifact and is what walkLookupBody does unconditionally since 2026-10-19.

    // Toggle `frameRate` between 0 and whatever the ini asked for, live. The A/B for anything
    // that might be the blend's: 0 is vanilla pacing with no blend and no extra frames, and
    // both `blendingEnabled()` and FlatFrames' `extraLimit()` read the option per call, so
    // nothing has to be reinstalled. '7' is the key report 85 told people to use for this
    // before the two options became one. 0 = off.
    int frameRateKey = '7';


    // THE RESOLVED OpenGL swap interval: 0 and up is the value to pass to
    // wglSwapIntervalEXT, and -1 means make no call at all.
    //
    // NOT WHAT THE INI SAID. There, -1 means "let frameRate decide" and 0 and up are a
    // direct override; loadOptions resolves that before this field is written, so nothing
    // downstream has to know a frameRate exists. The two spellings of -1 are different
    // questions -- "who decides" against "what did they decide" -- and the resolved one is
    // the only one that survives.
    //
    // -1 resolved is reached by `frameRate = 0` in flat mode, which is vanilla, and vanilla
    // makes no call: GPL never touches the swap interval, so WGL's specified initial value
    // of 1 stands and stock GPL runs with vsync ON. With a 144 Hz display that turns the
    // 6.944 ms frame budget into a cliff -- overrun it and the frame costs 13.889 ms, so the
    // rate steps straight from 144 to 72 with nothing between. See SwapControl.h.
    int swapInterval = -1;

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

    // Hear from the head instead of the driver's eye. In the cockpit, GPL's sound listener is
    // moved to the head's position and turned with it, so turning your head moves the other cars
    // between your ears; the car's own engine moves too. Pan only, as GPL computes it -- nothing
    // sounds behind or above. TV cameras and replays keep GPL's own. VR only. See HeadAudio.h.
    bool vrHeadAudio = true;

    // How hard the head's sound pans, 0 to 2. GPL's own curve leaves the far ear only 3 dB down
    // for a car 30 degrees off centre, so a head turn is barely heard. 1 follows the sine of the
    // angle (30 deg about 6 dB, 45 about 10, 90 about 26) and pulls near sources to the centre far
    // less; above 1 is steeper still. 0 keeps GPL's curve. Needs vrHeadAudio. Also a VR menu
    // row: the render thread reads it at every head publish (HeadAudio.h).
    float vrHeadAudioWidth = 1.0f;

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

    // HOW FAR IN FROM THE EYE'S EDGES GPL'S OWN 2D IS PULLED, as a fraction of the half
    // extent, about the eye's centre. 1.0 keeps the layout GPL computed, which is the only
    // value that preserves every relative position exactly; 0.6 brings a corner item to 20%
    // in from the edge.
    //
    // Below 1.0 this scales POSITIONS and not the art drawn at them, so a dense group
    // compresses while its pieces keep their authored size: the replay control band is ten
    // buttons at a 37-pixel step, which at 0.6 becomes a 22-pixel step and collides. The
    // tool for bringing a whole group into the comfortable cone is moving the group in its
    // own layout (Letterbox.cpp), not this. Use this for the sparse in-race HUD.
    float vrHudInset = 1.0f;

    // THE MIRROR GLASSES ARE NOT AN OPTION. They are read out of the car the game loaded
    // (MirrorGeometry.h), each is given a region of mirror.mip of its own by moving its UVs
    // (MirrorUv.h), and a car the walk refuses falls back to the hardcoded 1967 table and the two
    // halves. That was mirrorGeometry and mirrorUvSplit until 2026-10-12; mirrorTextures, a
    // texture per glass, could never work and went with them.

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

    // Text on the panel is drawn at kPribludaTextScale times pribluda's own size, a constant
    // in Pribluda.cpp. It was `vrHudTextScale` until 2026-09-27, and it is not a setting
    // because the panel's own size already decides how big the text looks: making the panel
    // wider makes everything on it bigger together, which is the control a driver wants.

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

    // ONE FRAME IN N, exactly as mirrorUpdateRate above, and shown the same way in the menu:
    // 1, 1/2, 1/3. Between redraws the headset and the monitor keep the last image, so a
    // lower rate costs nothing but the age of the numbers -- and pribluda's text changes a
    // few times a second at most, so 1/2 loses nothing at 90 fps and halves the panel's cost.
    //
    // No 0 here, unlike the mirror: turning the HUD off is its own thing, and it is the
    // menu's "Pribluda HUD" row, which skips the whole pass rather than freezing it.
    int pribludaUpdateRate = 2;

    // GPL's OWN in-race text goes into BOTH eyes, at the disparity vrHudDepth asks for. It
    // was `vrHudEye`, which could put it in one eye only; that is not something to configure
    // once the depth is right, and the text is being rerouted onto a layer of its own anyway.

    // WHO DRAWS GPL'S STAMPED TEXT IN VR, and the only switch that changes anything about it.
    //
    // 0 leaves it exactly where it is today: roglv2 blits each glyph with glDrawPixels into
    // whatever framebuffer is bound, which in VR is the off-screen eye target, at the
    // coordinates GPL computed for a 2560x1440 monitor. That puts the notices in the left eye's
    // periphery at zero disparity, and nothing can move them: the destination handle decides
    // WHERE IN A SURFACE a glyph lands, not which framebuffer receives it, so re-issuing a call
    // against an eye surface moves the pixels nowhere at all (measured 2026-10-07; Hud.cpp's
    // destinationCanSplit is why it declines).
    //
    // GPL'S FPS COUNTER AND LAP TIMES ARE OUT OF REACH OF BOTH VALUES, and that is a fact about
    // where GPL draws them rather than a gap here: both go through drawDebugText to
    // screenGetBackBuffer(), the back buffer is not what the presenter hands the headset, and so
    // the headset holds no copy of them to place or to enlarge. They are on the monitor only.
    //
    // 1 hands the stampDraw global to the fast text path, which already reads the glyph out of
    // roglv2's own font data and draws it from a GPU atlas as a quad of its own. A quad has a
    // size and a position we choose, so vrGplTextScale and vrGplTextInset below become
    // possible -- and a quad goes through the same transform the raster position did, so at
    // scale 1 and inset 1 it lands on the pixels roglv2 would have written.
    //
    // FLAT MODE IS NOT THIS SETTING. There the fast text path installs because it is faster
    // (103 us a glyph becomes one batched draw) and GPL's layout is left alone, which is what
    // a monitor wants. This decides the VR case only, and the placement below is VR-only.
    int vrGplText = 0;

    // HOW MUCH BIGGER GPL'S STAMPED GLYPHS ARE DRAWN, as a whole-number multiple.
    //
    // Whole numbers only, and that is the format's doing rather than a simplification: the
    // atlas holds roglv2's own 8-bit COLOUR INDICES and the shader takes the top nibble, so a
    // texel cannot be interpolated with its neighbour -- an index halfway between two palette
    // rows is not a colour halfway between them. Nearest sampling at an integer multiple
    // reproduces each source texel as an n-by-n block, which is what enlarging a bitmap font
    // means; a fractional multiple would drop and double rows unevenly along a line of text.
    //
    // Needs vrGplText = 1. 1 is the size GPL authored.
    int vrGplTextScale = 1;

    // HOW FAR IN FROM THE EYE'S EDGES GPL'S STAMPED TEXT IS PULLED, about the eye's centre,
    // as a fraction of the half extent. The same quantity vrHudInset names for the per-eye
    // re-issue path, applied where it can actually take effect.
    //
    // THE DELTA IS IN GPL'S OWN PIXELS, and this is how much of it survives. A run's anchor is
    // measured from the centre of GPL's layout, multiplied by this, and laid off from the
    // centre of the eye; 0 would be dead centre and 1.0 keeps GPL's offsets at their authored
    // size. 1.0 is already well inside the eye rather than at its edge, because the eye is
    // bigger than the layout it was authored for -- 3172x3172 against 2560x1440 on the install
    // this was measured on, so a corner item lands about 10% in. Turn it down from there.
    //
    // It went through FRACTIONS until 2026-10-08 -- a fraction of the layout re-read as the
    // same fraction of the eye -- and the two rects are not the same shape, so that stretched
    // x by 1.24 and y by 2.20 while no glyph size followed.
    //
    // It moves a RUN's anchor and not the glyphs within it, so the spacing inside a line of
    // text is exactly GPL's whatever this says -- which is the difference between this and
    // vrHudInset, whose per-glyph scaling collapsed the replay band's 37-pixel button step.
    // A run is recognised across frames by its stamp set and the baseline GPL asked for, so its
    // extent is known from the frame before and a line is centred as a line.
    //
    // Needs vrGplText = 1.
    float vrGplTextInset = 1.0f;

    // `vrPredictPosition` was here and is gone as of 2026-10-05: it moved each eye to where it
    // would be when the frame was displayed, and measured `mean eye shift 0.00 mm` in 189
    // windows across two installs. VrCamera.cpp records what it did and why it was zero.

    // HOW OFTEN THE GAME DRAWS A PICTURE, and THE ONLY FRAME-RATE SETTING THERE IS: vsync is
    // derived from it in loadOptions, in both modes, so the two cannot disagree. `vr` already
    // says which mode the game is in, so this does not repeat it.
    //
    // THREE RATES MEET HERE, which is one more than most games have and the reason this used
    // to take two settings. The sim ticks at 36 or 144 depending on which exe was patched; the
    // display refreshes at its own rate; the game draws somewhere between. GPL's frame loop
    // opens by sleeping until the tick lands, which welds the third rate to the first. This is
    // what unwelds it, and the display side then has nothing left to ask.
    //
    //   0  VANILLA. One frame per sim tick, because the loop's wait stands. Stock behaviour,
    //      and the baseline the other two are measured against -- including in the swap
    //      interval, which is left uncalled because that is also what GPL does.
    //
    //   1  DISPLAY. That wait is skipped and a frame is drawn for every frame the display
    //      shows: in VR, one per view set the presenter publishes; in flat mode, one per
    //      refresh, which the swap interval does by blocking in SwapBuffers.
    //
    //   2  UNLIMITED. The wait is skipped and nothing else paces the loop, so the rate is
    //      1000 / the cost of one frame, to a ceiling of 7 extra frames per tick. Vsync off.
    //
    // 1 and 2 are the SAME MECHANISM -- FlatFrames skips the loop's tick wait -- differing only
    // in what is allowed to gate a frame afterwards. THE SIMULATION RATE IS UNTOUCHED by both:
    // every frame between two ticks redraws the last latched tick with the blend advanced, and
    // reads no simulation state.
    //
    // 1 on a 36 Hz exe is what a 144 Hz exe achieves by accident -- its ticks arrive faster
    // than frames can be drawn, so the loop's wait never blocks. That is the whole of the
    // difference between the two binaries, and the whole of why the 36 Hz one felt like 36.
    //
    // 1 and 2 BOTH INTERPOLATE, and that is not a second setting.
    //
    // A frame drawn between two ticks without the blend is a duplicate of the one before it:
    // more frames, identical picture -- and in a headset, a doubled one. The blend is what
    // makes a rate above the sim's mean anything, so it is not separable from asking for one.
    // It was a second option until 2026-09-27, and every path that could reach the pair
    // separately had to carry a rule saying they could not be; the state is simply gone now.
    // `blendingEnabled()` in Interpolation.cpp is the one place that reads this as "is the
    // world blended", and everything else asks it.
    //
    // THE COST is one sim tick of latency (6.94 ms at 144 Hz), which is what buys the two
    // ticks a frame can be drawn between. 0 does not pay it.
    int frameRate = 1;

    // The sim/render state ring (SimRing.h, docs/audit-2026-09-18/72-work-vs-wait.md section 6).
    // The sim thread publishes each finished tick's car sources into one of four slots.
    // 0 off. 1 publishes and VERIFIES: the render thread rebuilds each car's render record from
    // the ring and compares it with what GPL's own latch wrote, and nothing drawn changes.
    // 2 writes the record from the ring instead of reading live sim state. GPL's tick wait and
    // its lock bracket are still GPL's in both, so neither reclaims the wait yet -- that is
    // stage 3, which needs the rest of the latch in the ring. Flat mode and VR.
    int simRing = 0;

    // The baked renderer (src/baked, src/baked_gpl; docs/audit-2026-09-18/105-baked-renderer-
    // rewrite.md). 0 off: GPL draws everything. 1 bakes the track GPL loads on a worker thread,
    // then draws it from the bake with real depth in every main pass; GPL draws the rest -- the
    // horizon, cars, cockpit, flagmen, particles, lit polygons, the mirrors -- tested against
    // that depth. Flat mode and VR.
    int bakedRenderer = 0;
    // Baked drawing off and on in the same session, for an A/B against GPL's own. 0 disables.
    int bakedRendererKey = '5';
    // An A/B capture: the next frame drawn by GPL alone and the one after it from the bake,
    // each main pass written as gplvr-captureN-{vanilla,baked}.tga beside gplvr.dll with its
    // camera and track position in the log. 0 disables.
    int bakedCaptureKey = '8';
    // Cycles what the baked draw shows: normal; the walk-order phase (sprites, lines, translucent
    // polygons, standing and object cutouts) without its test; without the walk-order phase; the
    // walk-order phase tested by depth alone, without GPL's order between items; two timing views
    // of the walk-order phase, with its state calls but no draw calls, and with no GL calls.
    // Tells a wrong order from content the solid surfaces hide. 0 disables.
    int bakedDebugKey = '9';
    // 1: the baked renderer draws each phase as one multi-draw, every texture through a bindless
    // handle (needs ARB_bindless_texture; without it, as 0). 0: batch by batch, binding each
    // batch's texture and state.
    int bakedMergedDraws = 1;
    // How many times the baked renderer blends the racing groove (surface class 20) over each
    // pixel: 1 as GPL's kernel and roglv2 do, 2 as if twice. Vanilla measured once at two spots
    // of spa67 and as twice at a third (report 68's top update); no mechanism for twice is known.
    int bakedGrooveLayers = 1;
    // Baked cars (report 105 section 4, R2). 0: GPL draws every car. 1, stage 1: every car GPL
    // draws is also walked by gplvr's own control walk (CarWalk.cpp) with the same register, gate,
    // selector and cull decisions, and its polygon count is checked against GPL's own counter for
    // that draw; the picture does not change. 2, stage 2a: as 1, and the walk also fills a draw
    // list per car draw from car objects baked out of GPL's memory (CarBake.cpp), checked against
    // the walk's count; still nothing drawn from it. 3, stage 2b: in the baked main passes gplvr
    // draws the cars itself from those lists (CarRenderer.cpp) and GPL's draw of each car keeps
    // only its blob shadow; mirror passes and the cockpit stay GPL's, counted but not walked (1
    // and 2 verify every draw). Needs bakedRenderer = 1.
    int bakedCars = 0;
    // The flagmen and the starter (report 105 section 4.13). 1: in the baked main passes gplvr
    // draws them itself (Flagmen.cpp, through the car renderer, so it needs bakedCars = 3), each
    // sprite in the 16 colours GPL sets for it and at its place in GPL's walk; whatever it cannot
    // resolve stays GPL's. 0: GPL draws them, as before.
    int bakedFlagmen = 1;

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

    // IN VR THE FRAME IS ALWAYS RENDERED OFF-SCREEN, at the size vrRenderScale asks for
    // rather than the size of a Windows display mode.
    //
    // It was `vrOffscreen`, off by default for its first release with two questions to
    // answer -- whether the menus still place their cursor and whether GPL's HUD follows
    // the larger surface. Both were answered in the affirmative and it has been on ever
    // since; the switch went on 2026-09-27. Three things now depend on it that did not
    // then: vrRenderScale means nothing without it, the aiming ray needs somewhere the
    // monitor cannot see, and the flat window is a view OF the frame rather than the frame.

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

    // Render the frame straight into the shared texture the presenter reads, instead of
    // rendering into our own and copying it across at publish. Report 80.
    //
    // The copy is a glBlitFramebuffer of the whole frame -- 80.5 MB read and written at
    // 6344x3172, measured at 1.0 to 1.9 ms of an 11.11 ms budget. Nothing needs it: the
    // shared texture is already a complete GL render target, so the off-screen framebuffer
    // can take it as colour attachment 0 and the game draws into it directly.
    //
    // What changes for the presenter is how long a slot is held: the interop lock now spans
    // the whole frame rather than the moment of the copy, and there are two slots. What
    // changes for the game is that a frame no longer starts from the previous one's pixels
    // -- the texture is registered WRITE_DISCARD and the two slots alternate -- so anything
    // GPL draws once and expects to survive would show every other frame.
    //
    // Requires vrOffscreen. A lock or an attachment that fails drops back to the copy for
    // the rest of the session and says so.
    bool vrDirectToShared = false;

    // GPL'S STAMPED TEXT IS NEVER DROPPED. `vrSkipStampText` threw away stamps that could
    // not reach the headset -- a few thousand glyphs a frame into a surface the eyes never
    // see, worth 2-3 ms of CPU and GPU. It went on 2026-09-27 because the answer is to put
    // that text somewhere it IS seen rather than to decide which of it to bin, and the rule
    // for "unseen" was four conditions deep and had to be kept true by hand.
    //
    // THE MOUSE POINTER is always drawn into the VR image while Windows says it is showing
    // (CursorOverlay.h); `vrCursor` chose that, off, or always until 2026-10-19.

    // What the headset is given while GPL shows its "no signal" static (gpl_drivingViewWidgetDraw
    // found no view to render: a camera or driver switch, a cut). GPL draws the static over the
    // surface of the camera bound last -- on the replay screen the 3D view's, the window's size at
    // the frame's top left -- so published as a stereo pair it is noise in one eye over the
    // previous picture.
    //   0  published as before: as a pair, with no head pose
    //   1  in a race, nothing is published and the headset keeps showing the last frame for the
    //      first kNoSignalHoldMs of a spell of static, then the static fills both eyes as one
    //      flat image filling the eye; on the replay screen it goes out that way at once
    int vrNoSignal = 1;
};

extern Options g_options;

void loadOptions(const char* fileName);

// How many frames have been presented. It advances once per frame in both modes, in the
// present hook, which is BETWEEN scene renders -- so every pass of one frame (two eyes and
// the mirror pass with them) reads the same value, and it changes between frames. That makes
// it the key for anything that must answer a question once per frame and give every pass the
// same answer; `walkLookupBody`'s per-frame cache uses it for exactly that.
unsigned hooksFrameCount();

// Write the HUD panel's eight placement keys back to gplvr.ini, where the panel tweak mode
// left them. The panel has no file of its own the way the mirror aim and the HUD layout do,
// so its own values go back into the one loadOptions read. False when a write failed, which
// the log line then names.
bool hudPlacementSave();

// The game's own window, from its exported GetMainWindowHandle, falling back to
// enumerating this thread's visible windows.
//
// Public because the cursor overlay has to turn a SCREEN position into a position in
// this window's client area, and that is only meaningful for exactly this HWND.
HWND gameWindow();

// Leave mirror tweak mode if it is on. `save` writes a changed aim to the viewed car's section
// of gplvr_mirrors.ini; without it the aim goes back to what the file holds. `why` is appended
// to the log line.
//
// Only the tweak key itself passes true. HUD tweak mode calls this on entering -- the two share
// the arrows and Page Up/Down -- and that is somebody switching tools, not somebody saving.
void mirrorTweakExit(const char* why, bool save);

// True while the KEYBOARD mirror tweak mode holds the aim. The semantic tweak API refuses
// while it does: the headset's mirror mode edits the same three numbers, and two owners of
// one aim is how a value ends up somewhere neither of them put it.
bool mirrorTweakActive();

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

// Clear GPL's "roll the credits on the way out" flag, when skipCredits asks for it. Flat mode
// too. One byte; no hook, so there is nothing to survive a mode change.
void creditsSkipInstall();

// Redirect ONE named import of ONE module, keeping the original in `original`.
//
// Separate from the sweep above because some hooks must not be applied everywhere:
// ScreenToClient is rewritten in gplc67 only, where it has a single call site inside
// gpl_mouseReadClientDelta, and rewriting the rasterizer's copy as well would corrupt
// coordinate arithmetic that has nothing to do with the front end.
bool patchModuleImport(HMODULE module, const char* name, void* replacement,
                       void** original);

}  // namespace gplvr
