#include "TweakApi.h"

#include "Hooks.h"
#include "Log.h"
#include "MirrorConfig.h"
#include "Pribluda.h"

#include <cmath>

namespace gplvr {
namespace {

// Refusals. Logged once per distinct reason rather than once per poll: a held stick
// against a clamp would otherwise be the whole log, and a refusal nobody sees is how a
// feature comes to be reported as "does nothing".
//
// Compared by POINTER, which is exact here because every message is a string literal in
// this file and a repeat of the same refusal passes the same pointer.
const char* g_lastRefusal = nullptr;

bool refuse(const char* message) {
    if (message != g_lastRefusal) {
        g_lastRefusal = message;
        logf("tweak: %s", message);
    }
    return false;
}

void clearRefusal() { g_lastRefusal = nullptr; }

float clampTo(const TweakRange& range, float value) {
    if (value < range.minimum) return range.minimum;
    if (value > range.maximum) return range.maximum;
    return value;
}

// The whole of the axis arithmetic, in one place. Whatever produced the command -- a key,
// a stick, a pointer dragged along a slider -- ends here, so the clamps and the step sizes
// exist once.
//
// A multiplicative axis works in octaves: a Delta of +1 doubles, a Rate is octaves per
// second. That is the mirror's fovScale, and it is why the keyboard path used pow(2, ...)
// -- a fixed additive step feels linear at one end of the range and useless at the other.
bool resolveScalar(const TweakCommand& command, const TweakRange& range, float& out) {
    float value = range.current;
    switch (command.mode) {
        case TweakMode::Absolute:
            value = command.value;
            break;
        case TweakMode::Delta:
            value = range.multiplicative
                        ? range.current * std::pow(2.0f, command.value)
                        : range.current + command.value;
            break;
        case TweakMode::Rate: {
            if (command.seconds <= 0.0f) return false;
            const float amount = command.value * range.rate * command.seconds;
            if (amount == 0.0f) return false;
            value = range.multiplicative ? range.current * std::pow(2.0f, amount)
                                         : range.current + amount;
            break;
        }
    }
    value = clampTo(range, value);
    if (value == range.current) return false;
    out = value;
    return true;
}

// -- the mirror ---------------------------------------------------------------------------
//
// ONE GLASS OR ALL OF THEM. An index names a glass (a pane, in MirrorConfig's order); -1 is
// whatever tweak mode has selected, and the selection may itself be -1, every glass together.
// Together, yaw moves each glass by its side -- the left one way, the right the other, a centre
// glass not at all -- because a stored yaw is the same way round on every glass and "all" means
// the symmetric adjustment the single car-wide aim always made.

int mirrorGlass(int32_t index) {
    return index >= 0 ? static_cast<int>(index) : mirrorConfigSelected();
}

float* mirrorField(MirrorAim& aim, TweakAxis axis) {
    switch (axis) {
        case TweakAxis::Yaw: return &aim.yaw;
        case TweakAxis::Pitch: return &aim.pitch;
        case TweakAxis::Fov: return &aim.fovScale;
        default: return nullptr;
    }
}

// The axis of one glass. For "all" there is no single value to report, so it answers for the
// first glass, which is what a slider would have to show anyway.
TweakRange mirrorRange(TweakAxis axis, int32_t index) {
    TweakRange range;
    int glass = mirrorGlass(index);
    if (glass < 0) glass = 0;
    if (glass >= mirrorConfigGlassCount()) return range;
    MirrorAim    aim = mirrorAimFor(glass);
    const float* field = mirrorField(aim, axis);
    if (!field) return range;
    range.exists = true;
    range.current = *field;
    switch (axis) {
        case TweakAxis::Yaw:
        case TweakAxis::Pitch:
            // The same limits the arrow keys have had since mirror tweak mode was written.
            range.minimum = -45.0f;
            range.maximum = 45.0f;
            range.rate = g_options.vrMirrorTweakAngleSpeed;
            range.step = 1.0f;
            range.defaultValue = 0.0f;
            range.unit = "deg";
            break;
        case TweakAxis::Fov:
            range.minimum = 0.25f;
            range.maximum = 6.0f;
            range.rate = g_options.vrMirrorTweakFovSpeed;  // octaves per second
            range.step = 0.125f;                           // of an octave
            range.defaultValue = 1.0f;
            range.multiplicative = true;
            range.unit = "x";
            break;
        default:
            break;
    }
    return range;
}

bool applyMirror(const TweakCommand& command) {
    if (command.action == TweakAction::Commit) {
        mirrorConfigSave();
        clearRefusal();
        return true;
    }
    if (command.action == TweakAction::Toggle)
        return refuse("the mirror has no on/off axis to toggle");

    int glasses = mirrorConfigGlassCount();
    if (glasses < 1)
        return refuse("no glasses have been laid out for this car yet -- the mirror panes have "
                      "not run, so there is nothing to aim");

    if (command.action == TweakAction::Select ||
        command.action == TweakAction::SelectNext ||
        command.action == TweakAction::SelectPrevious) {
        bool changed = false;
        if (command.action == TweakAction::Select) {
            if (command.index < -1 || command.index >= glasses)
                return refuse("there is no glass with that index");
            changed = mirrorConfigSelect(static_cast<int>(command.index));
        } else {
            int before = mirrorConfigSelected();
            changed = mirrorConfigSelectStep(command.action == TweakAction::SelectNext ? 1 : -1) !=
                      before;
        }
        if (changed) {
            logf("mirror: now aiming %s", mirrorConfigSelectedName());
            clearRefusal();
        }
        return changed;
    }

    int glass = mirrorGlass(command.index);
    if (glass >= glasses) return refuse("there is no glass with that index");
    int first = glass < 0 ? 0 : glass;
    int last = glass < 0 ? glasses - 1 : glass;

    if (command.action == TweakAction::Reset) {
        bool            changed = false;
        const TweakAxis axes[3] = {TweakAxis::Yaw, TweakAxis::Pitch, TweakAxis::Fov};
        for (int i = first; i <= last; ++i)
            for (TweakAxis axis : axes) {
                if (command.axis != TweakAxis::None && command.axis != axis) continue;
                TweakRange range = mirrorRange(axis, i);
                if (!range.exists || range.current == range.defaultValue) continue;
                MirrorAim* aim = mirrorAimSlot(i);
                float*     field = aim ? mirrorField(*aim, axis) : nullptr;
                if (!field) continue;
                *field = range.defaultValue;
                changed = true;
            }
        if (changed) {
            mirrorConfigMarkEdited();
            char aims[512];
            mirrorConfigFormatAims(aims, sizeof(aims));
            logf("mirror: %s reset -- %s", mirrorConfigSelectedName(), aims);
            clearRefusal();
        }
        return changed;
    }

    if (!mirrorRange(command.axis, first).exists)
        return refuse("the mirror has no such axis -- check the binding table");

    bool changed = false;
    for (int i = first; i <= last; ++i) {
        TweakCommand one = command;
        // Every glass together turns each by its side, so the pair moves symmetrically and a
        // glass on the centreline keeps its yaw.
        if (glass < 0 && command.axis == TweakAxis::Yaw) {
            int side = mirrorConfigGlassSide(i);
            if (!side) continue;
            one.value *= static_cast<float>(side);
        }
        TweakRange range = mirrorRange(command.axis, i);
        float      value = range.current;
        if (!resolveScalar(one, range, value)) continue;
        MirrorAim* aim = mirrorAimSlot(i);
        float*     field = aim ? mirrorField(*aim, command.axis) : nullptr;
        if (!field) continue;
        *field = value;
        changed = true;
    }
    if (!changed) return false;
    mirrorConfigMarkEdited();
    clearRefusal();
    return true;
}

// -- one element of the HUD panel -----------------------------------------------------------

int hudIndex(int32_t index) {
    return index >= 0 ? static_cast<int>(index) : pribludaElementSelected();
}

TweakRange hudElementRange(TweakAxis axis, int32_t index) {
    TweakRange range;
    const int  element = hudIndex(index);
    float      x = 0.0f;
    float      y = 0.0f;

    if (axis == TweakAxis::Size) {
        // The dash's width. It is the only element with a size of its own; everything else
        // is as big as the text pribluda last drew into it.
        const float width = pribludaDashWidth();
        if (width < 0.0f) return range;
        range.exists = true;
        range.minimum = 0.05f;
        range.maximum = 1.0f;
        range.rate = 0.25f;
        range.step = 0.01f;
        range.current = width;
        range.defaultValue = width;
        range.unit = "of the panel";
        return range;
    }

    if (axis == TweakAxis::Show) {
        const int show = pribludaElementShow(element);
        range.exists = show >= 0;
        range.minimum = 0.0f;
        range.maximum = 1.0f;
        range.rate = 0.0f;
        range.step = 1.0f;
        range.current = show > 0 ? 1.0f : 0.0f;
        range.defaultValue = 1.0f;
        range.unit = "";
        return range;
    }

    if (axis != TweakAxis::X && axis != TweakAxis::Y) return range;
    if (!pribludaElementPosition(element, x, y)) return range;

    range.exists = true;
    range.minimum = 0.0f;
    range.maximum = 1.0f;
    range.rate = g_options.vrHudTweakSpeed;  // fractions of the panel per second
    range.step = 0.005f;
    range.current = axis == TweakAxis::X ? x : y;
    range.unit = "of the panel";

    // The file IS the default. gplvr_pribluda.ini is where the layout comes from and where
    // it goes back to; there is no separate factory position.
    float fileX = range.current;
    float fileY = range.current;
    if (pribludaElementFilePosition(element, fileX, fileY))
        range.defaultValue = axis == TweakAxis::X ? fileX : fileY;
    else
        range.defaultValue = range.current;
    return range;
}

bool applyHudElement(const TweakCommand& command) {
    // The keyboard mode and this one both move the same fractions. Letting both run would
    // be two owners of one number, and the arrow keys are shared with mirror tweak mode as
    // well -- so one of them has to hold the panel, and the one that got there first does.
    if (pribludaTweakActive())
        return refuse("the keyboard HUD tweak mode has the panel -- leave it (vrHudTweakKey) "
                      "before adjusting from the headset");
    if (!pribludaPanelLive())
        return refuse("the HUD panel is not being drawn, so there is nothing to place");

    const int element = hudIndex(command.index);

    switch (command.action) {
        case TweakAction::Select:
            if (!pribludaElementUsable(element))
                return refuse("that HUD element is hidden or has no position in "
                              "gplvr_pribluda.ini");
            if (pribludaElementSelected() == element) return false;
            pribludaElementSelect(element);
            logf("tweak: HUD element '%s' selected", pribludaElementName(element));
            clearRefusal();
            return true;
        case TweakAction::SelectNext:
        case TweakAction::SelectPrevious: {
            const int before = pribludaElementSelected();
            const int after =
                pribludaElementCycle(command.action == TweakAction::SelectNext ? 1 : -1);
            if (after == before) return false;
            logf("tweak: HUD element '%s' selected", pribludaElementName(after));
            clearRefusal();
            return true;
        }
        case TweakAction::Reset: {
            const bool changed = pribludaElementResetPosition(element);
            if (changed) {
                logf("tweak: HUD element '%s' back to where gplvr_pribluda.ini puts it",
                     pribludaElementName(element));
                clearRefusal();
            }
            return changed;
        }
        case TweakAction::Toggle: {
            const int show = pribludaElementShow(element);
            if (show < 0)
                return refuse("that HUD element has no show key in gplvr_pribluda.ini, so "
                              "there is nothing to toggle");
            const bool changed = pribludaElementSetShow(element, show ? 0 : 1);
            if (changed) {
                logf("tweak: HUD element '%s' is now %s", pribludaElementName(element),
                     show ? "hidden" : "shown");
                clearRefusal();
            }
            return changed;
        }
        case TweakAction::Commit:
            pribludaLayoutSave();
            clearRefusal();
            return true;
        default:
            break;
    }

    TweakRange range = hudElementRange(command.axis, command.index);
    if (!range.exists)
        return refuse("that HUD element has no such axis -- it is hidden, unplaced, or the "
                      "binding table names an axis it does not have");

    float value = range.current;
    if (!resolveScalar(command, range, value)) return false;

    if (command.axis == TweakAxis::Size) return pribludaSetDashWidth(value);
    if (command.axis == TweakAxis::Show)
        return pribludaElementSetShow(element, value >= 0.5f ? 1 : 0);

    // Position. A Delta or a Rate goes through the move, which keeps the element on the
    // panel in the direction it is travelling -- the arrow keys' rule, and it is the same
    // code. Absolute is a drag or a typed value and goes straight in.
    if (command.mode == TweakMode::Absolute) {
        float x = 0.0f;
        float y = 0.0f;
        if (!pribludaElementPosition(element, x, y)) return false;
        if (command.axis == TweakAxis::X) x = value;
        else y = value;
        const bool changed = pribludaElementSetPosition(element, x, y);
        if (changed) clearRefusal();
        return changed;
    }

    const float delta = value - range.current;
    const bool  changed = command.axis == TweakAxis::X
                              ? pribludaElementMove(element, delta, 0.0f)
                              : pribludaElementMove(element, 0.0f, delta);
    if (changed) clearRefusal();
    return changed;
}

// -- the panel itself -------------------------------------------------------------------

// Which placement the panel is using. vrHudLayer 1 hangs it off the head, 2 off the seat,
// and each has its own set of metres -- adjusting the one that is not in force would look
// like the tweak doing nothing at all.
HudPlacement* panelPlacement() {
    if (g_options.vrHudLayer == 1) return &g_options.vrHudHead;
    if (g_options.vrHudLayer == 2) return &g_options.vrHudSeat;
    return nullptr;
}

float* panelSlot(TweakAxis axis) {
    HudPlacement* placement = panelPlacement();
    if (!placement) return nullptr;
    switch (axis) {
        case TweakAxis::X: return &placement->right;
        case TweakAxis::Y: return &placement->down;
        case TweakAxis::Depth: return &placement->ahead;
        case TweakAxis::Size: return &placement->width;
        default: return nullptr;
    }
}

// The compiled-in defaults, restated here rather than read back from the ini: this is what
// Reset goes to, and the ini is the user's starting point rather than a default to keep
// returning to.
const HudPlacement& fallbackOf() {
    static const HudPlacement kHeadDefault = {0.70f, 0.10f, 0.0f, 0.45f};
    static const HudPlacement kSeatDefault = {0.70f, 0.25f, 0.0f, 0.60f};
    return g_options.vrHudLayer == 1 ? kHeadDefault : kSeatDefault;
}

TweakRange panelRange(TweakAxis axis) {
    TweakRange range;
    const float* slot = panelSlot(axis);
    if (!slot) return range;

    const HudPlacement& fallback = fallbackOf();

    range.exists = true;
    range.current = *slot;
    range.rate = 0.4f;   // metres per second at full deflection
    range.step = 0.02f;  // metres per press
    range.unit = "m";
    switch (axis) {
        // X IS A BEARING IN DEGREES, not a distance: applyHudPanel swings the panel around
        // the driver rather than sliding it sideways, so the range, the rate and the unit
        // are all angular and `current` is the bearing the placement works out to.
        case TweakAxis::X: {
            const HudPlacement* placement = panelPlacement();
            range.minimum = -110.0f;  // just past the shoulder either way
            range.maximum = 110.0f;
            range.rate = 45.0f;  // degrees per second at full deflection
            range.step = 2.0f;
            range.unit = "deg";
            range.current = placement ? std::atan2(placement->right, placement->ahead) *
                                            57.29578f
                                      : 0.0f;
            range.defaultValue =
                std::atan2(fallback.right, fallback.ahead) * 57.29578f;
            break;
        }
        case TweakAxis::Y:
            range.minimum = -2.0f;
            range.maximum = 2.0f;
            range.defaultValue = fallback.down;
            break;
        case TweakAxis::Depth:
            range.minimum = 0.15f;
            range.maximum = 3.0f;
            range.defaultValue = fallback.ahead;
            break;
        case TweakAxis::Size:
            range.minimum = 0.10f;
            range.maximum = 3.0f;
            range.defaultValue = fallback.width;
            break;
        default:
            break;
    }
    return range;
}

bool applyHudPanel(const TweakCommand& command) {
    if (!panelPlacement())
        return refuse("the HUD panel is off (vrHudLayer 0), so there is nowhere to put it");

    if (command.action == TweakAction::Commit) {
        if (!hudPlacementSave())
            return refuse("the panel's placement could not be written -- the pribluda: line "
                          "above says which keys and why");
        clearRefusal();
        return true;
    }
    if (command.action == TweakAction::Select ||
        command.action == TweakAction::SelectNext ||
        command.action == TweakAction::SelectPrevious)
        return refuse("there is one HUD panel and nothing to select between");
    if (command.action == TweakAction::Toggle)
        return refuse("the panel has no on/off axis -- vrHudLayer picks head or seat, and "
                      "the menu owns that");

    if (command.action == TweakAction::Reset) {
        bool            changed = false;
        const TweakAxis axes[4] = {TweakAxis::X, TweakAxis::Y, TweakAxis::Depth,
                                   TweakAxis::Size};
        for (TweakAxis axis : axes) {
            if (command.axis != TweakAxis::None && command.axis != axis) continue;
            // X's default is a BEARING in degrees and its slot holds metres, so writing one
            // into the other would put a number like -40 into `right`. Both components of
            // the bearing go back together instead.
            if (axis == TweakAxis::X) {
                HudPlacement* placement = panelPlacement();
                if (placement->right == fallbackOf().right &&
                    placement->ahead == fallbackOf().ahead)
                    continue;
                placement->right = fallbackOf().right;
                placement->ahead = fallbackOf().ahead;
                changed = true;
                continue;
            }
            // Depth is the other half of the same pair, and X has just restored it.
            if (axis == TweakAxis::Depth && command.axis == TweakAxis::None) continue;
            float* slot = panelSlot(axis);
            TweakRange range = panelRange(axis);
            if (!slot || *slot == range.defaultValue) continue;
            *slot = range.defaultValue;
            changed = true;
        }
        if (changed) {
            HudPlacement* placement = panelPlacement();
            logf("tweak: the %s HUD panel is back to ahead %.2f down %.2f right %.2f "
                 "width %.2f m",
                 g_options.vrHudLayer == 1 ? "head" : "seat", placement->ahead,
                 placement->down, placement->right, placement->width);
            clearRefusal();
        }
        return changed;
    }

    // SIDEWAYS IS A ROTATION AROUND THE DRIVER, not a slide along a rail.
    //
    // Translating `right` changes two things the driver did not ask to change: the panel gets
    // further away as it goes out (its distance is the hypotenuse), and it stops facing them,
    // because the quad's facing is derived from where it sits. Swinging it around the head's
    // vertical axis at constant distance keeps both -- readable at every bearing, and the way
    // a display on an arm actually moves. It is what every other VR panel does.
    //
    // The bearing is stored back into `right` and `ahead`, which stay the placement's own
    // units; nothing downstream has to know this happened.
    if (command.axis == TweakAxis::X) {
        HudPlacement* placement = panelPlacement();
        TweakRange    range = panelRange(TweakAxis::X);
        float         level = std::sqrt(placement->right * placement->right +
                                        placement->ahead * placement->ahead);
        // A panel pulled into the driver's face has no bearing to speak of, and dividing a
        // rotation into it would fling it about.
        if (level < 0.05f) return refuse("the panel is too close to swing around you -- push "
                                         "it away first");

        float bearing = std::atan2(placement->right, placement->ahead) * 57.29578f;
        float turned = bearing;
        if (!resolveScalar(command, range, turned)) return false;
        float radians = turned * 0.0174532925f;
        placement->right = level * std::sin(radians);
        placement->ahead = level * std::cos(radians);
        clearRefusal();
        return true;
    }

    float*     slot = panelSlot(command.axis);
    TweakRange range = panelRange(command.axis);
    if (!slot || !range.exists)
        return refuse("the HUD panel has no such axis -- check the binding table");

    float value = range.current;
    if (!resolveScalar(command, range, value)) return false;
    *slot = value;
    clearRefusal();
    return true;
}

// -- the default controller mapping ---------------------------------------------------------
//
// EVERY row here is provisional. The user asked for the API first and the feel later, and
// this is the "later" file: nothing outside these three tables decides what a button does.

const TweakBinding kMirrorBindings[] = {
    // The modified rows come first, because a walker takes the first match: with the
    // trigger held the stick widens and narrows the glasses instead of aiming them.
    // Scale is NEGATIVE so that stick-up zooms IN. Fov is a multiplier on the angle the
    // glass subtends, so a bigger number shows more of the world, smaller; pushing the
    // stick away to magnify is the direction every other zoom control in the world uses.
    {TweakTarget::Mirror, TweakInputAxis::StickY, TweakInputButton::None,
     TweakInputButton::Trigger, TweakAxis::Fov, TweakAction::None, TweakMode::Rate, -1.0f,
     "trigger held: stick forward zooms the mirror in, back zooms it out"},
    {TweakTarget::Mirror, TweakInputAxis::StickX, TweakInputButton::None,
     TweakInputButton::None, TweakAxis::Yaw, TweakAction::None, TweakMode::Rate, 1.0f,
     "stick left and right turns the mirror"},
    {TweakTarget::Mirror, TweakInputAxis::StickY, TweakInputButton::None,
     TweakInputButton::None, TweakAxis::Pitch, TweakAction::None, TweakMode::Rate, 1.0f,
     "stick forward and back tilts the mirror"},
    {TweakTarget::Mirror, TweakInputAxis::None, TweakInputButton::StickClick,
     TweakInputButton::None, TweakAxis::None, TweakAction::Reset, TweakMode::Absolute, 1.0f,
     "stick click: the selected mirror (or all of them) back to straight and unzoomed"},
    {TweakTarget::Mirror, TweakInputAxis::None, TweakInputButton::Primary,
     TweakInputButton::None, TweakAxis::None, TweakAction::SelectNext, TweakMode::Absolute,
     1.0f, "A or X: next mirror -- all of them together, then each one in turn"},
    {TweakTarget::Mirror, TweakInputAxis::None, TweakInputButton::Secondary,
     TweakInputButton::None, TweakAxis::None, TweakAction::SelectPrevious, TweakMode::Absolute,
     1.0f, "B or Y: previous mirror"},
};

const TweakBinding kHudElementBindings[] = {
    // Drag first: with the grip held the pointer places the element outright, and the
    // unmodified pointer rows below never fire while it is down.
    {TweakTarget::HudElement, TweakInputAxis::PointerU, TweakInputButton::None,
     TweakInputButton::Squeeze, TweakAxis::X, TweakAction::None, TweakMode::Absolute, 1.0f,
     "HOLD the grip: the controller's pointer drags the selected readout (a tap goes back "
     "instead)"},
    {TweakTarget::HudElement, TweakInputAxis::PointerV, TweakInputButton::None,
     TweakInputButton::Squeeze, TweakAxis::Y, TweakAction::None, TweakMode::Absolute, 1.0f,
     "HOLD the grip: the controller's pointer drags the selected readout (a tap goes back "
     "instead)"},
    // The dash's size, and this is one of the two bindings left open. It shares the
    // trigger with Select below, so pressing the trigger to start sizing also selects
    // whatever the pointer happened to be over -- harmless, because selecting changes
    // nothing, but it is the reason this row is marked undecided rather than settled.
    {TweakTarget::HudElement, TweakInputAxis::StickY, TweakInputButton::None,
     TweakInputButton::Trigger, TweakAxis::Size, TweakAction::None, TweakMode::Rate, 1.0f,
     "UNDECIDED -- trigger held: stick forward and back widens and narrows the dash readout "
     "(the only one that resizes)"},
    {TweakTarget::HudElement, TweakInputAxis::StickX, TweakInputButton::None,
     TweakInputButton::None, TweakAxis::X, TweakAction::None, TweakMode::Rate, 1.0f,
     "stick left and right moves the selected readout"},
    // Scale is negative because the panel's y counts down from its top edge.
    {TweakTarget::HudElement, TweakInputAxis::StickY, TweakInputButton::None,
     TweakInputButton::None, TweakAxis::Y, TweakAction::None, TweakMode::Rate, -1.0f,
     "stick forward moves the selected readout up, back moves it down"},
    {TweakTarget::HudElement, TweakInputAxis::None, TweakInputButton::Trigger,
     TweakInputButton::None, TweakAxis::None, TweakAction::Select, TweakMode::Absolute,
     1.0f, "trigger: select the readout under the pointer"},
    {TweakTarget::HudElement, TweakInputAxis::None, TweakInputButton::StickClick,
     TweakInputButton::None, TweakAxis::None, TweakAction::Reset, TweakMode::Absolute, 1.0f,
     "stick click: the readout back to where gplvr_pribluda.ini has it"},
    {TweakTarget::HudElement, TweakInputAxis::None, TweakInputButton::Primary,
     TweakInputButton::None, TweakAxis::None, TweakAction::SelectNext, TweakMode::Absolute,
     1.0f, "A or X: select the next readout"},
    {TweakTarget::HudElement, TweakInputAxis::None, TweakInputButton::Secondary,
     TweakInputButton::None, TweakAxis::Show, TweakAction::Toggle, TweakMode::Absolute,
     1.0f, "UNDECIDED -- B or Y: hide or show the selected readout (a hidden one is crossed "
           "out and can still be selected)"},
};

const TweakBinding kHudPanelBindings[] = {
    {TweakTarget::HudPanel, TweakInputAxis::StickY, TweakInputButton::None,
     TweakInputButton::Trigger, TweakAxis::Depth, TweakAction::None, TweakMode::Rate, 1.0f,
     "trigger held: stick forward pushes the panel away, back pulls it in"},
    // Both modified rows are the VERTICAL stick, so the two hold-and-push gestures differ
    // only in which button is held and both read "more" as away from you: forward pushes
    // the panel out, forward makes it bigger.
    {TweakTarget::HudPanel, TweakInputAxis::StickY, TweakInputButton::None,
     TweakInputButton::Squeeze, TweakAxis::Size, TweakAction::None, TweakMode::Rate, 1.0f,
     "HOLD the grip: stick forward makes the panel bigger, back smaller (a tap goes back "
     "instead)"},
    {TweakTarget::HudPanel, TweakInputAxis::StickX, TweakInputButton::None,
     TweakInputButton::None, TweakAxis::X, TweakAction::None, TweakMode::Rate, 1.0f,
     "stick left and right swings the panel around you, keeping its distance"},
    // Scale is negative because the placement's Down counts downward.
    {TweakTarget::HudPanel, TweakInputAxis::StickY, TweakInputButton::None,
     TweakInputButton::None, TweakAxis::Y, TweakAction::None, TweakMode::Rate, -1.0f,
     "stick forward raises the panel, back lowers it"},
    {TweakTarget::HudPanel, TweakInputAxis::None, TweakInputButton::StickClick,
     TweakInputButton::None, TweakAxis::None, TweakAction::Reset, TweakMode::Absolute, 1.0f,
     "stick click: the panel back to its built-in default place and size"},
};

}  // namespace

bool tweakApply(const TweakCommand& command) {
    switch (command.target) {
        case TweakTarget::Mirror: return applyMirror(command);
        case TweakTarget::HudElement: return applyHudElement(command);
        case TweakTarget::HudPanel: return applyHudPanel(command);
        default:
            return refuse("a command arrived with no target -- the binding table has a "
                          "row that was never filled in");
    }
}

TweakRange tweakRange(TweakTarget target, TweakAxis axis, int32_t index) {
    switch (target) {
        case TweakTarget::Mirror: return mirrorRange(axis, index);
        case TweakTarget::HudElement: return hudElementRange(axis, index);
        case TweakTarget::HudPanel: return panelRange(axis);
        default: return TweakRange{};
    }
}

const char* tweakTargetName(TweakTarget target) {
    switch (target) {
        case TweakTarget::Mirror: return "mirror";
        case TweakTarget::HudElement: return "HUD element";
        case TweakTarget::HudPanel: return "HUD panel";
        default: return "nothing";
    }
}

const char* tweakAxisName(TweakAxis axis) {
    switch (axis) {
        case TweakAxis::Yaw: return "yaw";
        case TweakAxis::Pitch: return "pitch";
        case TweakAxis::Fov: return "field";
        case TweakAxis::X: return "x";
        case TweakAxis::Y: return "y";
        case TweakAxis::Depth: return "depth";
        case TweakAxis::Size: return "size";
        case TweakAxis::Show: return "shown";
        default: return "none";
    }
}

int32_t tweakIndexCount(TweakTarget target) {
    switch (target) {
        case TweakTarget::HudElement: return pribludaElementCount();
        case TweakTarget::Mirror: return mirrorConfigGlassCount();
        case TweakTarget::HudPanel: return 1;
        default: return 0;
    }
}

const char* tweakIndexName(TweakTarget target, int32_t index) {
    if (target == TweakTarget::HudElement)
        return pribludaElementName(static_cast<int>(index));
    if (target == TweakTarget::Mirror) return mirrorConfigGlassName(static_cast<int>(index));
    return tweakTargetName(target);
}

int32_t tweakSelected(TweakTarget target) {
    if (target == TweakTarget::HudElement) return pribludaElementSelected();
    if (target == TweakTarget::Mirror) return mirrorConfigSelected();
    if (target == TweakTarget::HudPanel) return 0;
    return -1;
}

bool tweakIndexUsable(TweakTarget target, int32_t index) {
    if (target == TweakTarget::HudElement)
        return pribludaElementUsable(static_cast<int>(index));
    if (target == TweakTarget::Mirror) return index >= 0 && index < mirrorConfigGlassCount();
    return index == 0 && tweakIndexCount(target) == 1;
}

const TweakBinding* tweakDefaultBindings(TweakTarget target, int32_t& count) {
    switch (target) {
        case TweakTarget::Mirror:
            count = static_cast<int32_t>(sizeof(kMirrorBindings) / sizeof(kMirrorBindings[0]));
            return kMirrorBindings;
        case TweakTarget::HudElement:
            count = static_cast<int32_t>(sizeof(kHudElementBindings) /
                                         sizeof(kHudElementBindings[0]));
            return kHudElementBindings;
        case TweakTarget::HudPanel:
            count = static_cast<int32_t>(sizeof(kHudPanelBindings) /
                                         sizeof(kHudPanelBindings[0]));
            return kHudPanelBindings;
        default:
            count = 0;
            return nullptr;
    }
}

}  // namespace gplvr
