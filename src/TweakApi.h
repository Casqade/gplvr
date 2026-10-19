#pragma once

// Adjusting things, expressed as commands rather than as controller code.
//
// WHY THIS EXISTS AT ALL. The mirror aim and the HUD layout were each reachable by one
// hardware path -- four arrow keys and Page Up/Down, hard-wired into the poll that read
// them. Adding a controller that way would mean a second copy of every clamp, every
// step size and every save rule, and the two copies would drift the first time one of
// them was fixed. So the operations are stated once, in terms that name no hardware:
//
//     a TARGET       the mirror, an element of the HUD panel, the panel itself
//     an AXIS        yaw, pitch, fov, x, y, size, show
//     a VALUE        with a MODE saying whether it is the new position, a step, or a
//                    stick deflection to integrate over the poll
//     or an ACTION   select, cycle, reset, toggle, commit
//
// A stick, a key, a mouse wheel, a slider dragged with a laser pointer and a line typed
// into a console are then all the same thing: something that produces a TweakCommand.
// The mapping from a physical control to a command is a TABLE (tweakDefaultBindings),
// which is the part we expect to retune once it has been worn in, and it is the only
// part that knows what a thumbstick is.
//
// WHAT AN AXIS IS WORTH. tweakRange answers in the axis's own units: what it may be, what
// it is now, and how fast a full stick deflection should move it. A mapping needs that to
// normalise a stick and a menu needs it to draw a slider, and neither has to know whether
// it is looking at degrees of mirror yaw or a fraction of the HUD panel's width.
//
// WHERE THE VALUES LIVE. Nowhere new. Mirror axes are MirrorConfig's per-glass aims, which
// aimPane already reads every frame; HUD axes are Pribluda's live layout, which
// buildCopies already reads every frame. A tweak therefore shows on the next frame with
// no plumbing, and Commit writes the same files the keyboard modes write.
//
// WHAT IT DOES NOT DO. It does not own a mode, a selection cursor or a hand. Entering and
// leaving a tweak mode, and deciding which controller is allowed to, is VrMenu's and
// VrInput's business; this layer only ever applies one command.

#include <cstddef>
#include <cstdint>

