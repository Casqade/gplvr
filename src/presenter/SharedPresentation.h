#pragma once

// The contract between the 32-bit game process and the 64-bit presenter.
//
// One definition, compiled into both, so every field is fixed-width and there is not a
// pointer anywhere: a pointer would be four bytes on one side and eight on the other and
// the struct would silently disagree with itself. Same reason for the explicit
// static_asserts at the bottom -- they are the only thing standing between us and a
// layout mismatch that would look like corrupted frames rather than like a build error.
//
// The shape is BFVR's (src/presenter/SharedPresentationProtocol.h): a control block in
// shared memory, interlocked state transitions, and every payload published *before* the
// counter that advertises it. The fields are not BFVR's -- most of that protocol is
// BF1942 semantics (haptics, kill sounds, weapon scopes, water masks) and a racing car
// needs none of it.
//
// Who says what:
//
//   producer (x86)                        presenter (x64)
//   --------------                        ---------------
//   create block, launch presenter   -->
//   Starting
//                                    <--  adapter LUID + target eye size
//                                         RequirementsReady
//   create device on THAT adapter,
//   create shared textures, publish
//   TexturesReady                    -->
//                                    <--  open textures, Running
//   frameSequence++ per frame        -->
//                                    <--  viewSequence++ per frame (head + eye poses)
//
// The adapter handshake is not ceremony. The runtime names the adapter it wants by LUID,
// and a texture shared from a device on a different adapter cannot be opened at all -- so
// the producer cannot create its textures until the presenter has told it which GPU.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace gplvr {

inline constexpr uint32_t kControlMagic = 0x52565047;  // 'GPVR'
// 16: the VR menu panel is one fixed size and each slot names the part its image fills;
// 15: the VR menu's own quad layer and the runtime's input-source names;
// 14: controller input added; 13: eyeSubImageScale removed (rebuild both)
inline constexpr uint32_t kControlVersion = 18;

// Two frame slots, written alternately. The producer never writes the slot the presenter
// is reading, so no lock is needed and a torn frame is impossible; the cost is one
// texture's worth of memory and at most one frame of latency.
inline constexpr uint32_t kFrameSlots = 2;

enum class ProcessState : int32_t {
    Failed = -1,
    Empty = 0,
    Starting = 1,
    RequirementsReady = 2,
    TexturesReady = 3,
    Running = 4,
    Stopping = 5,
    Stopped = 6,
};

struct SharedPose {
    float orientationX = 0.0f;
    float orientationY = 0.0f;
    float orientationZ = 0.0f;
    float orientationW = 1.0f;
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;
};

struct SharedFov {
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleUp = 0.0f;
    float angleDown = 0.0f;
};

struct SharedView {
    SharedPose pose;
    SharedFov  fov;
};

// A QueryPerformanceCounter value, split in halves like the texture handle so the field
// has the same shape in both builds whatever each compiler does with a 64-bit member's
// alignment. QPC is one clock for the whole machine, so a value taken in one process names
// the same instant in the other.
struct SharedQpc {
    uint32_t low = 0;
    uint32_t high = 0;
};

inline void storeQpc(SharedQpc& slot, int64_t value) {
    slot.low = static_cast<uint32_t>(static_cast<uint64_t>(value));
    slot.high = static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32);
}

inline int64_t loadQpc(const SharedQpc& slot) {
    return static_cast<int64_t>(static_cast<uint64_t>(slot.low) |
                                (static_cast<uint64_t>(slot.high) << 32));
}

// How the head is moving, and the display clock the views belong to. Published with every
// set of views so the game can move its eyes to where they will be when the frame it is
// about to render actually reaches the display.
struct SharedMotion {
    float     linearVelocity[3] = {};   // m/s, presenter LOCAL space
    float     angularVelocity[3] = {};  // rad/s, presenter LOCAL space
    int32_t   flags = 0;                // kMotion* bits
    SharedQpc displayTime;              // the instant these views were located for
    SharedQpc latchTime;                // when the presenter took the game frame shown then
    SharedQpc period;                   // one display frame
    // Displays between the display of the latch at latchTime and displayTime: 0 unpaced, 1 or
    // 2 while the game paces its frames (ControlBlock::viewLeadDisplays, as the presenter
    // read it for these views).
    int32_t   viewLead = 0;
    // The display refresh rate in Hz, queried through XR_FB_display_refresh_rate; 0 when the
    // runtime does not report one. When non-zero, `period` and the view lead are one refresh
    // of it; otherwise they are the runtime's predictedDisplayPeriod. SteamVR under PICO
    // Connect keeps predictedDisplayPeriod at the 90 Hz it started with while the headset
    // runs at 72.
    float     refreshRate = 0.0f;
};

