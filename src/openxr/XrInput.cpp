#include "openxr/XrInput.h"

#include "Log.h"

#include <cmath>
#include <cstring>

namespace gplvr {
namespace {

// The component path of one control on one hand, as a suffix: the full path is
// "/user/hand/" + the hand + this. Storing the suffix keeps every row half the width and
// makes a left/right pair obvious at a glance.
struct BindingSeed {
    int         action;
    int         hand;
    const char* component;
};

struct ProfileBindings {
    const char*        profile;
    const BindingSeed* seeds;
    int                count;
};

// Oculus Touch: the left controller carries X and Y, the right A and B, and only the left
// has a menu click -- the right's equivalent is the system button, which no application
// may bind.
const BindingSeed kTouchSeeds[] = {
    {kXrActionAimPose, kHandLeft, "/input/aim/pose"},
    {kXrActionGripPose, kHandLeft, "/input/grip/pose"},
    {kXrActionTrigger, kHandLeft, "/input/trigger/value"},
    {kXrActionSqueeze, kHandLeft, "/input/squeeze/value"},
    {kXrActionStick, kHandLeft, "/input/thumbstick"},
    {kXrActionStickClick, kHandLeft, "/input/thumbstick/click"},
    {kXrActionPrimary, kHandLeft, "/input/x/click"},
    {kXrActionSecondary, kHandLeft, "/input/y/click"},
    {kXrActionMenu, kHandLeft, "/input/menu/click"},
    {kXrActionAimPose, kHandRight, "/input/aim/pose"},
    {kXrActionGripPose, kHandRight, "/input/grip/pose"},
    {kXrActionTrigger, kHandRight, "/input/trigger/value"},
    {kXrActionSqueeze, kHandRight, "/input/squeeze/value"},
    {kXrActionStick, kHandRight, "/input/thumbstick"},
    {kXrActionStickClick, kHandRight, "/input/thumbstick/click"},
    {kXrActionPrimary, kHandRight, "/input/a/click"},
    {kXrActionSecondary, kHandRight, "/input/b/click"},
};

// Valve Index: A and B on both hands, and no bindable menu at all -- the system button is
// the runtime's. A menu action left unbound reports inactive, which is exactly right.
const BindingSeed kIndexSeeds[] = {
    {kXrActionAimPose, kHandLeft, "/input/aim/pose"},
    {kXrActionGripPose, kHandLeft, "/input/grip/pose"},
    {kXrActionTrigger, kHandLeft, "/input/trigger/value"},
    {kXrActionSqueeze, kHandLeft, "/input/squeeze/value"},
    {kXrActionStick, kHandLeft, "/input/thumbstick"},
    {kXrActionStickClick, kHandLeft, "/input/thumbstick/click"},
    {kXrActionPrimary, kHandLeft, "/input/a/click"},
    {kXrActionSecondary, kHandLeft, "/input/b/click"},
    {kXrActionAimPose, kHandRight, "/input/aim/pose"},
    {kXrActionGripPose, kHandRight, "/input/grip/pose"},
    {kXrActionTrigger, kHandRight, "/input/trigger/value"},
    {kXrActionSqueeze, kHandRight, "/input/squeeze/value"},
    {kXrActionStick, kHandRight, "/input/thumbstick"},
    {kXrActionStickClick, kHandRight, "/input/thumbstick/click"},
    {kXrActionPrimary, kHandRight, "/input/a/click"},
    {kXrActionSecondary, kHandRight, "/input/b/click"},
};

// HTC Vive wand: a trackpad where the stick is, and a squeeze that is a click rather than
// an analogue pull. Binding a boolean source to a float action is defined by the runtime
// as 0 or 1, which is what the deadzones downstream expect anyway.
const BindingSeed kViveSeeds[] = {
    {kXrActionAimPose, kHandLeft, "/input/aim/pose"},
    {kXrActionGripPose, kHandLeft, "/input/grip/pose"},
    {kXrActionTrigger, kHandLeft, "/input/trigger/value"},
    {kXrActionSqueeze, kHandLeft, "/input/squeeze/click"},
    {kXrActionStick, kHandLeft, "/input/trackpad"},
    {kXrActionStickClick, kHandLeft, "/input/trackpad/click"},
    {kXrActionMenu, kHandLeft, "/input/menu/click"},
    {kXrActionAimPose, kHandRight, "/input/aim/pose"},
    {kXrActionGripPose, kHandRight, "/input/grip/pose"},
    {kXrActionTrigger, kHandRight, "/input/trigger/value"},
    {kXrActionSqueeze, kHandRight, "/input/squeeze/click"},
    {kXrActionStick, kHandRight, "/input/trackpad"},
    {kXrActionStickClick, kHandRight, "/input/trackpad/click"},
    {kXrActionMenu, kHandRight, "/input/menu/click"},
};

// Windows Mixed Reality: a stick and a trackpad, so the trackpad click stands in for the
// face button neither hand has.
const BindingSeed kWmrSeeds[] = {
    {kXrActionAimPose, kHandLeft, "/input/aim/pose"},
    {kXrActionGripPose, kHandLeft, "/input/grip/pose"},
    {kXrActionTrigger, kHandLeft, "/input/trigger/value"},
    {kXrActionSqueeze, kHandLeft, "/input/squeeze/click"},
    {kXrActionStick, kHandLeft, "/input/thumbstick"},
    {kXrActionStickClick, kHandLeft, "/input/thumbstick/click"},
    {kXrActionPrimary, kHandLeft, "/input/trackpad/click"},
    {kXrActionMenu, kHandLeft, "/input/menu/click"},
    {kXrActionAimPose, kHandRight, "/input/aim/pose"},
    {kXrActionGripPose, kHandRight, "/input/grip/pose"},
    {kXrActionTrigger, kHandRight, "/input/trigger/value"},
    {kXrActionSqueeze, kHandRight, "/input/squeeze/click"},
    {kXrActionStick, kHandRight, "/input/thumbstick"},
    {kXrActionStickClick, kHandRight, "/input/thumbstick/click"},
    {kXrActionPrimary, kHandRight, "/input/trackpad/click"},
    {kXrActionMenu, kHandRight, "/input/menu/click"},
};

// The Khronos simple controller is the fallback every runtime must support: a pose, one
// select click and one menu click. Everything else reports inactive, which the game reads
// as "this control does not exist on this hardware" rather than as "it is not pressed".
const BindingSeed kSimpleSeeds[] = {
    {kXrActionAimPose, kHandLeft, "/input/aim/pose"},
    {kXrActionGripPose, kHandLeft, "/input/grip/pose"},
    {kXrActionPrimary, kHandLeft, "/input/select/click"},
    {kXrActionMenu, kHandLeft, "/input/menu/click"},
    {kXrActionAimPose, kHandRight, "/input/aim/pose"},
    {kXrActionGripPose, kHandRight, "/input/grip/pose"},
    {kXrActionPrimary, kHandRight, "/input/select/click"},
    {kXrActionMenu, kHandRight, "/input/menu/click"},
};

#define GPLVR_SEEDS(array) array, static_cast<int>(sizeof(array) / sizeof(array[0]))

const ProfileBindings kProfiles[] = {
    {"/interaction_profiles/oculus/touch_controller", GPLVR_SEEDS(kTouchSeeds)},
    {"/interaction_profiles/valve/index_controller", GPLVR_SEEDS(kIndexSeeds)},
    {"/interaction_profiles/htc/vive_controller", GPLVR_SEEDS(kViveSeeds)},
    {"/interaction_profiles/microsoft/motion_controller", GPLVR_SEEDS(kWmrSeeds)},
    {"/interaction_profiles/khr/simple_controller", GPLVR_SEEDS(kSimpleSeeds)},
};

#undef GPLVR_SEEDS

constexpr int kProfileCount = static_cast<int>(sizeof(kProfiles) / sizeof(kProfiles[0]));

struct ActionSpec {
    const char*  name;       // lower case: the spec forbids capitals in an action name
    const char*  localized;
    XrActionType type;
};

// Indexed by XrInputAction, in that enum's order. The static_assert below is what keeps
// the two from drifting: a new action added to the enum without a row here is a build
// error rather than a control that silently never fires.
const ActionSpec kActions[] = {
    {"aim_pose", "Controller aim", XR_ACTION_TYPE_POSE_INPUT},
    {"grip_pose", "Controller grip", XR_ACTION_TYPE_POSE_INPUT},
    {"trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT},
    {"squeeze", "Grip squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
    {"stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {"stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"primary", "Primary face button", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"secondary", "Secondary face button", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"menu", "Menu button", XR_ACTION_TYPE_BOOLEAN_INPUT},
};

static_assert(sizeof(kActions) / sizeof(kActions[0]) ==
                  static_cast<unsigned>(kXrActionCount),
              "every XrInputAction needs a name and a type");
static_assert(static_cast<unsigned>(kXrActionCount) == kInputActions,
              "SharedInputNames has one row per action; the two counts must agree or the "
              "game reads a different control's name than the one it asked for");
// And in the same ORDER, which is the part a count cannot catch.
static_assert(kXrActionAimPose == kInputAimPose && kXrActionGripPose == kInputGripPose &&
                  kXrActionTrigger == kInputTrigger &&
                  kXrActionSqueeze == kInputSqueeze && kXrActionStick == kInputStick &&
                  kXrActionStickClick == kInputStickClick &&
                  kXrActionPrimary == kInputPrimary &&
                  kXrActionSecondary == kInputSecondary && kXrActionMenu == kInputMenu,
              "XrInputAction and SharedInputAction must index the same rows");

const char* handName(int hand) { return hand == kHandLeft ? "left" : "right"; }

float finiteOrZero(float value) { return std::isfinite(value) ? value : 0.0f; }

float clampUnit(float value) {
    if (!std::isfinite(value)) return 0.0f;
    if (value < 0.0f) return 0.0f;
    return value > 1.0f ? 1.0f : value;
}

float clampSigned(float value) {
    if (!std::isfinite(value)) return 0.0f;
    if (value < -1.0f) return -1.0f;
    return value > 1.0f ? 1.0f : value;
}

}  // namespace

bool XrInput::create(const XrApi& api, XrInstance instance, XrSession session) {
    // Every entry point is resolved from the loader by name, so a runtime missing one is a
    // null pointer here rather than a link failure. Checked as a group: partial input is
    // worse than none, because half the controls working reads as a hardware fault.
    if (!api.xrStringToPath || !api.xrCreateActionSet || !api.xrCreateAction ||
        !api.xrSuggestInteractionProfileBindings || !api.xrAttachSessionActionSets ||
        !api.xrCreateActionSpace || !api.xrSyncActions || !api.xrGetActionStatePose ||
        !api.xrGetActionStateFloat || !api.xrGetActionStateVector2f ||
        !api.xrGetActionStateBoolean) {
        logf("xr input: the runtime did not provide the OpenXR action entry points -- "
             "there are no motion controllers this session");
        return false;
    }

    for (int hand = 0; hand < kHands; ++hand) {
        char path[32] = {};
        _snprintf_s(path, sizeof(path), _TRUNCATE, "/user/hand/%s", handName(hand));
        XrResult result = api.xrStringToPath(instance, path, &handPaths_[hand]);
        if (XR_FAILED(result)) {
            logf("xr input: %s could not be resolved to a path (%s) -- no controllers", path,
                 xrResultName(api, instance, result));
            return false;
        }
    }

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "gplvr_controls");
    strcpy_s(setInfo.localizedActionSetName, "GPLVR controls");
    setInfo.priority = 0;
    XrResult result = api.xrCreateActionSet(instance, &setInfo, &set_);
    if (XR_FAILED(result)) {
        logf("xr input: xrCreateActionSet refused (%s) -- no controllers",
             xrResultName(api, instance, result));
        return false;
    }

    for (int i = 0; i < kXrActionCount; ++i) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(info.actionName, kActions[i].name);
        strcpy_s(info.localizedActionName, kActions[i].localized);
        info.actionType = kActions[i].type;
        // Both hands on every action. The subaction path is what picks one of them apart
        // again at xrGetActionState* time.
        info.countSubactionPaths = static_cast<uint32_t>(kHands);
        info.subactionPaths = handPaths_;
        result = api.xrCreateAction(set_, &info, &actions_[i]);
        if (XR_FAILED(result)) {
            logf("xr input: action '%s' could not be created (%s) -- no controllers",
                 kActions[i].name, xrResultName(api, instance, result));
            destroy(api);
            return false;
        }
    }

