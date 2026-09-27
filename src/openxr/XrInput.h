#pragma once

// The motion controllers, which exist only here.
//
// OpenXR is 64-bit-only in practice and gplc67.exe is Win32, so the game process cannot
// call xrSyncActions or anything else in this file. Every control the game will ever see
// is sampled here, once a frame, at the same predicted display time the views are located
// for, and published into the shared control block as SharedInput.
//
// ONE ACTION SET, ONE ACTION PER CONTROL, BOTH HANDS ON EVERY ACTION. Each action is
// created with both /user/hand/left and /user/hand/right as subaction paths, so the state
// of a given control on a given hand is one xrGetActionState* call with that hand's path
// in XrActionStateGetInfo::subactionPath. The alternative -- two actions per control --
// doubles the table for nothing.
//
// The bindings are a table rather than code, because the table is the part that changes:
// a new headset is a new row of component paths, not a new code path. A component path
// the runtime does not recognise is skipped with a log line and the rest of the profile is
// still suggested, so one unknown path cannot cost a controller all its buttons.
//
// WHAT IS DELIBERATELY NOT HERE: haptics. XR_ACTION_TYPE_VIBRATION_OUTPUT would need a
// return channel from the game (a counter in the control block the presenter consumes),
// and nothing in gplvr asks for one yet. The action set is the only place it would have to
// be added, so adding it later costs one action and one binding row per profile.

#include "openxr/Xr.h"
#include "presenter/SharedPresentation.h"

namespace gplvr {

// Every control gplvr asks OpenXR for, in the order the tables in the .cpp index. At
// namespace scope rather than inside the class so the binding tables can name a row
// instead of numbering it -- a renumbered enum and a table of integers is exactly the
// silent mismatch this project cannot afford without a compiler to run.
enum XrInputAction {
    kXrActionAimPose = 0,
    kXrActionGripPose,
    kXrActionTrigger,
    kXrActionSqueeze,
    kXrActionStick,
    kXrActionStickClick,
    kXrActionPrimary,
    kXrActionSecondary,
    kXrActionMenu,
    kXrActionCount,
};

class XrInput {
public:
    // Creates the action set, its actions, the suggested bindings for every profile in the
    // table, attaches the set to the session and creates the four pose spaces. False means
    // the game gets no controllers this session; every step that refused says so in the
    // log. Safe to call once per session, after xrCreateSession and before xrSyncActions.
    bool create(const XrApi& api, XrInstance instance, XrSession session);

    void destroy(const XrApi& api);

    bool attached() const { return attached_; }

    // One xrSyncActions and one read of every control, into `out`. `space` is the space the
    // poses are located in -- the same appSpace the views and the head pose use, or the
    // hands would arrive in a frame of reference the game has no transform for.
    //
    // `focused` is whether the session is in XR_SESSION_STATE_FOCUSED. Unfocused, OpenXR
    // reports every action inactive, so the whole sample is zeroed and the flag says why
    // rather than the game reading it as the driver letting go of everything.
    void sample(const XrApi& api, XrInstance instance, XrSession session, XrSpace space,
                XrTime displayTime, bool focused, SharedInput& out);

    // WHAT THE RUNTIME CALLS EACH BOUND CONTROL, re-queried when a hand's interaction
    // profile changes and not otherwise. True when `out` was filled and is worth
    // publishing.
    //
    // OpenXR has no glyph or icon API -- nothing in the headers returns a picture of a
    // button, and the only "picture of the controller" it offers at all is a glTF model
    // through XR_MSFT_controller_model or XR_FB_render_model, which would mean a glTF
    // loader for a help line. xrGetInputSourceLocalizedName is core and answers the
    // question that actually matters: what is this action bound to on the hardware
    // attached right now, in words the driver will recognise. Our own binding table can
    // only name controls in our words, and we do not know which profile a given headset
    // will bind.
    bool takeChangedNames(const XrApi& api, XrInstance instance, XrSession session,
                          SharedInputNames& out);

private:
    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction    actions_[kXrActionCount] = {};
    XrPath      handPaths_[kHands] = {};
    XrSpace     aimSpaces_[kHands] = {};
    XrSpace     gripSpaces_[kHands] = {};
    bool        attached_ = false;

    // The interaction profile last logged for each hand, so a controller waking up or
    // being swapped is one line rather than one a frame.
    XrPath loggedProfile_[kHands] = {};
    int    syncFailures_ = 0;
    // A hand's profile changed, so the localized names are stale. Set true at startup so
    // the first sample after a controller appears asks once.
    bool namesDirty_ = true;
    bool loggedNoNames_ = false;
};

}  // namespace gplvr