inline constexpr int32_t kMotionLinear = 1;
inline constexpr int32_t kMotionAngular = 2;
inline constexpr int32_t kMotionTiming = 4;

// -- motion controllers ---------------------------------------------------------------
//
// OpenXR lives only in the presenter, so this is the only way controller state reaches the
// game at all. The presenter samples one action set once per frame, at the same predicted
// display time it locates the views for, and publishes the result here; the game reads it
// from its own frame hook whenever it gets round to it.
//
// Poses are in the presenter's LOCAL space and OpenXR's axes, exactly like SharedPose
// everywhere else in this block, so the game converts a hand through the same seat
// transform it already converts the eyes through (VrCamera::vrSeatRelative). Nothing here
// is pre-converted: a second conversion living in the presenter is a second place for the
// sign errors this project has already paid for.
//
// An aim pose is OpenXR's "aim": its -Z is the direction the controller points, aligned
// with the device's own pointing ray rather than with the grip. A grip pose is the hand's
// hold. Both are located, so both carry their own validity bits.

inline constexpr int32_t kHandLeft = 0;
inline constexpr int32_t kHandRight = 1;
inline constexpr int32_t kHands = 2;

// SharedHand::flags. Aim and grip carry VALID and TRACKED separately because the two mean
// different things: valid says the pose field holds a number, tracked says the runtime is
// actually following the device rather than extrapolating from its last known place.
inline constexpr int32_t kHandAimActive = 0x0001;
inline constexpr int32_t kHandAimPositionValid = 0x0002;
inline constexpr int32_t kHandAimOrientationValid = 0x0004;
inline constexpr int32_t kHandAimPositionTracked = 0x0008;
inline constexpr int32_t kHandAimOrientationTracked = 0x0010;
inline constexpr int32_t kHandGripActive = 0x0020;
inline constexpr int32_t kHandGripPositionValid = 0x0040;
inline constexpr int32_t kHandGripOrientationValid = 0x0080;
inline constexpr int32_t kHandGripPositionTracked = 0x0100;
inline constexpr int32_t kHandGripOrientationTracked = 0x0200;
inline constexpr int32_t kHandTriggerActive = 0x0400;
inline constexpr int32_t kHandSqueezeActive = 0x0800;
inline constexpr int32_t kHandStickActive = 0x1000;
// The runtime has bound an interaction profile to this hand. Zero means the controller is
// off, asleep or unknown to the runtime, and every other bit is then meaningless.
inline constexpr int32_t kHandProfileBound = 0x2000;

// SharedHand::buttons. Two face buttons, because that is what every profile has: A/B on a
// right Touch or an Index, X/Y on a left Touch, and the trackpad or menu click standing in
// where a controller has neither.
inline constexpr int32_t kButtonStick = 0x1;      // thumbstick or trackpad click
inline constexpr int32_t kButtonPrimary = 0x2;    // A / X / select
inline constexpr int32_t kButtonSecondary = 0x4;  // B / Y
inline constexpr int32_t kButtonMenu = 0x8;       // the system-adjacent menu click

struct SharedHand {
    int32_t    flags = 0;
    int32_t    buttons = 0;
    SharedPose aimPose;
    SharedPose gripPose;
    float      trigger = 0.0f;  // 0 to 1
    float      squeeze = 0.0f;  // 0 to 1
    float      stickX = 0.0f;   // -1 to 1, right positive
    float      stickY = 0.0f;   // -1 to 1, forward positive
};

// SharedInput::flags.
// The session had input focus when this was sampled. Without it OpenXR reports every
// action inactive, so a sample taken then is all zeroes and must not be read as "the
// driver let go of everything".
inline constexpr int32_t kInputFocused = 1;

// ControlBlock::menuSpace: which of the presenter's spaces the menu quad's pose is in.
inline constexpr int32_t kMenuSpaceSeat = 1;
inline constexpr int32_t kMenuSpaceView = 2;