    // Suggested bindings, profile by profile. The runtime keeps the one that matches the
    // hardware and ignores the rest, so suggesting a profile for a headset that is not
    // plugged in costs nothing.
    int profilesAccepted = 0;
    for (int p = 0; p < kProfileCount; ++p) {
        XrPath profilePath = XR_NULL_PATH;
        result = api.xrStringToPath(instance, kProfiles[p].profile, &profilePath);
        if (XR_FAILED(result)) {
            logf("xr input: the runtime does not know the profile %s (%s) -- skipped",
                 kProfiles[p].profile, xrResultName(api, instance, result));
            continue;
        }

        // At most one binding per seed; the array is sized for the largest table so a
        // profile gaining rows cannot overrun it.
        XrActionSuggestedBinding bindings[64] = {};
        uint32_t                 count = 0;
        int                      skipped = 0;
        for (int s = 0; s < kProfiles[p].count && count < 64; ++s) {
            const BindingSeed& seed = kProfiles[p].seeds[s];
            char               full[96] = {};
            _snprintf_s(full, sizeof(full), _TRUNCATE, "/user/hand/%s%s",
                        handName(seed.hand), seed.component);
            XrPath bindingPath = XR_NULL_PATH;
            if (XR_FAILED(api.xrStringToPath(instance, full, &bindingPath))) {
                ++skipped;
                continue;
            }
            bindings[count].action = actions_[seed.action];
            bindings[count].binding = bindingPath;
            ++count;
        }

        XrInteractionProfileSuggestedBinding suggestion{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestion.interactionProfile = profilePath;
        suggestion.countSuggestedBindings = count;
        suggestion.suggestedBindings = bindings;
        result = api.xrSuggestInteractionProfileBindings(instance, &suggestion);
        if (XR_FAILED(result)) {
            logf("xr input: %s refused its %u bindings (%s) -- that controller will have "
                 "none; the others are unaffected",
                 kProfiles[p].profile, count, xrResultName(api, instance, result));
            continue;
        }
        ++profilesAccepted;
        if (skipped)
            logf("xr input: %s bound %u controls, %d component paths the runtime does not "
                 "know were skipped",
                 kProfiles[p].profile, count, skipped);
    }

    if (!profilesAccepted) {
        logf("xr input: not one interaction profile was accepted -- no controllers");
        destroy(api);
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &set_;
    result = api.xrAttachSessionActionSets(session, &attach);
    if (XR_FAILED(result)) {
        logf("xr input: the action set could not be attached to the session (%s) -- no "
             "controllers",
             xrResultName(api, instance, result));
        destroy(api);
        return false;
    }

    // The pose spaces, one per hand per pose. A located space is how a pose action becomes
    // a position and an orientation; the action state itself only says whether it is live.
    for (int hand = 0; hand < kHands; ++hand) {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.subactionPath = handPaths_[hand];
        info.poseInActionSpace.orientation.w = 1.0f;  // no offset from the action's own pose

        info.action = actions_[kXrActionAimPose];
        if (XR_FAILED(api.xrCreateActionSpace(session, &info, &aimSpaces_[hand])))
            logf("xr input: the %s aim space could not be created -- that hand cannot point",
                 handName(hand));

        info.action = actions_[kXrActionGripPose];
        if (XR_FAILED(api.xrCreateActionSpace(session, &info, &gripSpaces_[hand])))
            logf("xr input: the %s grip space could not be created -- that hand has no hold "
                 "pose",
                 handName(hand));
    }

    attached_ = true;
    logf("xr input: %d interaction profile(s) accepted, %d actions attached -- motion "
         "controllers are live",
         profilesAccepted, static_cast<int>(kXrActionCount));
    return true;
}

void XrInput::destroy(const XrApi& api) {
    for (int hand = 0; hand < kHands; ++hand) {
        if (aimSpaces_[hand] && api.xrDestroySpace) api.xrDestroySpace(aimSpaces_[hand]);
        if (gripSpaces_[hand] && api.xrDestroySpace) api.xrDestroySpace(gripSpaces_[hand]);
        aimSpaces_[hand] = XR_NULL_HANDLE;
        gripSpaces_[hand] = XR_NULL_HANDLE;
    }
    for (int i = 0; i < kXrActionCount; ++i) {
        if (actions_[i] && api.xrDestroyAction) api.xrDestroyAction(actions_[i]);
        actions_[i] = XR_NULL_HANDLE;
    }
    if (set_ && api.xrDestroyActionSet) api.xrDestroyActionSet(set_);
    set_ = XR_NULL_HANDLE;
    attached_ = false;
}

void XrInput::sample(const XrApi& api, XrInstance instance, XrSession session, XrSpace space,
                     XrTime displayTime, bool focused, SharedInput& out) {
    out = SharedInput{};
    if (!attached_) return;
    if (!focused) {
        // Not a failure and not worth a log line a frame: OpenXR reports every action
        // inactive while another application has focus, so the zeroed sample above IS the
        // truth. kInputFocused stays clear, and the game holds whatever it was doing.
        return;
    }
    out.flags |= kInputFocused;

    XrActiveActionSet active{};
    active.actionSet = set_;
    active.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    XrResult result = api.xrSyncActions(session, &sync);
    if (XR_FAILED(result)) {
        // Capped rather than deduplicated: a sync that starts failing mid-session is worth
        // a handful of lines, and a sync that fails every frame would otherwise be the
        // whole log.
        if (syncFailures_ < 8) {
            ++syncFailures_;
            logf("xr input: xrSyncActions refused (%s) -- the controllers are frozen this "
                 "frame%s",
                 xrResultName(api, instance, result),
                 syncFailures_ == 8 ? " (further failures not logged)" : "");
        }
        out.flags = 0;
        return;
    }

    for (int hand = 0; hand < kHands; ++hand) {
        SharedHand& target = out.hands[hand];

        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = handPaths_[hand];

        // -- the two poses -----------------------------------------------------------
        struct PoseSlot {
            int         action;
            XrSpace     space;
            SharedPose& pose;
            int32_t     activeBit;
            int32_t     positionValidBit;
            int32_t     orientationValidBit;
            int32_t     positionTrackedBit;
            int32_t     orientationTrackedBit;
        };
        PoseSlot slots[2] = {
            {kXrActionAimPose, aimSpaces_[hand], target.aimPose, kHandAimActive,
             kHandAimPositionValid, kHandAimOrientationValid, kHandAimPositionTracked,
             kHandAimOrientationTracked},
            {kXrActionGripPose, gripSpaces_[hand], target.gripPose, kHandGripActive,
             kHandGripPositionValid, kHandGripOrientationValid, kHandGripPositionTracked,
             kHandGripOrientationTracked},
        };
        for (PoseSlot& slot : slots) {
            get.action = actions_[slot.action];
            XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
            if (XR_FAILED(api.xrGetActionStatePose(session, &get, &state)) ||
                state.isActive == XR_FALSE)
                continue;
            target.flags |= slot.activeBit;
            if (!slot.space || !api.xrLocateSpace) continue;

            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(api.xrLocateSpace(slot.space, space, displayTime, &location)))
                continue;
            if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                slot.pose.positionX = finiteOrZero(location.pose.position.x);
                slot.pose.positionY = finiteOrZero(location.pose.position.y);
                slot.pose.positionZ = finiteOrZero(location.pose.position.z);
                target.flags |= slot.positionValidBit;
            }
            if (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
                slot.pose.orientationX = finiteOrZero(location.pose.orientation.x);
                slot.pose.orientationY = finiteOrZero(location.pose.orientation.y);
                slot.pose.orientationZ = finiteOrZero(location.pose.orientation.z);
                slot.pose.orientationW = finiteOrZero(location.pose.orientation.w);
                target.flags |= slot.orientationValidBit;
            }
            if (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
                target.flags |= slot.positionTrackedBit;
            if (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)
                target.flags |= slot.orientationTrackedBit;
        }

        // -- the analogue pulls -------------------------------------------------------
        get.action = actions_[kXrActionTrigger];
        XrActionStateFloat analogue{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(api.xrGetActionStateFloat(session, &get, &analogue)) &&
            analogue.isActive != XR_FALSE) {
            target.flags |= kHandTriggerActive;
            target.trigger = clampUnit(analogue.currentState);
        }

        get.action = actions_[kXrActionSqueeze];
        analogue = XrActionStateFloat{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(api.xrGetActionStateFloat(session, &get, &analogue)) &&
            analogue.isActive != XR_FALSE) {
            target.flags |= kHandSqueezeActive;
            target.squeeze = clampUnit(analogue.currentState);
        }

        // -- the stick ----------------------------------------------------------------
        get.action = actions_[kXrActionStick];
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(api.xrGetActionStateVector2f(session, &get, &stick)) &&
            stick.isActive != XR_FALSE) {
            target.flags |= kHandStickActive;
            target.stickX = clampSigned(stick.currentState.x);
            target.stickY = clampSigned(stick.currentState.y);
        }

        // -- the buttons ---------------------------------------------------------------
        const int     buttonActions[4] = {kXrActionStickClick, kXrActionPrimary, kXrActionSecondary,
                                          kXrActionMenu};
        const int32_t buttonBits[4] = {kButtonStick, kButtonPrimary, kButtonSecondary,
                                       kButtonMenu};
        for (int b = 0; b < 4; ++b) {
            get.action = actions_[buttonActions[b]];
            XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(api.xrGetActionStateBoolean(session, &get, &button)) &&
                button.isActive != XR_FALSE && button.currentState != XR_FALSE)
                target.buttons |= buttonBits[b];
        }

