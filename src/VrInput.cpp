#include "VrInput.h"

#include "Hooks.h"
#include "Log.h"
#include "VrCamera.h"
#include "presenter/Producer.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

namespace gplvr {
namespace {

// A sample older than this is not input any more. The presenter publishes at the display
// rate, so even a 72 Hz headset with a frame missed here and there stays far inside it;
// reaching it means the presenter stopped, and holding the last stick deflection then
// would leave a mirror turning by itself.
constexpr double kStaleSeconds = 0.25;

// The most one poll may be worth to a rate-driven axis. The same number and the same
// reason as mirror tweak mode's kMirrorTweakMaxStepSeconds: a stall must not become one
// enormous step.
constexpr float kMaxStepSeconds = 0.1f;

// Below this the stick is at rest. Measured on nothing -- it is the value BFVR's
// ControllerInputOverlay settled on for the same hardware class, and it is the first
// thing to change if a drifting stick moves a mirror on its own.
constexpr float kStickDeadzone = 0.20f;

// The analogue pulls as buttons. Two thresholds, not one: a finger resting at the
// threshold otherwise chatters at the poll rate, and a chattering trigger in a tweak mode
// means an element selected and deselected several times a frame.
constexpr float kAnaloguePress = 0.60f;
constexpr float kAnalogueRelease = 0.45f;

// Why a poll produced nothing. Counted rather than logged per frame, and printed in the
// once-a-second line: a silently inert input path reads exactly like a controller that
// is not being pressed.
enum Refusal {
    kRefusalNone = 0,
    kRefusalNoChannel,   // the presenter is not publishing
    kRefusalNoSample,    // it is, but nothing has been published yet
    kRefusalUnfocused,   // another application has the runtime's input focus
    kRefusalStale,       // the newest sample is older than kStaleSeconds
    kRefusalCount,
};

const char* refusalName(int refusal) {
    switch (refusal) {
        case kRefusalNoChannel: return "no presenter channel";
        case kRefusalNoSample: return "nothing published yet";
        case kRefusalUnfocused: return "the runtime gave input focus to another application";
        case kRefusalStale: return "the newest sample is stale";
        default: return "none";
    }
}

struct State {
    VrHandInput hands[kHands];

    bool     available = false;
    bool     fresh = false;
    float    seconds = 0.0f;
    int32_t  lastSequence = 0;
    LONGLONG frequency = 0;
    LONGLONG lastPoll = 0;

    int owner = -1;

    // The edge detector's memory. Kept out of VrHandInput so a consumer cannot confuse
    // "was down last sample" with "is down now".
    bool wasDown[kHands][6] = {};

    int  lastRefusal = kRefusalNone;
    bool loggedFirstSample = false;
    bool loggedSeatMissing = false;