// WHAT THE RUNTIME CALLS EACH CONTROL, for the menu's help lines.
//
// OpenXR has no glyph or icon API -- nothing in the headers returns a picture of a button
// -- but xrGetInputSourceLocalizedName is core, and it names what an action is ACTUALLY
// bound to on the hardware attached right now. That matters more than a picture would:
// our own binding table names controls in our words, and we do not know which interaction
// profile a given headset will bind, so a hardcoded "trigger" can simply be wrong. The
// runtime's own name is right by construction and follows a rebinding.
//
// Fixed-size and truncated rather than a second variable-length mechanism across the
// bitness boundary. One entry per action per hand, indexed the way XrInput's action enum
// is; kInputActions is asserted against that enum on the presenter side.
inline constexpr uint32_t kInputActions = 9;
inline constexpr uint32_t kInputNameCapacity = 48;

// The order SharedInputNames is indexed in. HERE rather than beside the action set,
// because the 32-bit side needs it and cannot see the OpenXR headers at all; XrInput's
// own enum is asserted against it so the two can never drift.
enum SharedInputAction {
    kInputAimPose = 0,
    kInputGripPose,
    kInputTrigger,
    kInputSqueeze,
    kInputStick,
    kInputStickClick,
    kInputPrimary,
    kInputSecondary,
    kInputMenu,
};

struct SharedInputNames {
    char text[kHands][kInputActions][kInputNameCapacity] = {};
};

struct SharedInput {
    int32_t   flags = 0;
    // Written zero, read by nothing. It exists so the struct's size is stated rather than
    // left to whatever each compiler does after an odd number of 4-byte members.
    int32_t   reserved0 = 0;
    // When the presenter published this, in QPC ticks. One clock for the whole machine, so
    // the game can age the sample without the two processes agreeing on anything else.
    SharedQpc sampleTime;
    SharedHand hands[kHands];
};

// A legacy D3D11 shared handle -- IDXGIResource::GetSharedHandle, not an NT handle. It
// is valid in any process on the same adapter, which is what makes this work across the
// bitness boundary at all. Stored as two 32-bit halves so the field is the same size in
// both builds; the value itself always fits in the low half today, but splitting it
// costs nothing and removes the question.
struct SharedTextureHandle {
    uint32_t low = 0;
    uint32_t high = 0;
};

inline void storeSharedHandle(SharedTextureHandle& slot, HANDLE handle) {
    const uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    slot.low = static_cast<uint32_t>(value);
    slot.high = static_cast<uint32_t>(value >> 32);
}

inline HANDLE loadSharedHandle(const SharedTextureHandle& slot) {
    const uint64_t value =
        static_cast<uint64_t>(slot.low) | (static_cast<uint64_t>(slot.high) << 32);
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
}

struct ControlBlock {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t byteSize = 0;
    uint32_t producerProcessId = 0;
    uint32_t presenterProcessId = 0;

    volatile int32_t producerState = static_cast<int32_t>(ProcessState::Empty);
    volatile int32_t presenterState = static_cast<int32_t>(ProcessState::Empty);
    volatile int32_t shutdownRequested = 0;

    // -- presenter to producer, valid once presenterState >= RequirementsReady --------
    uint32_t adapterLuidLow = 0;
    int32_t  adapterLuidHigh = 0;
    // The size the game should render EACH EYE at: the runtime's recommendation
    // multiplied by vrRenderScale and clamped to the runtime's own maximum.
    //
    // Already scaled and clamped, on purpose. This used to carry the raw recommendation
    // and the presenter applied the scale again when it created its swapchains, which
    // was fine while nothing else read it -- but the moment the game renders off-screen
    // there are two places computing the same number from the same inputs, and two such
    // places eventually disagree. One number, computed once, by the side that knows the
    // runtime's maximum.
    uint32_t targetEyeWidth = 0;
    uint32_t targetEyeHeight = 0;

    // -- producer to presenter, valid once producerState >= TexturesReady ------------
    //
    // One texture per slot holds BOTH eyes: Phase 1 already renders them into the two
    // halves of one buffer, so a single transfer moves the frame and the presenter
    // samples two sub-rects out of it. Splitting them here would mean an extra copy on
    // the producer side to gain nothing.
    uint32_t            textureWidth = 0;
    uint32_t            textureHeight = 0;
    uint32_t            textureFormat = 0;  // DXGI_FORMAT
    SharedTextureHandle textures[kFrameSlots] = {};

    // Bumped every time the textures are recreated. The game changes render size at
    // every menu-to-sim transition, so this is not an edge case -- it happens on the way
    // into every session. The presenter re-opens when it sees this change.
    volatile int32_t textureGeneration = 0;

