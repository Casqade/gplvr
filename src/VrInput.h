#pragma once

// The motion controllers, as the game sees them.
//
// The presenter samples OpenXR and publishes SharedInput into the control block; this is
// the other end of that wire. It does three things and nothing else:
//
//   * freshness. The presenter publishes at the display rate and the game polls at its
//     own, so the same sample is read several times over. Edges are computed only when
//     the published counter moves, or every press would fire once per poll rather than
//     once per press. A sample that stops arriving goes stale and every hand goes away,
//     which is not the same as every button being released -- see `present`.
//
//   * conditioning. A radial deadzone on the stick, rescaled so the usable range still
//     starts at zero; a Schmitt trigger on each analogue pull so a trembling finger at
//     the threshold does not chatter; rising and falling edges on everything.
//
//   * the seat frame. The published poses are in the presenter's LOCAL space and OpenXR's
//     axes. vrSeatRelative (VrCamera) converts them exactly as an eye is converted, so a
//     hand position is in the same frame and the same units as the cockpit.
//
// OWNERSHIP. The two controllers are interchangeable and offer the same menu, but only
// one drives it at a time: whichever hand opened the menu or entered a tweak mode holds
// it until it exits. Everything that reacts to a control asks vrInputOwner() first, so
// there is exactly one place that decides and no chance of two hands dragging the same
// element in opposite directions.

#include "presenter/SharedPresentation.h"

#include <cstddef>

namespace gplvr {

// A button, with both edges of the poll it was seen in. `pressed` and `released` are true
// for exactly one poll -- the one whose sample carried the change.
struct VrEdge {
    bool down = false;
    bool pressed = false;
    bool released = false;
};

struct VrHandInput {
    // The hand is tracked well enough to point with: an aim pose that is active, valid and
    // actually tracked. False for a controller that is asleep, out of the play space, or
    // simply not there.
    bool present = false;
    // The runtime has bound an interaction profile to this hand. present false with this
    // true is a controller the runtime knows about but cannot locate.
    bool profileBound = false;

    // As published: presenter LOCAL space, OpenXR axes, metres.
    SharedPose aimPose;
    SharedPose gripPose;

    // The same poses in the seat's frame and GPL's axes, in GPL units. Meaningful only
    // while `seatValid`, which needs a recentre to have happened.
    bool  seatValid = false;
    float aimPosition[3] = {0.0f, 0.0f, 0.0f};
    float aimForward[3] = {1.0f, 0.0f, 0.0f};
    float gripPosition[3] = {0.0f, 0.0f, 0.0f};

    // 0 to 1, as the runtime reported them.
    float trigger = 0.0f;
    float squeeze = 0.0f;
    // -1 to 1 with the deadzone removed and the remainder rescaled, so the first
    // perceptible movement of the stick is the first non-zero value here.
    float stickX = 0.0f;
    float stickY = 0.0f;

    // The analogue pulls as buttons, through a Schmitt trigger.
    VrEdge triggerHeld;
    VrEdge squeezeHeld;
    // The digital controls, as the runtime reported them.
    VrEdge stick;
    VrEdge primary;
    VrEdge secondary;
    VrEdge menu;
};

// Once a frame, from the key poll on the render thread. Everything below reads what this
// left behind, so nothing else ever touches the control block.
void vrInputPoll();

// A fresh, focused sample arrived within the staleness window. False means every hand is
// absent and no edge in this poll is real.
bool vrInputAvailable();

// Seconds since the previous poll, clamped to a tenth of a second so a stall cannot turn
// one poll into a huge rate-driven step.
float vrInputSeconds();

// True when this poll carried a sample the previous one had not seen. A rate-driven axis
// integrates over vrInputSeconds regardless; an edge is only ever real on such a poll.
bool vrInputFresh();

const VrHandInput& vrInputHand(int hand);

// Which hand owns the interactive surface, or -1 while nobody does.
int vrInputOwner();

// Take ownership for `hand`. Refused, with a log line, while the other hand holds it --
// which is what keeps two controllers from fighting over one slider. Granting it to the
// hand that already has it succeeds and says nothing.
bool vrInputClaim(int hand, const char* why);

// Give it back. Safe to call when nobody holds it.
void vrInputRelease(const char* why);

// The once-a-second line: what arrived, what was refused and why. False when the channel
// has never published, so a session with no headset prints nothing.
bool vrInputFormatStatus(char* out, size_t size);

}  // namespace gplvr