    // Once-a-second arithmetic.
    unsigned polls = 0;
    unsigned freshPolls = 0;
    unsigned refusals[kRefusalCount] = {};
    unsigned handPresent[kHands] = {};
};

State g_state;

// Indices into State::wasDown, in the order the edges are updated below.
enum { kEdgeTrigger = 0, kEdgeSqueeze, kEdgeStick, kEdgePrimary, kEdgeSecondary, kEdgeMenu };

LONGLONG qpcNow() {
    if (!g_state.frequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_state.frequency = frequency.QuadPart;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// The edge, and the memory that produces it. `fresh` false means this poll is looking at
// a sample it has already seen, so whatever edge there was has been delivered and must
// not be delivered again.
void updateEdge(VrEdge& edge, bool down, bool& was, bool fresh) {
    edge.down = down;
    edge.pressed = fresh && down && !was;
    edge.released = fresh && !down && was;
    if (fresh) was = down;
}

// A Schmitt trigger: it takes kAnaloguePress to go down and dropping below
// kAnalogueRelease to come back up.
bool analogueHeld(float value, bool was) {
    return was ? value > kAnalogueRelease : value >= kAnaloguePress;
}

// The radial deadzone. Rescaled rather than clipped: clipping leaves a step at the edge of
// the zone, and a step in a rate axis is a mirror that jumps as soon as it starts moving.
void applyDeadzone(float x, float y, float& outX, float& outY) {
    const float magnitude = std::sqrt(x * x + y * y);
    if (!(magnitude > kStickDeadzone)) {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }
    const float scale = (magnitude - kStickDeadzone) / ((1.0f - kStickDeadzone) * magnitude);
    outX = x * scale;
    outY = y * scale;
    if (outX < -1.0f) outX = -1.0f;
    if (outX > 1.0f) outX = 1.0f;
    if (outY < -1.0f) outY = -1.0f;
    if (outY > 1.0f) outY = 1.0f;
}

void clearHands() {
    for (int hand = 0; hand < kHands; ++hand) {
        g_state.hands[hand] = VrHandInput{};
        // The edge memory goes with them, so the first sample after a gap cannot deliver a
        // release for a button that was down when the channel stopped.
        for (bool& was : g_state.wasDown[hand]) was = false;
    }
    if (g_state.owner >= 0) {
        logf("vr input: the %s hand lost the interactive surface -- there is no input any "
             "more",
             g_state.owner == kHandLeft ? "left" : "right");
        g_state.owner = -1;
    }
}

void noteRefusal(int refusal) {
    ++g_state.refusals[refusal];
    if (refusal != g_state.lastRefusal) {
        g_state.lastRefusal = refusal;
        if (refusal != kRefusalNone)
            logf("vr input: no controller state -- %s", refusalName(refusal));
    }
}

}  // namespace

void vrInputPoll() {
    State& state = g_state;

    LONGLONG now = qpcNow();
    double   elapsed = state.lastPoll ? static_cast<double>(now - state.lastPoll) /
                                          static_cast<double>(state.frequency)
                                    : 0.0;
    state.lastPoll = now;
    if (elapsed > kMaxStepSeconds) elapsed = kMaxStepSeconds;
    state.seconds = static_cast<float>(elapsed);
    state.fresh = false;
    ++state.polls;

    if (!g_options.vr) {
        state.available = false;
        clearHands();
        return;
    }

    SharedInput sample;
    int32_t     sequence = 0;
    if (!presenterLatestInput(sample, sequence)) {
        state.available = false;
        clearHands();
        noteRefusal(kRefusalNoChannel);
        return;
    }

    if (!(sample.flags & kInputFocused)) {
        state.available = false;
        clearHands();
        noteRefusal(kRefusalUnfocused);
        return;
    }

    const int64_t sampled = loadQpc(sample.sampleTime);
    if (!sampled) {
        state.available = false;
        clearHands();
        noteRefusal(kRefusalNoSample);
        return;
    }
    const double age =
        static_cast<double>(now - sampled) / static_cast<double>(state.frequency);
    if (age > kStaleSeconds || age < -kStaleSeconds) {
        state.available = false;
        clearHands();
        noteRefusal(kRefusalStale);
        return;
    }

    state.fresh = sequence != state.lastSequence;
    state.lastSequence = sequence;
    state.available = true;
    noteRefusal(kRefusalNone);
    if (state.fresh) ++state.freshPolls;

    if (!state.loggedFirstSample) {
        state.loggedFirstSample = true;
        logf("vr input: the first controller sample arrived -- left flags 0x%04X buttons "
             "0x%X, right flags 0x%04X buttons 0x%X",
             static_cast<unsigned>(sample.hands[kHandLeft].flags),
             static_cast<unsigned>(sample.hands[kHandLeft].buttons),
             static_cast<unsigned>(sample.hands[kHandRight].flags),
             static_cast<unsigned>(sample.hands[kHandRight].buttons));
    }

    bool anySeat = false;
    for (int hand = 0; hand < kHands; ++hand) {
        const SharedHand& in = sample.hands[hand];
        VrHandInput&      out = state.hands[hand];

        out.profileBound = (in.flags & kHandProfileBound) != 0;
        // Pointing needs all four: an active action, a located pose, and the runtime
        // actually following the device rather than replaying its last known place.
        out.present = (in.flags & kHandAimActive) && (in.flags & kHandAimPositionValid) &&
                      (in.flags & kHandAimOrientationValid) &&
                      (in.flags & kHandAimOrientationTracked);
        out.aimPose = in.aimPose;
        out.gripPose = in.gripPose;

        out.trigger = in.trigger;
        out.squeeze = in.squeeze;
        applyDeadzone(in.stickX, in.stickY, out.stickX, out.stickY);

        bool* was = state.wasDown[hand];
        updateEdge(out.triggerHeld, analogueHeld(in.trigger, was[kEdgeTrigger]),
                   was[kEdgeTrigger], state.fresh);
        updateEdge(out.squeezeHeld, analogueHeld(in.squeeze, was[kEdgeSqueeze]),
                   was[kEdgeSqueeze], state.fresh);
        updateEdge(out.stick, (in.buttons & kButtonStick) != 0, was[kEdgeStick], state.fresh);
        updateEdge(out.primary, (in.buttons & kButtonPrimary) != 0, was[kEdgePrimary],
                   state.fresh);
        updateEdge(out.secondary, (in.buttons & kButtonSecondary) != 0, was[kEdgeSecondary],
                   state.fresh);
        updateEdge(out.menu, (in.buttons & kButtonMenu) != 0, was[kEdgeMenu], state.fresh);

        // The seat frame. Fails until the first recentre, which is why nothing that needs
        // a cockpit-space hand may assume it is there.
        out.seatValid = false;
        if (out.present && vrSeatRelative(in.aimPose, out.aimPosition, out.aimForward)) {
            out.seatValid = true;
            anySeat = true;
            if (in.flags & kHandGripPositionValid)
                vrSeatRelative(in.gripPose, out.gripPosition, nullptr);
        }

        if (out.present) ++state.handPresent[hand];
    }

    if (!anySeat && !state.loggedSeatMissing &&
        (state.hands[kHandLeft].present || state.hands[kHandRight].present)) {
        state.loggedSeatMissing = true;
        logf("vr input: a controller is tracked but there is no seat yet, so its position "
             "in the cockpit is unknown -- recentre (vrRecentreKey) and it appears");
    }

    // A hand that goes away cannot keep the surface it claimed, or the menu would be stuck
    // open against a controller that has been put down.
    if (state.owner >= 0 && !state.hands[state.owner].present) {
        logf("vr input: the %s hand stopped being tracked and gave up the interactive "
             "surface",
             state.owner == kHandLeft ? "left" : "right");
        state.owner = -1;
    }
}

bool vrInputAvailable() { return g_state.available; }

float vrInputSeconds() { return g_state.seconds; }

bool vrInputFresh() { return g_state.fresh; }

const VrHandInput& vrInputHand(int hand) {
    static const VrHandInput kAbsent;
    if (hand < 0 || hand >= kHands) return kAbsent;
    return g_state.hands[hand];
}

int vrInputOwner() { return g_state.owner; }

bool vrInputClaim(int hand, const char* why) {
    if (hand < 0 || hand >= kHands) return false;
    if (g_state.owner == hand) return true;
    if (g_state.owner >= 0) {
        logf("vr input: the %s hand asked for %s and was refused -- the %s hand still has "
             "it",
             hand == kHandLeft ? "left" : "right", why ? why : "the interactive surface",
             g_state.owner == kHandLeft ? "left" : "right");
        return false;
    }
    if (!g_state.hands[hand].present) {
        logf("vr input: the %s hand asked for %s and was refused -- it is not tracked",
             hand == kHandLeft ? "left" : "right", why ? why : "the interactive surface");
        return false;
    }
    g_state.owner = hand;
    logf("vr input: the %s hand now has %s", hand == kHandLeft ? "left" : "right",
         why ? why : "the interactive surface");
    return true;
}

void vrInputRelease(const char* why) {
    if (g_state.owner < 0) return;
    logf("vr input: the %s hand let go of %s",
         g_state.owner == kHandLeft ? "left" : "right",
         why ? why : "the interactive surface");
    g_state.owner = -1;
}

bool vrInputFormatStatus(char* out, size_t size) {
    State& state = g_state;
    if (!state.loggedFirstSample && !state.polls) return false;
    if (!state.lastSequence && !state.refusals[kRefusalNoChannel]) return false;

    unsigned refused = 0;
    int      worst = 1;
    for (int i = 1; i < kRefusalCount; ++i) {
        refused += state.refusals[i];
        if (state.refusals[i] > state.refusals[worst]) worst = i;
    }

    _snprintf_s(out, size, _TRUNCATE,
                "vr input: %u polls, %u carried a new sample; left tracked %u, right "
                "tracked %u; %u refused%s%s | owner %s",
                state.polls, state.freshPolls, state.handPresent[kHandLeft],
                state.handPresent[kHandRight], refused, refused ? " -- mostly " : "",
                refused ? refusalName(worst) : "",
                state.owner < 0 ? "none"
                                : (state.owner == kHandLeft ? "left" : "right"));

    state.polls = 0;
    state.freshPolls = 0;
    for (unsigned& count : state.refusals) count = 0;
    for (unsigned& count : state.handPresent) count = 0;
    return true;
}

}  // namespace gplvr