    // Non-zero when the game rendered the runtime's exact frustum into each half. The
    // image then already carries the eye's field of view, anamorphically if the half is
    // not square, so the presenter must STRETCH it to fill the swapchain. Aspect-fitting
    // it would letterbox a picture that is already correct and shrink the world.
    volatile int32_t stretchToFill = 0;

    // The exact width of one eye's half, in pixels. Not textureWidth/2: an odd-width
    // buffer leaves a column belonging to neither eye, and a clean 50% split would then
    // sample the right eye half a pixel away from what was rendered into it -- a
    // disparity error on one eye only, which is far easier to feel than to see.
    uint32_t eyeHalfWidth = 0;

    // The exact sub-rect each eye was rendered into, in SOURCE pixels, image-space:
    // origin top-left, the way the game sees the surface. [eye][x, y, w, h].
    //
    // eyeHalfWidth could only ever describe a full-height left/right split. It cannot
    // describe a SQUARE eye -- a 1280x1280 sub-rect inside a 1280x1440 half -- and a
    // square eye is the point: GPL's `aspect` is a vertical stretch factor, and the rest
    // of the engine assumes it is 1 (gpl_cameraBind hands the rasterizer the fov and
    // never the aspect; gpl_lodScreenScale is computed from the horizontal axis alone).
    // Matching the target's SHAPE to the frustum keeps aspect at 1.0 and takes the
    // engine out of a regime it was never built for. It is also what BFVR does, by
    // sizing the game's render target from the runtime instead of compensating
    // afterwards, and it is why BFVR does not have this problem.
    //
    // All zero means "fall back to the eyeHalfWidth split", so a producer that has not
    // filled this in still works.
    //
    // With sideBySide zero, a non-zero rect is the part of the texture that one image is in,
    // the same for both eyes; all zero means the whole texture.
    uint32_t eyeRect[2][4] = {};

    // Non-zero when the two eyes are the left and right halves of the texture, which is
    // what Phase 1's stereo produces. Zero means one image for both eyes -- the menus,
    // or stereo switched off -- and the presenter shows it to both eyes rather than
    // showing each eye half a picture.
    int32_t sideBySide = 0;

    // Non-zero when the image is stored bottom-up, which it is for a GL back buffer
    // copied through WGL_NV_DX_interop2. Measured, not assumed -- see STATUS.md. The
    // presenter inverts v rather than the producer paying for a flip.
    int32_t textureBottomUp = 1;

    // -- per frame, producer to presenter --------------------------------------------
    // Published AFTER the slot's pixels are complete. The presenter reads
    // frameSequence, then reads slot (frameSequence % kFrameSlots).
    volatile int32_t frameSequence = 0;
    volatile int32_t consumedSequence = 0;

    // -- per frame, presenter to producer --------------------------------------------
    // Published AFTER views/headPose are written.
    volatile int32_t viewSequence = 0;
    volatile int32_t viewsValid = 0;

    // Whether the runtime permits the application to submit a different field of view
    // (XrViewConfigurationProperties::fovMutable).
    //
    // PicoStreamingXR reports NO. Submitting a scaled fov to such a runtime is not an
    // override it will fight -- it is simply unsupported, and the image gets displayed
    // across the runtime's own fixed fov, i.e. stretched. We logged this field from the
    // first survey and never read it, and vrFovScale silently did nothing for days as a
    // result. 0 until the presenter has said.
    volatile int32_t fovMutable = 0;
    SharedPose       headPose;
    SharedView       views[2];
    SharedMotion     motion;

    // -- the pose this frame was RENDERED from, echoed back in the presenter's own
    // LOCAL space. Published before frameSequence.
    //
    // This is the difference between reprojection working and not working. The game
    // renders at 36 Hz and the presenter submits at 90, so by the time a frame reaches
    // the compositor it is up to 28 ms old. Submit it with the CURRENT head pose and the
    // runtime believes a stale image is current, leaves it alone, and the world ends up
    // pinned to the viewer's head on every fast turn. Submit it with the pose it was
    // drawn from and the runtime warps it to where the head actually is, which is what
    // reprojection is for.
    // PER SLOT, like the textures, and this is not a detail.
    //
    // Version 1 had a single un-versioned copy of these, written before frameSequence
    // was bumped. The consumer's seqlock could not catch that: it read sequence N, the
    // producer wrote frame N+1's poses underneath it, it re-read sequence -- still N,
    // so "consistent" -- and drew frame N's PIXELS with frame N+1's POSES. One frame
    // submitted with the wrong pose, then correct again: a blink, reported as "some
    // frame with a stale pose gets into the present queue", which is exactly right.
    //
    // Writing the payload before the counter is what made the check useless, because
    // the check can only see the counter move. Indexing these by slot removes the race
    // rather than narrowing it: the poses a consumer reads are the ones belonging to
    // the sequence it latched, by construction.
    SharedView renderedViews[kFrameSlots][2];
    int32_t    renderedViewsValid[kFrameSlots] = {};

