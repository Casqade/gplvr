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
//                                    <--  adapter LUID + recommended eye size
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
inline constexpr uint32_t kControlVersion = 4;  // 4: added eyeRect

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
    uint32_t recommendedEyeWidth = 0;
    uint32_t recommendedEyeHeight = 0;

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

    // -- counters, for the log and nothing else --------------------------------------
    volatile int32_t producedFrames = 0;
    volatile int32_t presentedFrames = 0;
    volatile int32_t presenterError = 0;
};

static_assert(std::is_standard_layout_v<ControlBlock>,
              "the control block is mapped into two processes; it must be POD-shaped");
static_assert(sizeof(SharedPose) == 28, "SharedPose must not gain padding");
static_assert(sizeof(SharedView) == 44, "SharedView must not gain padding");
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