        // -- which controller the runtime thinks this is -------------------------------
        //
        // Logged on change only. "No controllers work" and "the runtime bound a profile we
        // never suggested" look identical from the game side, and this is the only place
        // that can tell them apart.
        if (api.xrGetCurrentInteractionProfile && api.xrPathToString) {
            XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
            if (XR_SUCCEEDED(api.xrGetCurrentInteractionProfile(session, handPaths_[hand],
                                                                &profile))) {
                if (profile.interactionProfile != XR_NULL_PATH)
                    target.flags |= kHandProfileBound;
                if (profile.interactionProfile != loggedProfile_[hand]) {
                    loggedProfile_[hand] = profile.interactionProfile;
                    // The bound controls have changed, so what the runtime calls them has
                    // too. Asked once here rather than per frame: it is several calls per
                    // action and none of it moves between profile changes.
                    namesDirty_ = true;
                    char     text[XR_MAX_PATH_LENGTH] = {};
                    uint32_t written = 0;
                    if (profile.interactionProfile != XR_NULL_PATH &&
                        XR_SUCCEEDED(api.xrPathToString(instance, profile.interactionProfile,
                                                        sizeof(text), &written, text))) {
                        logf("xr input: the %s hand is a %s", handName(hand), text);
                    } else {
                        logf("xr input: the %s hand has no interaction profile -- that "
                             "controller is off or unknown to the runtime",
                             handName(hand));
                    }
                }
            }
        }
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    storeQpc(out.sampleTime, now.QuadPart);
}

bool XrInput::takeChangedNames(const XrApi& api, XrInstance instance, XrSession session,
                               SharedInputNames& out) {
    if (!attached_ || !namesDirty_) return false;
    namesDirty_ = false;

    if (!api.xrEnumerateBoundSourcesForAction || !api.xrGetInputSourceLocalizedName ||
        !api.xrPathToString) {
        if (!loggedNoNames_) {
            loggedNoNames_ = true;
            logf("xr input: this runtime has no xrEnumerateBoundSourcesForAction or "
                 "xrGetInputSourceLocalizedName, so the menu keeps its own words for the "
                 "controls");
        }
        return false;
    }

    out = SharedInputNames{};
    int filled = 0;

    for (int action = 0; action < kXrActionCount; ++action) {
        XrBoundSourcesForActionEnumerateInfo info{
            XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
        info.action = actions_[action];

        uint32_t count = 0;
        if (XR_FAILED(api.xrEnumerateBoundSourcesForAction(session, &info, 0, &count,
                                                           nullptr)) ||
            !count)
            continue;

        // A control bound to several sources is normal -- a trigger with both a value and
        // a click, say. The first one that names a hand wins; the rest say the same thing
        // about the same button.
        XrPath sources[16] = {};
        if (count > 16) count = 16;
        uint32_t written = 0;
        if (XR_FAILED(api.xrEnumerateBoundSourcesForAction(session, &info, count, &written,
                                                           sources)))
            continue;

        for (uint32_t i = 0; i < written; ++i) {
            char     path[XR_MAX_PATH_LENGTH] = {};
            uint32_t pathWritten = 0;
            if (XR_FAILED(api.xrPathToString(instance, sources[i], sizeof(path),
                                             &pathWritten, path)))
                continue;

            int hand = -1;
            if (strncmp(path, "/user/hand/left", 15) == 0)
                hand = kHandLeft;
            else if (strncmp(path, "/user/hand/right", 16) == 0)
                hand = kHandRight;
            if (hand < 0) continue;
            if (out.text[hand][action][0]) continue;

            XrInputSourceLocalizedNameGetInfo nameInfo{
                XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
            nameInfo.sourcePath = sources[i];
            // The COMPONENT alone -- "Trigger", "A" -- is what a help line wants. Some
            // runtimes give nothing for it on its own, and the user path with it always
            // produces something, so that is the fallback rather than nothing at all.
            nameInfo.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
            char     name[XR_MAX_PATH_LENGTH] = {};
            uint32_t nameWritten = 0;
            if (XR_FAILED(api.xrGetInputSourceLocalizedName(session, &nameInfo,
                                                            sizeof(name), &nameWritten,
                                                            name)) ||
                !name[0]) {
                nameInfo.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
                                           XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
                name[0] = '\0';
                nameWritten = 0;
                if (XR_FAILED(api.xrGetInputSourceLocalizedName(
                        session, &nameInfo, sizeof(name), &nameWritten, name)))
                    continue;
            }
            if (!name[0]) continue;

            // Truncated rather than carried as a variable-length string across the
            // bitness boundary. kInputNameCapacity is generous for a button's name.
            _snprintf_s(out.text[hand][action], kInputNameCapacity, _TRUNCATE, "%s", name);
            ++filled;
        }
    }

    logf("xr input: the runtime named %d of %d bound controls%s", filled,
         static_cast<int>(kXrActionCount) * kHands,
         filled ? "" : " -- none, so the menu keeps its own words");
    if (filled) {
        logf("xr input: for example right trigger \"%s\", right primary \"%s\", left "
             "stick \"%s\"",
             out.text[kHandRight][kXrActionTrigger], out.text[kHandRight][kXrActionPrimary],
             out.text[kHandLeft][kXrActionStick]);
    }
    return filled > 0;
}

}  // namespace gplvr