    // How much of the eye a mono image covers, 0 to 1. Menus are drawn flat to the back
    // buffer with no world render; presented at full size they fill 104 degrees a few
    // centimetres from the face.
    float monoScale = 0.5f;

    // vrGplMenuMode 1: a mono image is not drawn into the eyes but onto a quad fixed in the
    // seat's space, at gplMenuPose (seat space), gplMenuWidthMetres x gplMenuHeightMetres, showing
    // gplMenuRect of the frame (image space, origin top left; all zero = the whole frame). Written
    // with the frame, before the sequence that advertises it. Mode 0 or no seat yet: the eyes, at
    // monoScale.
    int32_t    gplMenuMode = 0;
    SharedPose gplMenuPose;
    float      gplMenuWidthMetres = 0.0f;
    float      gplMenuHeightMetres = 0.0f;
    uint32_t   gplMenuRect[4] = {};
    // While that panel is up, the controller beam the presenter draws into the eyes: the hand
    // (kHandLeft or kHandRight, -1 for none) and how far along its aim ray it reaches, in metres
    // -- to the panel it points at (GPL's, or the VR menu's while that is open), or a fixed
    // length when it points at nothing.
    int32_t    gplMenuBeamHand = -1;
    float      gplMenuBeamLength = 0.0f;

    // vrGamma, applied by the presenter to everything it draws: the eyes and the HUD panel.
    // 1 leaves the game's colours as they are; above 1 lifts the mid-tones.
    float gamma = 1.0f;

    // Render pacing, producer to presenter: locate the views published at each latch this many
    // displays after that latch's own display, 0 to 2. 0 while the game is unpaced, which
    // keeps the views for the display being prepared; 1 while it paces, because a paced frame
    // rendered from them reaches the display after that one. The presenter echoes the value
    // it used in motion.viewLead.
    volatile int32_t viewLeadDisplays = 0;

    // -- pribluda's overlay on a quad layer, producer to presenter ------------------
    //
    // hudLayer: 0 no panel, 1 head-locked (VIEW space), 2 fixed to the seat. The panel's
    // centre sits hudRight / hudUp / hudAhead metres from the head or the seat, in OpenXR's
    // axes with ahead along -z; it faces the eye, and hudWidth is its width in metres, the
    // height following the texture's aspect.
    volatile int32_t hudLayer = 0;
    float            hudRight = 0.0f;
    float            hudUp = 0.0f;
    float            hudAhead = 0.0f;
    float            hudWidth = 0.0f;

    // The panel's pixels, in two slots written alternately like the frame textures:
    // B8G8R8A8 with coverage in alpha, stored bottom-up. hudGeneration moves when the
    // textures are recreated -- handles and size are written before it -- and hudSequence
    // when a new image is complete; the image is in slot (hudSequence % kFrameSlots).
    SharedTextureHandle hudTextures[kFrameSlots] = {};
    uint32_t            hudTextureWidth = 0;
    uint32_t            hudTextureHeight = 0;
    volatile int32_t    hudGeneration = 0;
    volatile int32_t    hudSequence = 0;

    // The seat: the head pose captured at the last recentre, in the presenter's own LOCAL
    // space, heading only -- the same origin the game renders the eyes relative to. A
    // seat-locked layer is placed in a reference space built from it. The pose is written
    // before seatGeneration is bumped, so a new generation means the pose beside it is new.
    volatile int32_t seatGeneration = 0;
    SharedPose       seatPose;

    // -- motion controllers, presenter to producer -----------------------------------
    //
    // Published AFTER `input` is complete, so a reader that checks the counter either side
    // of its copy can tell whether it caught a write in progress -- the same protocol the
    // views use, and for the same reason: the presenter writes at the display rate and the
    // game reads at the sim rate, so the overlap is rare and never zero.
    //
    // 0 means the presenter has never published a sample, which is what a runtime with no
    // controllers, or one whose action set could not be attached, leaves behind.
    volatile int32_t inputSequence = 0;
    SharedInput      input;

