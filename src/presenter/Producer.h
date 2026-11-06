#pragma once

// The game's half of the VR channel.
//
// Creates the shared control block, launches gplvr-xr.exe as the presenter, waits to be
// told which adapter the OpenXR runtime wants, then hands it one shared D3D11 texture
// per frame containing both eyes.
//
// It runs entirely from the frame hook on the game's render thread, and it is a state
// machine rather than a blocking sequence: the presenter takes a second or two to bring
// SteamVR up, and the game must not stop while that happens. Every step that can fail
// logs once and drops the whole thing back to inactive, because a mod that stalls the
// game waiting for a headset is worse than one that quietly does not start.

#include "presenter/SharedPresentation.h"

namespace gplvr {

// Called once, after discovery. False means VR is off or the presenter could not be
// launched; the game carries on as a flat stereo mod.
bool presenterStart();

// Called from the frame hook, before the game presents, so the render target still
// holds the finished frame. Drives the handshake, then copies and publishes.
// `monoRect` (x, y, width, height, rows from the top) is the part of the frame a one-image
// publish shows; null shows all of it. Ignored for a side-by-side frame.
void presenterFrame(bool sideBySide, bool stretchToFill, const int* monoRect = nullptr);

// The off-screen render target, which is what decouples the eye resolution from the
// game window. These own the GL entry points, which is why they live here rather than
// being called on RenderTarget directly from the frame hook.
//
//   presenterEndFrame     stop drawing off-screen, before the copy and the present
//   presenterBlitToWindow show the off-screen frame in the game window
//   presenterBeginFrame   start drawing off-screen again, after the present
//
// All three are inert unless vr is on and the off-screen target actually built.
void presenterEndFrame();

// The frame transfer's cost. Noted per transfer, taken and reset by the reporter, so the
// once-a-second line can say how much of the tick the transport is using -- the only
// per-frame term that has never been measured.
void presenterNoteTransfer(long long ticks);
bool presenterTakeTransferCost(double& totalMs, int& count);

void presenterBlitToWindow(int eye);
void presenterBeginFrame();

// Draw the mouse pointer into the frame that is about to be copied and presented.
//
// Here rather than called on CursorOverlay directly from the frame hook, for the same
// reason the three above are: this side owns the resolved GL entry points and the frame
// hook has no GlFunctions of its own. `sideBySide` says whether this frame holds two
// eyes, which is what decides between one cursor and one per eye.
//
// It also saves the pixels it covers; presenterRestoreUnderCursor puts them back.
void presenterDrawCursor(bool sideBySide);

// Build the VR menu's layer image and publish it. Here for the same reason
// presenterDrawCursor is: this side owns the resolved GL entry points. It draws nothing
// into the frame -- the menu is a composition layer of its own -- and is inert unless the
// menu is open and its content has changed.
void presenterUpdateVrMenu();

// The names the runtime gives the controls each action is bound to, for the menu's help
// lines. False until the presenter has asked at least once.
bool presenterLatestInputNames(SharedInputNames& names, int32_t& generation);

// The VR menu's own quad layer.
//
// ONE SIZE FOR EVERY USER, made once. The layer is drawn by the VR menu (1024x640, its tweak
// strips 1024x400, HUD tweak 1024 by the HUD panel's shape), the in-car and replay radials
// (640x640) and the replay panel (1280x200, a dialog by its own size). Its textures are made at
// the widest of them by the tallest fixed one, in the front end before the first race entry
// (Producer's prebuild), and every image is drawn into the bottom-left of that at its own size,
// so switching between them, or opening one in a race, allocates nothing. Each user's canvas
// static_asserts against these; a request past them (HUD tweak on a monitor squarer than 16:10,
// a big dialog) is scaled down whole to fit, and actualWidth/actualHeight say to what.
inline constexpr int kMenuPanelWidth = 1280;
inline constexpr int kMenuPanelHeight = 640;

// Begin makes the menu's texture the GL draw target, cleared transparent, with a viewport of
// the image's size at GL's origin (making the textures if the prebuild did not); the size
// drawn at comes back in actualWidth/actualHeight.
// End must follow every successful Begin in the same frame and restores the previous
// draw target. Publish then copies the finished image into a shared slot under a lock of
// its own -- NOT the frame transfer's batched one, which carries exactly one panel
// already -- and publishes the pose, the extent and the sequence. Hide takes the layer
// away without touching the texture.
//
// `pose` is in the SEAT's space, which is the space the presenter builds from the
// published seat pose for the seat-locked HUD panel. False from any of them means no
// layer this frame, and the reason has been logged.
bool presenterMenuBegin(int width, int height, int& actualWidth, int& actualHeight);
void presenterMenuEnd();
bool presenterMenuPublish(const SharedPose& pose, int32_t space, float widthMetres,
                          float heightMetres);
void presenterMenuHide();

// Put back the pixels the cursor was drawn over, before the game presents.
//
// Exactly one of these per presenterDrawCursor, in the same frame, AFTER the frame has
// gone to the headset. GPL's menu does not redraw the whole back buffer and roglv2
// preserves it across the swap, so a cursor left in there stays for the rest of the
// session and the pointer paints a trail instead of moving.
//
// Gated only on the GL entry points and deliberately NOT on the presenter's stage, so a
// save can never be left unmatched by a stage change landing between the two calls.
void presenterRestoreUnderCursor();

void presenterStop();

// The most recent eye views the presenter published, or false if none yet. This is what
// Phase 3 will feed into the scene parameter block; nothing consumes it today beyond the
// log line that proves the return path works.
//
// `motion`, when given, receives the head velocity and display clock published with them.
bool presenterLatestViews(SharedView views[2], SharedPose& head,
                          SharedMotion* motion = nullptr);

// The newest motion-controller sample the presenter published, and the counter it came
// under. False when the channel is not running, nothing has been published yet, or the
// copy caught a write in progress three times running -- in every case the caller must
// keep whatever state it had rather than treating the hands as released.
//
// The counter is what tells a fresh sample from a repeat: the presenter publishes at the
// display rate and the game reads at its own, so the same sample is read several times
// over and an edge detector keyed on the values alone would see each press once per read.
bool presenterLatestInput(SharedInput& input, int32_t& sequence);

// Publish the seat -- the recentred head pose, heading only, in the presenter's LOCAL
// space -- so a layer can be placed relative to it. Called on every recentre.
void presenterPublishSeat(const SharedPose& seat);

// True once a seat has been published to the presenter.
bool presenterSeatPublished();

// Pribluda's overlay, drawn into a texture the presenter shows as a quad layer.
//
// presenterHudBegin makes gplvr's own panel texture the GL draw target, cleared transparent,
// with the aspect of aspectWidth x aspectHeight; the size chosen comes back in
// width/height. False means there is no headset channel, no framebuffer support or no
// texture, and nothing was bound. presenterHudEnd must follow every successful Begin in
// the same frame: it restores the previous draw target. No interop lock is taken for the
// draw: the image is copied into a shared slot inside the next frame transfer's lock and
// published after it, so the presenter never copies a panel that is still being drawn.
bool presenterHudBegin(int aspectWidth, int aspectHeight, int& width, int& height);
void presenterHudEnd();

// True when presenterBlitToWindow drew the headset panel over the monitor picture this frame,
// so pribluda's own monitor draw, later in roglv2's present, is to be skipped.
bool presenterMonitorHudTaken();

// The presenter's view-publication counter, or 0 if it is not publishing.
//
// Extra renders pace themselves off this rather than off a slice of the game's tick.
// Slicing produced 108 Hz into a 90 Hz display, which does not divide: about every
// fifth frame is discarded and the leftovers beat at 18 Hz, which is visible as exactly
// the stepping the extra renders were meant to remove. Locking to this counter makes
// each rendered frame correspond to one display frame.
int32_t presenterViewSequence();

// The view set the LAST RENDER actually took its eye cameras from, which is what an extra
// frame has to be newer than.
//
// Not the same question as "has the counter moved since I got here": by the time a frame has
// been drawn and presented, the presenter has usually published one or two sets already, and
// waiting for yet another spends a display period of the tick's idle tail on nothing. The
// first extra frame of a tick almost always has a fresh pose waiting for it.
int32_t presenterAcquiredViewSequence();

// The presenter's display clock from the newest published view set, in QPC ticks. False
// when it is not publishing, has no valid views, or has not published timing.
struct PresenterClock {
    int64_t latchTime = 0;    // when the presenter latched the game frame for that display
    int64_t displayTime = 0;  // the instant the published views were located for
    int64_t period = 0;       // one display
    int32_t viewLead = 0;     // displays between that latch's display and displayTime
    float   refreshRate = 0;  // queried display refresh rate; 0 = period is the runtime's own
};
bool presenterClock(PresenterClock& out);

// Ask the presenter to locate the views it publishes `lead` displays after the display of
// its current latch, 0 to 2. 0 is the unpaced behaviour.
void presenterSetViewLead(int lead);

// The last frame sequence this process published, and the last one the presenter drew.
int32_t presenterPublishedSequence();
int32_t presenterConsumedSequence();

// False when the runtime forbids the application changing the field of view. Defaults
// to true until the presenter has reported, so nothing is disabled on a guess.
bool presenterFovMutable();

// Whether the headset's images for this race exist, so that work which takes a lot of address
// space -- a track bake -- can no longer starve them (report 113). The images are made at race
// entry and released when the race is left.
//
//   Ready              nothing to wait for: they are settled for this race, or none will be made
//                      (flat mode, a presenter that is off or failed, no off-screen target, or
//                      the engine's mode cannot be read)
//   AwaitingRaceEntry  the front end is up, or the race's first frame has not been published:
//                      they are made when the car is entered
//   AwaitingPresenter  the presenter has not reported its runtime yet
enum class HeadsetMemory { Ready, AwaitingRaceEntry, AwaitingPresenter };
HeadsetMemory presenterHeadsetMemory();

// What the headset's images were sized to at the last race entry, for the VR menu's warning.
// False before the first race entry of this GL context.
struct HeadsetSizing {
    bool  inRace = false;         // that race is still on
    float fraction = 1.0f;        // of the presenter's request, per side
    bool  windowPicture = false;  // nothing fitted: the headset shows the game window's picture
};
bool presenterHeadsetSizing(HeadsetSizing& out);

}  // namespace gplvr