namespace gplvr {

// What is being adjusted.
enum class TweakTarget : int32_t {
    None = 0,
    // The rear-view glasses' aim, one per glass: MirrorConfig's live set, saved per car to
    // gplvr_mirrors.ini. An index is a glass; the selection can also be -1, every glass
    // together, with yaw moving the left and right glasses opposite ways.
    Mirror,
    // One element of pribluda's HUD panel -- the times, the boards, the tyres, the dash --
    // placed in fractions of the panel and saved to gplvr_pribluda.ini.
    HudElement,
    // The panel itself: where it hangs relative to the head or the seat, and how wide it
    // is, in metres. Saved to gplvr.ini's own vrHudHead*/vrHudSeat* keys, both layers at
    // once, by hudPlacementSave.
    HudPanel,
    Count,
};

// A continuous quantity of a target. Not every axis exists on every target, and
// tweakRange is the only thing that knows which do -- a mapping asks rather than assumes,
// so a row of the binding table pointing at an axis its target does not have refuses
// loudly instead of writing somewhere unrelated.
enum class TweakAxis : int32_t {
    None = 0,
    Yaw,    // Mirror, degrees
    Pitch,  // Mirror, degrees
    Fov,    // Mirror, a multiplier on the 40-degree pane, so a MULTIPLICATIVE axis
    X,      // HudElement / HudPanel
    Y,      // HudElement / HudPanel
    Depth,  // HudPanel, metres ahead
    Size,   // HudPanel width in metres; HudElement dash width as a fraction
    Show,   // 0 or 1, expressed as an axis so one setter covers it
    Count,
};

// A discrete thing. `index` selects which element it applies to, or -1 for whichever is
// selected.
enum class TweakAction : int32_t {
    None = 0,
    Select,          // adopt `index` as the target's selection
    SelectNext,      // the next selectable index, wrapping
    SelectPrevious,  // the previous one
    Reset,           // the named axis back to its default; TweakAxis::None resets them all
    Toggle,          // flip a 0/1 axis
    Commit,          // write the target's values to the file they came from
    Count,
};

// How to read TweakCommand::value.
enum class TweakMode : int32_t {
    // `value` IS the new position, in the axis's own units. A dragged slider, a typed
    // number, a laser pointer on a track.
    Absolute,
    // `value` is added to the current position, in the axis's own units. A mouse wheel
    // detent, an arrow key's per-press step. On a multiplicative axis it is added to the
    // exponent: +1 doubles.
    Delta,
    // `value` is a -1 to 1 deflection. It is multiplied by the axis's own full-scale rate
    // and by `seconds`, so how far a stick moves a thing depends on how long it is held
    // and not on how often the caller polls.
    Rate,
};

struct TweakCommand {
    TweakTarget target = TweakTarget::None;
    TweakAxis   axis = TweakAxis::None;
    // None means this is an axis command. Anything else means it is an action and `axis`
    // only says WHICH axis the action is about (Reset, Toggle).
    TweakAction action = TweakAction::None;
    TweakMode   mode = TweakMode::Delta;
    float       value = 0.0f;
    // The poll's elapsed time. Only TweakMode::Rate reads it, and a Rate command with
    // zero seconds does nothing, which is the right answer for the first poll.
    float seconds = 0.0f;
    // Which element, or -1 for the target's current selection.
    int32_t index = -1;
};

// Apply one command. True when something actually changed -- a clamp that refused to move
// any further returns false, which is what lets a caller stop logging a held stick.
//
// Every refusal that is not a clamp says so in the log, once per reason: an axis a target
// does not have, an element that is not on the panel, a target whose file could not be
// written. A silent false and a silent true are the same thing from the headset.
bool tweakApply(const TweakCommand& command);

// What an axis can be, so a mapping can normalise and a menu can draw without knowing
// what it is looking at.
struct TweakRange {
    bool        exists = false;
    float       minimum = 0.0f;
    float       maximum = 0.0f;
    // Units per second at full stick deflection, for TweakMode::Rate. On a multiplicative
    // axis this is octaves per second.
    float       rate = 0.0f;
    // A sensible single press or detent, for TweakMode::Delta from a key or a wheel.
    float       step = 0.0f;
    float       current = 0.0f;
    float       defaultValue = 0.0f;
    // True when the axis multiplies rather than adds -- Mirror::Fov is the only one today.
    // A caller that draws a slider wants it logarithmic; a caller that adds a delta adds
    // to the exponent.
    bool        multiplicative = false;
    const char* unit = "";
};

TweakRange tweakRange(TweakTarget target, TweakAxis axis, int32_t index);

// Names, for the menu and for the log. Never null.
const char* tweakTargetName(TweakTarget target);
const char* tweakAxisName(TweakAxis axis);

// How many indices the target has, and what they are called. A target with one thing to
// adjust reports 1 and names it after itself.
int32_t     tweakIndexCount(TweakTarget target);
const char* tweakIndexName(TweakTarget target, int32_t index);

// The target's current selection, or -1 when it has none (nothing selectable, or the
// target does not have a selection at all). For the mirror -1 is a selection of its own:
// every glass together.
int32_t tweakSelected(TweakTarget target);

// True when this index can be adjusted at all right now -- a HUD element that is hidden,
// or that the layout file gives no position for, cannot be.
bool tweakIndexUsable(TweakTarget target, int32_t index);

// -- the input side ---------------------------------------------------------------------
//
// The abstract controls a mapping may bind. These are what a UX layer produces; they name
// a shape of input, not a device. PointerU and PointerV are a normalised position on
// whatever surface the pointer is on, which is what makes a dragged slider an Absolute
// command with no extra machinery.

enum class TweakInputAxis : int32_t {
    None = 0,
    StickX,
    StickY,
    Trigger,   // the analogue pull, 0 to 1
    Squeeze,   // the analogue grip, 0 to 1
    PointerU,  // 0 to 1 across the surface being pointed at
    PointerV,  // 0 to 1 down it
    Wheel,     // signed detents
    Count,
};

enum class TweakInputButton : int32_t {
    None = 0,
    Trigger,
    Squeeze,
    StickClick,
    Primary,
    Secondary,
    Menu,
    Count,
};

// One row of a mapping. Exactly one of `inputAxis` and `inputButton` is set; `modifier`
// must be held for the row to fire, and None means always.
struct TweakBinding {
    TweakTarget      target = TweakTarget::None;
    TweakInputAxis   inputAxis = TweakInputAxis::None;
    TweakInputButton inputButton = TweakInputButton::None;
    TweakInputButton modifier = TweakInputButton::None;
    TweakAxis        axis = TweakAxis::None;
    TweakAction      action = TweakAction::None;
    TweakMode        mode = TweakMode::Rate;
    // Multiplies the input before it becomes TweakCommand::value. Negative flips a stick.
    float            scale = 1.0f;
    // What the binding is for, in the log and in the menu's help line.
    const char*      description = "";
};

// The default controller mapping for a target's tweak mode. THIS TABLE IS THE MAPPING:
// nothing outside it decides that the stick turns the glasses or that the trigger picks
// an element up, so retuning the feel is editing rows here and rebuilding, with no other
// file touched.
//
// HOW A WALKER READS IT: first match wins, in table order, for each physical control. So
// a row carrying a modifier must come BEFORE the unmodified row for the same control, or
// the unmodified one swallows it and the modifier does nothing. The tables are written
// that way and the comment above each modified row says so.
//
// Action rows with TweakAction::Select leave `index` to the walker: the row says "this
// button selects", and what it selects is whatever the pointer was over, which only the
// walker knows.
const TweakBinding* tweakDefaultBindings(TweakTarget target, int32_t& count);

}  // namespace gplvr