    // What the runtime calls each bound control, re-queried when a hand's interaction
    // profile changes and published before the generation is bumped. 0 means it has never
    // been asked, and the menu falls back to its own binding table's words.
    volatile int32_t inputNamesGeneration = 0;
    SharedInputNames inputNames;

    // -- the VR menu's own quad layer, producer to presenter -------------------------
    //
    // The same shape as the HUD panel above, and for the same reason: a quad layer is
    // composited by the runtime at the display's rate, from the layer's own texture, so
    // the menu is sharp, reprojected at display time, and -- the fault that drove this --
    // it ROTATES AS A WHOLE. Drawn into the eye buffers instead, its background was a
    // projected quad while its text was glBitmap, which is screen-axis-aligned and cannot
    // be turned; the two disagreed the moment the head rolled.
    //
    // Unlike the HUD's, the pose is published outright rather than built from three
    // scalars: the menu is put wherever the driver was looking when it opened, which is
    // not something the presenter can derive.
    SharedTextureHandle menuTextures[kFrameSlots] = {};
    uint32_t            menuTextureWidth = 0;
    uint32_t            menuTextureHeight = 0;
    volatile int32_t    menuGeneration = 0;
    volatile int32_t    menuSequence = 0;

    // The part of each slot its image fills, width then height, written with the image and
    // before the sequence that names it. The textures are made once at the largest size any
    // user of the panel draws (the VR menu, its tweak strips, the radial quick menus, the
    // replay panel), so a smaller image fills GL's origin: rows 0..h-1 of the bottom-up
    // texture, which is the bottom-left w x h of the image once it is turned the right way up.
    uint32_t menuImageSize[kFrameSlots][2] = {};

    // Non-zero while the layer is to be shown. Written last, so a menu coming back is
    // never shown for a display frame carrying the image from before it closed.
    volatile int32_t menuVisible = 0;

    // Where the quad hangs, and in which of the presenter's spaces.
    //
    // Usually the seat's -- the same space the seat-locked HUD panel uses, which the
    // presenter already builds from the published seat pose, and the frame the game
    // places its panel in. The exception is HUD tweak mode with a HEAD-LOCKED HUD panel
    // (vrHudLayer 1): the menu's quad is put exactly over that panel to annotate it, and
    // a panel the runtime re-places against the head every display frame cannot be
    // followed by a layer nailed to the seat from a pose computed a frame earlier. It
    // has to be in the same space as the thing it is drawn on.
    volatile int32_t menuSpace = kMenuSpaceSeat;
    SharedPose       menuPose;
    float            menuWidthMetres = 0.0f;
    float            menuHeightMetres = 0.0f;

    // -- counters, for the log and nothing else --------------------------------------
    volatile int32_t producedFrames = 0;
    volatile int32_t presentedFrames = 0;
    volatile int32_t presenterError = 0;
};

static_assert(std::is_standard_layout_v<ControlBlock>,
              "the control block is mapped into two processes; it must be POD-shaped");
static_assert(sizeof(SharedPose) == 28, "SharedPose must not gain padding");
static_assert(sizeof(SharedView) == 44, "SharedView must not gain padding");
static_assert(sizeof(SharedMotion) == 60, "SharedMotion must not gain padding");
static_assert(sizeof(SharedHand) == 80, "SharedHand must not gain padding");
static_assert(sizeof(SharedInput) == 176, "SharedInput must not gain padding");
static_assert(sizeof(SharedInputNames) == 2 * 9 * 48,
              "SharedInputNames is plain chars and must not gain padding");
static_assert(sizeof(ControlBlock) < 4096, "one page is plenty and keeps mapping simple");

// The mapping name is derived from an instance string -- the game's process id -- so two
// copies of the game cannot find each other's frames.
//
// No event object to go with it. The presenter runs its own 90 Hz loop and always wants
// the newest frame the producer has finished, never the next one; waiting on an event
// would only let a 36 Hz producer pace a 90 Hz compositor. So it polls frameSequence,
// which is the correct relationship between the two rates as well as the simpler one.
inline void makeMappingName(wchar_t (&out)[128], const wchar_t* instance) {
    _snwprintf_s(out, _TRUNCATE, L"Local\\gplvr-%s-block", instance);
}

}  // namespace gplvr
