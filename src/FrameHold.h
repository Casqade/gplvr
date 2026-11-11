#pragma once

// Race frames GPL presents without rendering the world.
//
// Such a frame still holds the last stereo pair in the off-screen buffer. Published as one image
// it put the whole side-by-side buffer in each eye, which reads as double vision; the present hook
// publishes nothing for it while the pair is intact, and the headset keeps the last frame with its
// own pose. This module counts the runs of such frames.

namespace gplvr {

// What the present hook did with one race frame in VR.
enum class FrameOutcome {
    Rendered,      // a stereo world was rendered and published as a pair
    HeldNoRender,  // no world render, the pair intact: nothing published
    Other,         // anything else: a menu, GPL's static, a mono render
};

// Once per published-or-held frame. A run of held frames is logged when it ends.
void frameHoldNoteOutcome(FrameOutcome outcome);

}  // namespace gplvr
