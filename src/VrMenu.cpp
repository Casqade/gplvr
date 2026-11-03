#include "VrMenu.h"

#include "DataRebase.h"
#include "HeadAudio.h"
#include "Hooks.h"
#include "Interpolation.h"
#include "Log.h"
#include "MenuScale.h"
#include "MirrorConfig.h"
#include "Paths.h"
#include "Pribluda.h"
#include "QuickMenu.h"
#include "RenderTarget.h"
#include "ReplayPanel.h"
#include "ReplayQuickMenu.h"
#include "Stereo.h"
#include "TweakApi.h"
#include "VrCamera.h"
#include "VrInput.h"
#include "VrText.h"
#include "Window.h"
#include "presenter/Producer.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// -- geometry ------------------------------------------------------------------------
//
// All of this is in the presenter's LOCAL space and OpenXR's axes -- +X right, +Y up, -Z
// forward -- because that is the space the published head pose, eye poses and hand poses
// already share. Converting to GPL's axes first would put the panel in one frame and the
// ray in another, and the whole reason this is placeable at all is that they agree.

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

// The same rotation VrCamera uses: v' = v + 2w(qv x v) + 2 qv x (qv x v).
Vec3 rotate(const Quat& q, const Vec3& v) {
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    Vec3 out;
    out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
    out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
    out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
    return out;
}

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Zero when the vector has no length worth normalising, so a caller that divides by the
// result gets nothing rather than an infinity.
Vec3 normalise(const Vec3& v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 1e-6f)) return {0.0f, 0.0f, 0.0f};
    return {v.x / length, v.y / length, v.z / length};
}

// a then b, in that order: the rotation that applies b in a's frame. Used to carry the
// presenter's quad orientation, which is stated relative to its space, into LOCAL.
Quat multiply(const Quat& a, const Quat& b) {
    Quat out;
    out.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    out.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    out.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    out.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return out;
}

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

Quat quatOf(const SharedPose& pose) {
    return {pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
}

Vec3 positionOf(const SharedPose& pose) {
    return {pose.positionX, pose.positionY, pose.positionZ};
}

// The heading alone, as the seat capture takes it and for the same reason: a panel placed
// against a head that happened to be tilted inherits the tilt, and a tilted panel reads
// as the whole world leaning.
Quat yawOnly(const Quat& q) {
    const Vec3 forward = rotate(q, {0.0f, 0.0f, -1.0f});
    const float planar = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (!(planar > 0.01f)) return {0.0f, 0.0f, 0.0f, 1.0f};
    const float yaw = std::atan2(-forward.x, -forward.z);
    return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

// -- the panel -------------------------------------------------------------------------

// THE CANVAS. Everything the panel says is drawn into a texture of this size, in its
// pixels, and the texture is then mapped onto the panel's four projected corners. Two
// consequences, and both were bugs in the first build:
//
//   * the text turns with the panel, because it IS the panel. glBitmap cannot be rotated
//     -- a raster position places it and nothing transforms it -- so text drawn straight
//     into the frame stayed level with the display while the quad rolled with the head.
//   * how big the text looks is set by these numbers and the panel's size in metres, and
//     by nothing else. Sizing the font from the projected pixel height, as the first
//     build did, made the tweak strip's readout three times too wide for its own strip.
constexpr int kMenuCanvasWidth = 1024;
// Tall enough for the lines there are: the buttons, the tabs and the tallest tab's rows. At
// 768 and nineteen rows a band came out 34 px for a 30 px font -- the glyphs fit, but with
// nothing between one row and the next. The quad's height in metres follows this through
// kPanelHeight, so the panel grows with it rather than squeezing the same picture; that is the
// property to keep when rows are added again. 640 is nine lines at about 62 px each.
constexpr int kMenuCanvasHeight = 640;
constexpr int kStripCanvasWidth = 1024;
// Tall enough for the readout AND the mode's whole control list under it. A driver inside a
// tweak mode cannot see the menu's help line, and leaving the mode to read what the buttons do
// is exactly the trip the list exists to save.
constexpr int kStripCanvasHeight = 400;

// Font sizes in canvas pixels. The menu's body has to fit "Save mirror aim" in the label
// column and the small size has to fit the longest help line across the title band; the
// strip's has to fit its whole readout across 1024.
constexpr int kMenuBodyPixels = 30;
constexpr int kMenuSmallPixels = 19;
constexpr int kStripBodyPixels = 36;

// THE FOOTER IS A FIXED HEIGHT, whatever it happens to be saying.
//
// A help line that grows and shrinks with the hovered row would move every row above it, so
// the row under the pointer would slide out from under the pointer as it was hovered. The
// space is therefore reserved: the rows are laid out in what is left, the text wraps into the
// reservation, and a hint too long for it is cut rather than allowed to push anything.
constexpr int   kHelpLines = 3;
constexpr int   kHintLines = 9;  // a tweak mode's list, which is longer and has no rows to move
constexpr int   kHelpLineChars = 160;
constexpr float kHelpLineSpacing = 1.25f;
// One margin for every footer, so the width the text was WRAPPED to and the width it is drawn
// into are the same number in all three modes -- including the signature's copy, which would
// otherwise rebuild the canvas forever against a wrap it never draws.
constexpr float kHelpMargin = 22.0f;

// Far enough not to need focusing effort, near enough that the panel fills a comfortable
// part of the view. Not an option: the whole feature is a default.
//
// The extents follow the canvas's shape, so a canvas resize cannot silently stretch the
// text: change one and the other follows.
constexpr float kPanelDistance = 0.85f;
constexpr float kPanelWidth = 0.52f;
constexpr float kPanelHeight = kPanelWidth * static_cast<float>(kMenuCanvasHeight) /
                               static_cast<float>(kMenuCanvasWidth);

// The tweak modes replace the panel with a strip, because a panel in front of the glasses
// while the glasses are being aimed is the one place it must not be.
constexpr float kStripWidth = 0.44f;
constexpr float kStripHeight = kStripWidth * static_cast<float>(kStripCanvasHeight) /
                               static_cast<float>(kStripCanvasWidth);
constexpr float kStripDrop = 0.22f;  // metres below the panel's centre

// THE FRONT END'S CONTROL PLANE. A rectangle square in front of the head that the aim ray
// is intersected with; where it is hit is where the Windows pointer goes.
//
// This replaces a head-relative angular box, and the difference is wrist effort. A box
// throws the controller's POSITION away and keeps only its direction, so every degree of
// reach has to come from the wrist. A ray moves its hit point when the controller is
// MOVED as well as when it is turned, so the reach is shared between shoulder, elbow and
// wrist -- and the further the plane is beyond the hand, the more a small turn is worth.
//
// From a relaxed forearm, roughly 0.4 m ahead of the head, the throw to the plane is
// about 1.6 m: an edge is 14 degrees of turn, or 0.40 m of arm, or any mix. The old box
// wanted 25 degrees of pure wrist for the same edge.
//
// NONE OF THESE THREE IS A MEASUREMENT. They are the reach we want and the numbers to
// tune: a larger distance costs less wrist and amplifies tracking noise (the smoothing
// below is what pays for it); a smaller half-width costs less of everything and makes the
// pointer twitchy.
constexpr float kFrontEndDistance = 2.0f;    // metres ahead of the head
constexpr float kFrontEndHalfWidth = 0.40f;  // metres; the height follows the menu's shape

// The comfort offset, and it is a comfort offset rather than a measurement: the plane's
// centre sits this far BELOW the head's eye line, which biases every ray upward in the
// image by the same amount. A controller held where a hand naturally rests -- below the
// eyes, pointing roughly level -- then lands mid-screen instead of off the bottom.
constexpr float kFrontEndUpwardBias = 0.34f;

// -- smoothing the pointer -------------------------------------------------------------
//
// ONE FILTER FOR EVERY POINTER, and these are the only numbers it has. The front end's
// mapped position and the in-race ray's hit on the panel are the same quantity -- a point
// on a rectangle, in fractions of it -- so they go through the same code with the same
// constants and cannot be tuned apart.
//
// IT IS A 1-EURO FILTER. A one-pole low-pass whose cutoff RISES WITH THE POINTER'S OWN
// SPEED: a hand held still is filtered hard, a deliberate sweep is barely filtered at all.
// A fixed alpha cannot be both -- enough smoothing to kill the tremor puts visible lag on
// every movement -- and that trade is the whole reason the cutoff moves.
//
// The speed the cutoff follows is ITSELF low-passed, at kPointerSpeedCutoffHz. The speed
// measured straight from raw samples is mostly tremor, so feeding it to the cutoff opens
// the filter up on a hand that is not going anywhere, which is the shake it was supposed
// to remove.
//
// Everything is in fractions of the surface being pointed at, and every step is worked out
// from the elapsed time rather than per poll, so the same numbers behave the same at 45 Hz
// and at 90.
constexpr float  kPointerDeadband = 0.0018f;      // fractions of the surface
constexpr float  kPointerMinCutoffHz = 1.2f;      // a still hand: filtered hard
constexpr float  kPointerMaxCutoffHz = 24.0f;     // a fast sweep: barely filtered
constexpr float  kPointerSpeedToCutoff = 12.0f;   // Hz per fraction-per-second of speed
constexpr float  kPointerSpeedCutoffHz = 1.0f;    // the low-pass on the speed itself
constexpr double kPointerSnapSeconds = 0.10;      // a longer gap snaps rather than slides

// How far the pointer has to travel from where the grip closed before a HUD element is
// being DRAGGED rather than held still. Below this the grip is doing nothing, which is
// what lets its release still count as a tap -- see gripTapped and claimGrip.
constexpr float kGrabSlop = 0.010f;  // fractions of the panel

// The pointer only takes the mouse over while the hand is doing something. Below this,
// per poll, the hand counts as still; after kPointerIdleSeconds of stillness the real
// mouse has it back.
constexpr float  kPointerWakeRadians = 0.0035f;
constexpr double kPointerIdleSeconds = 1.5;

enum class Mode {
    Closed = 0,
    Menu,
    MirrorTweak,
    HudTweak,
    PanelTweak,
    // "Save changes?" on the way out of a tweak mode that changed something. Its own mode
    // rather than a flag, because while it is up the tweak mode's bindings must not run --
    // the stick answering the question must not also be moving the thing being asked about.
    Confirm,
    // "Reset what?" -- five answers rather than three, and its own mode for the same reason
    // Confirm is: while a question about the settings is up, nothing else may write them.
    Reset,
};

// What Reset offers, in the order it is drawn. Cancel LAST and the destructive one next to
// it, so the stick has to travel past everything to reach "all" and a driver who opened this
// by accident is one tap from leaving.
enum ResetChoice {
    kResetVrSettings = 0,
    kResetMirrors,
    kResetPribluda,
    kResetEverything,
    kResetCancel,
    kResetCount
};

const char* const kResetLabel[kResetCount] = {"Reset VR settings", "Clear mirror presets",
                                              "Reset Pribluda layout", "Reset ALL", "Cancel"};

// The three answers to "save changes?", in the order they are drawn.
enum ConfirmChoice { kConfirmSave = 0, kConfirmDiscard, kConfirmCancel, kConfirmCount };

enum RowKind {
    kRowAction = 0,
    kRowToggleBool,
    // An int that only has two useful values, shown and worked as on and off. `low` is the ON
    // value and `high` the OFF one, so a setting whose "on" is the larger number reads the
    // right way round without a second field saying which way it runs.
    kRowToggleInt,
    kRowChoiceInt,
    // An int whose values have NAMES rather than meanings you have to remember. `help` holds
    // what each one does; the value column holds what it is called. A number in that column
    // is only ever readable to whoever wrote the option.
    kRowNamedInt,
    // An int that means "one frame in N", shown as 1, 1/2, 1/3 and (where `low` is 0) off.
    // A plain number would read as a rate and invite the question "one what in N" every
    // time; the fraction is what the setting actually does to the update.
    kRowRateInt,
    kRowSliderFloat,
    // One of the tabs across the strip under the buttons. Its `tab` is the tab it opens.
    kRowTab,
};

// THE TABS. Each setting lives on one, filed where somebody chasing the symptom would look:
// the update rates are on Performance rather than beside the mirror and the panel, because a
// driver reaches for them when the frame rate drops. The page's own buttons -- Save settings,
// Reset, Close -- are on no tab and sit above the strip, so they are there whichever tab is open.
enum MenuTab { kTabHeadset = 0, kTabCockpit, kTabPerformance, kTabDisplay, kTabCount };
constexpr int kTabNone = -1;

enum RowCommand {
    kCommandNone = 0,
    kCommandClose,
    kCommandRecentre,
    kCommandMirrorTweak,
    kCommandHudTweak,
    kCommandPanelTweak,
    kCommandSaveSettings,
    kCommandResetSettings,
};

struct MenuRow {
    const char* label;
    int         kind;
    int         command;   // kRowAction only
    bool*       boolValue;  // kRowToggleBool
    int*        intValue;   // kRowChoiceInt
    float*      floatValue; // kRowSliderFloat
    float       low;
    float       high;
    float       step;
    const char* help;
    // The tab the row is on, kTabNone for the buttons above the strip, and for a kRowTab the
    // tab it opens.
    int tab;
    // Whether the row belongs on the page at all, asked every time the page is laid out.
    // Null is yes, which is what every row that always applies leaves it as. A row this
    // refuses takes no band, cannot be highlighted and is not drawn -- the rows below it on
    // its tab move up.
    bool (*offered)();
};

// pribluda's rows. The overlay is a separate mod and a GEM+ patch, so an install can
// perfectly well not have it -- the 2020 demo does not -- and six rows that cannot do
// anything are worse than six rows missing: each one reads as a setting that is being
// ignored.
bool pribludaRowOffered() { return pribludaPresence() != kPribludaAbsent; }

// The root page. Every row here changes a live value and nothing else; Save settings is what
// writes them to gplvr.ini, so a page of experiments is thrown away by not pressing it.
//
// NOT AN OPTION, and deliberately not one: turning the HUD off is something a driver does for
// a lap to see the road, or to find out what the panel costs. It is therefore not one of the
// values Save writes, and every session starts with the HUD on.
bool g_pribludaOn = true;

// A row's help says WHAT THE ROW IS, and never which button to press. How to work the
// highlighted widget is the same answer for every row of its kind and is built from the
// runtime's own control names in canvasControlLine; how to work a TWEAK MODE is the mode's
// whole binding table, listed in the mode itself where it is needed. Putting the scheme in
// each row's words repeated it thirteen times and still left it invisible from inside a mode.
//
// In page order: the buttons above the strip, the tabs, then each tab's rows, top to bottom.
MenuRow g_rows[] = {
    {"Save settings", kRowAction, kCommandSaveSettings, nullptr, nullptr, nullptr, 0, 0, 0,
     "write the settings on every tab to gplvr.ini, so they are what the game starts with next "
     "time. The mirror aim and the pribluda layout are not here -- each tweak mode asks about "
     "its own on the way out",
     kTabNone},
    {"Reset", kRowAction, kCommandResetSettings, nullptr, nullptr, nullptr, 0, 0, 0,
     "put things back to how they shipped -- it asks which. A stick click on any setting "
     "resets that one alone",
     kTabNone},
    {"Close", kRowAction, kCommandClose, nullptr, nullptr, nullptr, 0, 0, 0, "", kTabNone},

    {"Headset", kRowTab, kCommandNone, nullptr, nullptr, nullptr, 0, 0, 0,
     "how it feels to sit in the car: recentre, world scale, horizon lock, gamma and the "
     "head-tracked sound",
     kTabHeadset},
    {"Cockpit", kRowTab, kCommandNone, nullptr, nullptr, nullptr, 0, 0, 0,
     "what is in the car with you: the mirrors and pribluda's panel", kTabCockpit},
    {"Performance", kRowTab, kCommandNone, nullptr, nullptr, nullptr, 0, 0, 0,
     "what a frame costs: the frame rate, the renderer, the render scale, and how often and "
     "from where the mirrors and the panel are redrawn",
     kTabPerformance},
    {"Display", kRowTab, kCommandNone, nullptr, nullptr, nullptr, 0, 0, 0,
     "text and screens: GPL's own text, its menus in the headset, and the monitor window",
     kTabDisplay},

    {"Recentre", kRowAction, kCommandRecentre, nullptr, nullptr, nullptr, 0, 0, 0,
     "this pose becomes the seat -- also on a face button or a stick click at any time with "
     "the menu closed, which is how to use it mid-lap",
     kTabHeadset},
    {"World scale", kRowSliderFloat, kCommandNone, nullptr, nullptr, &g_options.vrWorldScale,
     0.5f, 2.0f, 0.02f, "metres per world unit; moves the eye separation with it", kTabHeadset},
    {"Horizon lock", kRowSliderFloat, kCommandNone, nullptr, nullptr, &g_options.vrHorizonLock,
     0.0f, 1.0f, 0.05f,
     "how much of the car's roll and pitch is taken out of the view. 0 rides with the car, "
     "1 keeps the horizon level. Roll is the axis nobody can counteract with their own head, "
     "so this is the comfort setting to reach for first",
     kTabHeadset},
    {"Gamma", kRowSliderFloat, kCommandNone, nullptr, nullptr, &g_options.vrGamma, 0.6f,
     2.0f, 0.05f, "lifts the mid-tones in the headset", kTabHeadset},
    {"Audio stereo width", kRowSliderFloat, kCommandNone, nullptr, nullptr,
     &g_options.vrHeadAudioWidth, 0.0f, 2.0f, 0.05f,
     "how hard the cockpit's sound pans between your ears as you turn your head. 0 is GPL's own "
     "gentle panning, 1 makes a car 30 degrees off to one side clearly louder in that ear, 2 is "
     "close to hard left and right. Takes effect as you move it",
     kTabHeadset, headAudioWidthAdjustable},

    {"Mirror setup", kRowAction, kCommandMirrorTweak, nullptr, nullptr, nullptr, 0, 0, 0,
     "aim the rear-view glasses and set how wide they see", kTabCockpit},
    {"Pribluda HUD", kRowToggleBool, kCommandNone, &g_pribludaOn, nullptr, nullptr, 0, 1, 1,
     "off skips the HUD pass entirely -- the panel goes from the headset and the monitor, "
     "and its cost goes with it. Runtime only: nothing is written and it is back on next "
     "time the game starts",
     kTabCockpit, pribludaRowOffered},
    {"Pribluda setup", kRowAction, kCommandHudTweak, nullptr, nullptr, nullptr, 0, 0, 0,
     "arrange the elements on pribluda's panel", kTabCockpit, pribludaRowOffered},
    {"Pribluda position", kRowAction, kCommandPanelTweak, nullptr, nullptr, nullptr, 0, 0, 0,
     "move the whole panel, and set how far away and how wide it hangs", kTabCockpit,
     pribludaRowOffered},
    // vrHudLayer 1 is head-locked and 2 is seat-locked, so ON is 1 here: `low` is the value
    // the label is true for.
    {"Head-locked Pribluda", kRowToggleInt, kCommandNone, nullptr, &g_options.vrHudLayer,
     nullptr, 1, 2, 1,
     "on, the panel stays in front of you wherever you look; off, it is fixed to the seat "
     "and you look down at it",
     kTabCockpit, pribludaRowOffered},
    {"Pribluda opacity", kRowSliderFloat, kCommandNone, nullptr, nullptr,
     &g_options.vrHudBackdropOpacity, 0.0f, 1.0f, 0.05f,
     "how much of the world the panel's boxes hide. 0 is clear glass, 1 is solid",
     kTabCockpit, pribludaRowOffered},

    {"Frame rate", kRowNamedInt, kCommandNone, nullptr, &g_options.frameRate, nullptr, 0,
     2, 1,
     "how often the game draws a picture. Vanilla is one per physics step -- the sim's 36, "
     "unblended, as GPL ships. Display is one per screen refresh: the headset's here, the "
     "monitor's in flat mode. Unlimited is as fast as the machine manages. The last two "
     "blend the world between ticks, which is what makes the extra frames new pictures "
     "instead of copies, and costs one tick of latency. Vsync follows this and is not a "
     "separate setting",
     kTabPerformance},
    {"Renderer", kRowNamedInt, kCommandNone, nullptr, &g_options.vrRendererType, nullptr, 0, 2,
     1,
     "who draws the world. Vanilla is GPL's own renderer, as it ships. GPL VR is gplvr's: the "
     "track, the other cars, the flagmen, your cockpit and the mirrors drawn with real depth "
     "and less CPU time, anything it cannot draw left to GPL. GPL VR (debug) is the same with a "
     "red square in the corner of every view it drew. Takes effect on the next frame; the first "
     "switch away from Vanilla in a session prepares the track for a few seconds while GPL "
     "draws it",
     kTabPerformance},
    {"Render scale", kRowSliderFloat, kCommandNone, nullptr, nullptr,
     &g_options.vrRenderScale, 0.5f, 2.0f, 0.05f,
     "NOTE: NEEDS A RESTART. Multiplies the runtime's recommended per-eye size, clamped to "
     "the maximum it reports. The presenter is given this on its command line when it "
     "starts, so changing it here does nothing until you Save settings and restart the game",
     kTabPerformance},
    {"Mirror update rate", kRowRateInt, kCommandNone, nullptr, &g_options.mirrorUpdateRate,
     nullptr, 0, 4, 1,
     "render the mirror on one frame in N and keep the last image on the rest -- the "
     "reflection only changes at the sim's rate anyway. Off goes further: nothing renders "
     "it and the glasses are filled with black",
     kTabPerformance},
    {"Mirrors", kRowNamedInt, kCommandNone, nullptr, &g_options.mirrorMode, nullptr,
     kMirrorSharedHeadCentre, kMirrorPerGlass, 1,
     "how the mirror is rendered, cheapest on the left. Shared, flat renders one view astern "
     "that every glass shows its part of, once from the head centre for both eyes: one "
     "render, but the reflection has no depth and sits on the glass. Shared renders that view "
     "from each eye, so it has depth like the rest of the frame: two renders whatever the car "
     "has, each glass a little softer. Per glass renders each glass on its own from each eye: "
     "the sharpest, two renders per glass every frame. Mirror setup aims each glass in all "
     "three",
     kTabPerformance},
    {"Pribluda update rate", kRowRateInt, kCommandNone, nullptr,
     &g_options.pribludaUpdateRate, nullptr, 1, 4, 1,
     "redraw the panel on one frame in N. The last image stays up in between, so a lower "
     "rate costs nothing but the age of the numbers. No off here -- that is the Pribluda "
     "HUD row on the Cockpit tab",
     kTabPerformance, pribludaRowOffered},

    {"GPL text size", kRowChoiceInt, kCommandNone, nullptr, &g_options.vrGplTextScale, nullptr,
     1, 4, 1,
     "how much bigger GPL's own text is drawn -- the pause prompt, the F1 help line, \"car "
     "must be stopped\". Not its fps counter, which GPL draws onto the window and the headset "
     "never sees. Whole steps, because the letters are tiny bitmaps and 2 makes each dot a 2x2 "
     "block while a half step would come out ragged",
     kTabDisplay},
    {"GPL text inset", kRowSliderFloat, kCommandNone, nullptr, nullptr,
     &g_options.vrGplTextInset, 0.1f, 1.0f, 0.05f,
     "how much of GPL's own distance from the middle survives. 1.00 keeps it at the size GPL "
     "authored, which is already about 10% in from the edge because your eye sees more than "
     "the monitor layout it was drawn for; 0.40 brings a corner line most of the way in. It "
     "moves the line and not the letters in it, so the spacing stays GPL's, and a line that "
     "would land on the one above it is moved down until it clears. Turn it down as you turn "
     "the size up -- a line too big to fit in one eye is dropped rather than spilling into "
     "the other",
     kTabDisplay},
    {"Menu size in headset", kRowSliderFloat, kCommandNone, nullptr, nullptr,
     &g_options.vrMenuScale, 0.05f, 1.0f, 0.05f,
     "how much of the view the game's own menus cover. They are flat images with no world "
     "behind them, so at 1.00 they fill the whole view a few centimetres from your face. "
     "Takes effect as you move it",
     kTabDisplay},
    {"Monitor view", kRowNamedInt, kCommandNone, nullptr, &g_options.vrMonitorEye, nullptr,
     kMonitorOff, kMonitorBoth, 1,
     "what the game window on the monitor shows: off, the left or the right eye, or both eyes "
     "side by side. Off leaves the window black and saves the copy into it. The headset is "
     "unaffected",
     kTabDisplay},
};

constexpr int kRowCount = static_cast<int>(sizeof(g_rows) / sizeof(g_rows[0]));

// The tab on show. It outlives the menu, so the menu reopens where the driver left it.
int g_openTab = kTabHeadset;

// -- which rows are on the page -----------------------------------------------------------
//
// A row keeps its index in g_rows for as long as the process lives, and that index is what
// `hovered` and `pressed` hold, what resetRow takes and what Reset-all walks. What changes is
// the row's LINE: the bands the page is divided into, top to bottom. The buttons share the
// first line with the title, the tabs share the second, and the open tab's rows take one line
// each below them. The layout, the hit test, the stick and the draw all count in lines and
// convert, and nothing that remembers a row has to be told the page changed shape.
//
// THE PAGE IS AS TALL AS ITS TALLEST TAB, whichever tab is open, so switching tabs moves
// nothing that stays: the buttons and the strip keep their place under the pointer, and a
// short tab leaves its lower bands empty.
constexpr int kButtonLine = 0;
constexpr int kTabLine = 1;
constexpr int kFirstRowLine = 2;

bool rowOffered(int row) {
    if (row < 0 || row >= kRowCount) return false;
    return !g_rows[row].offered || g_rows[row].offered();
}

// A setting on `tab` that is on the page, as opposed to a button or a tab of the strip.
bool rowOnTab(int row, int tab) {
    return rowOffered(row) && g_rows[row].kind != kRowTab && g_rows[row].tab == tab;
}

// The line `row` is drawn on, or -1 when it is not on the page: refused, or on a tab that is
// not open.
int rowLine(int row) {
    if (!rowOffered(row)) return -1;
    if (g_rows[row].kind == kRowTab) return kTabLine;
    if (g_rows[row].tab == kTabNone) return kButtonLine;
    if (g_rows[row].tab != g_openTab) return -1;
    int line = kFirstRowLine;
    for (int i = 0; i < row; ++i)
        if (rowOnTab(i, g_openTab)) ++line;
    return line;
}

int lineCount() {
    int tallest = 0;
    for (int tab = 0; tab < kTabCount; ++tab) {
        int rows = 0;
        for (int row = 0; row < kRowCount; ++row)
            if (rowOnTab(row, tab)) ++rows;
        if (rows > tallest) tallest = rows;
    }
    return kFirstRowLine + (tallest > 0 ? tallest : 1);
}

// How many rows share `line`: several on the first two, one or none below them.
int lineWidth(int line) {
    int count = 0;
    for (int row = 0; row < kRowCount; ++row)
        if (rowLine(row) == line) ++count;
    return count;
}

// The `column`th row on `line`, left to right, or -1.
int lineRow(int line, int column) {
    if (column < 0) return -1;
    for (int row = 0; row < kRowCount; ++row) {
        if (rowLine(row) != line) continue;
        if (column-- == 0) return row;
    }
    return -1;
}

int rowColumn(int row) {
    int line = rowLine(row);
    if (line < 0) return -1;
    int column = 0;
    for (int i = 0; i < row; ++i)
        if (rowLine(i) == line) ++column;
    return column;
}

// The part of the page's width a line's rows divide between them, in panel fractions. The
// buttons leave the left of their line to the title.
void lineSpan(int line, float& left, float& right) {
    left = line == kButtonLine ? 0.22f : 0.02f;
    right = 0.98f;
}

// The strip's row for `tab`.
int tabRow(int tab) {
    for (int row = 0; row < kRowCount; ++row)
        if (g_rows[row].kind == kRowTab && g_rows[row].tab == tab) return row;
    return -1;
}

// Where the highlight goes when it has nowhere better: the open tab's first setting, or its
// tab on the strip when the tab has none on the page.
int firstRow() {
    int row = lineRow(kFirstRowLine, 0);
    return row >= 0 ? row : tabRow(g_openTab);
}

void openTab(int tab) {
    if (tab < 0 || tab >= kTabCount || tab == g_openTab) return;
    g_openTab = tab;
    logf("vr menu: %s tab", g_rows[tabRow(tab)].label);
}

// -- what the footer is saying ------------------------------------------------------------
//
// One line, shown in place of the hovered row's help and in a warning colour, for a change the
// menu would not make. It times out rather than needing to be dismissed: a driver mid-race has
// no attention to spend on acknowledging anything.
char   g_notice[200] = {};
double g_noticeUntil = 0.0;

constexpr double kNoticeSeconds = 6.0;

double menuNow();

void notice(const char* text) {
    _snprintf_s(g_notice, sizeof(g_notice), _TRUNCATE, "%s", text);
    g_noticeUntil = menuNow() + kNoticeSeconds;
    logf("vr menu: %s", text);
}

// What a row's new value implies elsewhere. There is nothing here today: the one rule that
// lived here -- that the blend cannot be off while a rate above the sim's is asked for -- was
// removed by deleting the second option, so the illegal pair is not representable rather than
// guarded against. The hook stays because setRowInt and setRowBool are the funnel every writer
// goes through, and the next such rule belongs in exactly one place.
void enforceRowRules(const MenuRow& changed) {
    if (changed.intValue == &g_options.frameRate && g_options.frameRate > 0 &&
        !interpolationActive())
        notice("the blend did not install this session, so a rate above the sim's cannot be "
               "drawn -- the interp: line in gplvr.log says why");
}

// Why a draw produced nothing. Counted rather than logged per frame.
// Why a frame produced no layer. The eye-rect refusals are gone with the eye-rect path:
// a composition layer has no eyes to project into, no frustum to match and no sub-rects
// to fall outside, which is most of why it is the right way to draw this.
enum DrawRefusal {
    kDrawNone = 0,
    kDrawNoViews,     // nothing tracked has been published yet
    kDrawNoGl,        // an entry point the build needs did not resolve
    kDrawNoText,      // the font could not be built
    kDrawNoSeat,      // there has been no recentre, so there is no frame to place it in
    kDrawNoHudPanel,  // HUD tweak, with no HUD panel on screen to annotate
    kDrawNoLayer,     // the layer's texture or its publish refused
    kDrawRefusalCount,
};

const char* drawRefusalName(int refusal) {
    switch (refusal) {
        case kDrawNoViews: return "no tracked frame has been published";
        case kDrawNoGl: return "a GL entry point is missing";
        case kDrawNoText: return "the font could not be built";
        case kDrawNoSeat:
            return "there is no seat yet, so there is no frame to place the panel in -- "
                   "recentre (vrRecentreKey)";
        case kDrawNoHudPanel:
            return "the HUD panel is not on screen, so there is nothing to arrange";
        case kDrawNoLayer:
            return "the menu's layer texture could not be made or published (see the vr: "
                   "and gl: lines)";
        default: return "none";
    }
}

// -- the aiming ray ----------------------------------------------------------------------
//
// Drawn as geometry in the eye images, because there is no composition layer type that
// can carry a line through depth. Its own refusals, because it can fail for reasons the
// panel cannot -- the panel is a layer and needs no eye rects, no frustum and no render
// target, and the ray needs all three.
constexpr float kRayStartOffset = 0.10f;    // metres ahead of the grip
constexpr float kRayMissLength = 1.30f;     // metres, when it is pointing at nothing
constexpr float kRayNearHalfWidth = 0.0035f;  // metres
constexpr float kRayFarHalfWidth = 0.0009f;
constexpr float kRayNearAlpha = 0.80f;
constexpr float kRayFarAlpha = 0.06f;
constexpr int   kRaySegments = 10;

enum RayRefusal {
    kRayNone = 0,
    kRayNoFrustum,
    kRayNoViews,
    kRayNoEyeRect,
    kRayNoSeat,
    kRayNoAim,
    kRayNoTarget,
    kRayNoOffscreen,
    kRayRefusalCount,
};

const char* rayRefusalName(int refusal) {
    switch (refusal) {
        case kRayNoFrustum: return "the eyes did not carry the runtime's frustum";
        case kRayNoViews: return "no tracked frame has been rendered";
        case kRayNoEyeRect: return "the eye sub-rects are not built";
        case kRayNoSeat: return "there is no seat yet -- recentre (vrRecentreKey)";
        case kRayNoAim: return "the aim pose has no direction";
        case kRayNoTarget: return "there is no render target to draw into";
        case kRayNoOffscreen:
            // Unreachable since off-screen rendering stopped being an option, but the target
            // can still fail to build, and then this is what it looks like from here.
            return "the off-screen frame did not build, so there is nowhere to put a ray "
                   "that the monitor would not also show -- the target: lines say why";
        default: return "none";
    }
}

// The canvas is rebuilt at most this often. The cursor is part of the picture, so a hand
// in motion would otherwise rebuild four hundred glyphs on every game frame; 45 a second
// is below what anyone notices on a menu cursor and bounds the cost at a few per cent of
// a tick.
constexpr LONGLONG kMenuRebuildHz = 45;

struct State {
    Mode mode = Mode::Closed;
    int  hand = -1;  // the hand that owns the surface while a mode is open

    // The panel, captured when a mode opens.
    Vec3 panelPosition;
    Quat panelOrientation;
    bool panelValid = false;

    // Where the owner's ray meets the panel, in panel fractions, and whether it does.
    bool  pointerOn = false;
    float pointerU = 0.0f;
    float pointerV = 0.0f;
    int   hovered = -1;
    // In HUD tweak mode, which element's box the ray is inside, or -1. The Select binding
    // takes it, and the draw outlines it.
    int hoveredElement = -1;

    // Sliders and choices are dragged: the press latches the row and the hold keeps
    // applying, so a value follows the pointer instead of stepping once per press.
    int  pressed = -1;
    bool draggingSlider = false;

    // Picking a HUD element up. An element's position is its TOP-LEFT corner, so a drag
    // that simply wrote the pointer there would snap the corner to the ray the moment the
    // grip closed. The offset between the two is captured on that edge and held for the
    // drag, which is what makes the element stay under the point it was grabbed by.
    //
    // `grabDragging` is the second half of that: where the pointer was when the grip
    // closed, and whether it has since travelled kGrabSlop away from there. Until it has,
    // the grip is held but nothing is being moved, and a grip that moves nothing is still
    // a tap.
    bool  grabbed = false;
    bool  grabDragging = false;
    float grabOffsetU = 0.0f;
    float grabOffsetV = 0.0f;
    float grabStartU = 0.0f;
    float grabStartV = 0.0f;

    // Opening on both face buttons, for a controller with no menu click of its own. The
    // combo must be RELEASED before it can fire again or the menu would open and close
    // once a frame.
    // When the grip went down on each hand, and whether that press has already been spent on
    // something else (a drag), so the release can tell a tap from a hold.
    double gripSince[kHands] = {};
    bool   gripConsumed[kHands] = {};

    // True while the stick is held sideways far enough to be adjusting a row. It latches
    // with hysteresis so a hand that shakes below the threshold does not let go.
    bool stickAdjusting = false;

    // The "save changes?" dialog: which mode it is on the way out of, how many values it
    // found changed, and which of the three answers is highlighted.
    Mode confirmFrom = Mode::Menu;
    int  confirmChanged = 0;
    int  confirmChoice = 0;
    int  resetChoice = 0;
    bool menuWasDown[kHands] = {};

    // Stick navigation. The stick is a set of detents, not an axis, for moving the
    // highlight: it has to latch so one push is one row, and repeat while it is held so a
    // long list does not need fifteen pushes.
    int      navDetent = 0;
    LONGLONG navRepeatAt = 0;
    int      adjustDetent = 0;
    LONGLONG adjustRepeatAt = 0;
    // Which of the two owns the highlight. The stick takes it on any move; the pointer
    // takes it back the moment the ray lands on a DIFFERENT row, so neither locks the
    // other out and a driver can switch between them mid-menu.
    bool stickOwnsHighlight = false;
    // Which button the highlight was last on, so the stick comes back up to it.
    int buttonColumn = 0;

    // The front end's pointer.
    bool     pointerHasMouse = false;
    Vec3     pointerLastDirection;
    bool     pointerHaveLast = false;
    LONGLONG pointerMovedAt = 0;
    LONGLONG frequency = 0;
    // A LEFTDOWN this has sent and not yet matched with a LEFTUP. The down and the up are
    // two separate polls, and everything that can take the front end's pointer away
    // happens between polls: the menu opening, the race starting, the controller being
    // put down. Without this the button stays held for the rest of the session.
    bool frontEndButtonDown = false;
    // Over the replay panel: the hand driving the pointer, and where its ray last met the panel.
    // The aiming ray is drawn from these, so a driver can see where the hand points when it is
    // off the panel as well as on it.
    int   replayHand = -1;
    bool  replayHitOn = false;
    float replayHitU = 0.0f;
    float replayHitV = 0.0f;
    // Which side of the front-end boundary the last poll was on, so the crossing can be
    // acted on once rather than tested for every frame by everything that cares.
    bool wasFrontEnd = false;

    // What the canvas currently says. Rebuilt only when this changes: six hundred glyphs
    // a frame is a millisecond of the frame budget and none of it is needed while the
    // driver is reading a menu that has not moved.
    char     canvasSignature[1024] = {};
    bool     canvasSignatureValid = false;
    LONGLONG lastBuildQpc = 0;
    // Whether the presenter is showing the layer. Kept here so closing the menu hides it
    // exactly once instead of writing the flag on every frame it is shut.
    bool layerVisible = false;

    // THE POINTER, SMOOTHED, in fractions of whatever surface it is on -- the front end's
    // control plane or the panel the in-race ray is tested against. One filter serves both
    // because only one of them owns the hand at a time: the VR menu takes the controller
    // from the front end's pointer while it is open and gives it back when it closes, and
    // the menu's own modes each invalidate it as they change surface.
    //
    // `smoothAt` is when it was last advanced, so a gap longer than a frame or two snaps
    // rather than sliding. `smoothRaw` and `smoothSpeed` are the 1-euro filter's second
    // state: the previous raw sample, and the low-passed speed the cutoff is taken from.
    bool     smoothValid = false;
    float    smoothU = 0.0f;
    float    smoothV = 0.0f;
    float    smoothRawU = 0.0f;
    float    smoothRawV = 0.0f;
    float    smoothSpeedU = 0.0f;
    float    smoothSpeedV = 0.0f;
    LONGLONG smoothAt = 0;
    int      lastCursorX = 0;
    int      lastCursorY = 0;
    bool     haveLastCursor = false;

    // What the last poll worked out for the draw, so the draw does no thinking of its own.
    int  drawRefusal = kDrawNone;
    int  lastLoggedRefusal = -1;
    unsigned refusals[kDrawRefusalCount] = {};
    unsigned draws = 0;
    unsigned opens = 0;
    unsigned commands = 0;
    // Did the ray ever reach the panel? "The menu is open and nothing happens when I
    // point at it" and "the ray test never fires" are the same picture from the headset,
    // and only this tells them apart.
    unsigned rayHits = 0;
    unsigned rayMisses = 0;
    unsigned rayDraws = 0;
    unsigned rayRefusals[kRayRefusalCount] = {};
    int      lastLoggedRayRefusal = -1;
};

State g_state;

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

// Seconds, for the one thing that wants a duration rather than a deadline in counts.
double menuNow() {
    LONGLONG ticks = qpcNow();
    if (!g_state.frequency) return 0.0;
    return static_cast<double>(ticks) / static_cast<double>(g_state.frequency);
}

void noteRayRefusal(int refusal) {
    ++g_state.rayRefusals[refusal];
    if (refusal != g_state.lastLoggedRayRefusal) {
        g_state.lastLoggedRayRefusal = refusal;
        if (refusal != kRayNone) logf("vr menu: no aiming ray -- %s", rayRefusalName(refusal));
    }
}

void noteDrawRefusal(int refusal) {
    ++g_state.refusals[refusal];
    g_state.drawRefusal = refusal;
    if (refusal != g_state.lastLoggedRefusal) {
        g_state.lastLoggedRefusal = refusal;
        if (refusal != kDrawNone)
            logf("vr menu: nothing drawn -- %s", drawRefusalName(refusal));
    }
}

// -- the frame everything below lives in: THE SEAT ----------------------------------------
//
// This is the correction the first build got wrong, and it is worth stating plainly.
//
// The presenter publishes head, eye and hand poses in its own LOCAL space. The game does
// NOT render the world in that space. Stereo.cpp composes the eye rotation onto the car's
// (Stereo.cpp, the `composed` loop in the two-pass block):
//
//     camera_orientation = savedOrientation * vr[eye].rotation
//     camera_position    = car_position + savedOrientation * vr[eye].offset
//
// where `savedOrientation` is the car's attitude in GPL world axes and vr[eye] is what
// vrAcquireEyes hands over -- the eye taken RELATIVE TO THE SEAT. Work out where a point
// P, stated in seat axes, lands in camera-local coordinates:
//
//     camera^T * savedOrientation * (P - offset)
//   = vr.rotation^T * savedOrientation^T * savedOrientation * (P - offset)
//   = vr.rotation^T * (P - offset)
//
// savedOrientation CANCELS. A point stated in the seat's frame projects through the
// seat-relative eye and nothing else, which is why a panel stated in that frame sits
// still in the cockpit whatever the car is doing.
//
// A panel stated in LOCAL does not. A head rotation moves the world on screen by that
// rotation conjugated by `car * seat^-1`, and moved a LOCAL panel by the rotation raw;
// the two differ by exactly `car * seat^-1`, which is not identity and changes as the car
// leans. That difference is the roll the first build put on the panel.
//
// So: THE PANEL, THE RAY AND THE EYES ARE ALL IN THE SEAT'S FRAME, in OpenXR's axes. The
// conversion is one conjugation, it is the same one vrSeatRelative does for a hand, and
// nothing here works until the driver has recentred once -- which the draw says out loud
// rather than silently drawing nothing.

// The seat's inverse rotation and its origin, both in the presenter's LOCAL space. False
// before the first recentre.
bool seatInverse(Quat& inverse, Vec3& origin) {
    SharedPose seat;
    if (!vrSeatPose(seat)) return false;
    inverse = conjugate(quatOf(seat));
    origin = positionOf(seat);
    return true;
}

// A LOCAL pose restated in the seat's frame. Kept as a SharedPose so it drops straight
// into the ray test and the projection with no second representation to keep in step.
bool poseInSeat(const SharedPose& pose, SharedPose& out) {
    Quat inverse;
    Vec3 origin;
    if (!seatInverse(inverse, origin)) return false;

    const Vec3 position = rotate(inverse, sub(positionOf(pose), origin));
    // multiply(a, b) applied to v is a(b(v)), so this is the pose's rotation seen from
    // the seat -- the same composition order vrAcquireEyes builds its eye rotation with.
    const Quat orientation = multiply(inverse, quatOf(pose));

    out.positionX = position.x;
    out.positionY = position.y;
    out.positionZ = position.z;
    out.orientationX = orientation.x;
    out.orientationY = orientation.y;
    out.orientationZ = orientation.z;
    out.orientationW = orientation.w;
    return true;
}

// -- surfaces ----------------------------------------------------------------------------
//
// A surface is a rectangle in the SEAT's frame with fractions running u left to right and
// v top to bottom, which is the same convention the HUD panel's texels have and the same
// one a menu row is laid out in. Two of them exist at once in a tweak mode: the one being
// DRAWN (the strip with the readout) and the one being POINTED AT (the HUD panel itself,
// when the mode is placing things on it).

struct Surface {
    bool  valid = false;
    Vec3  centre;
    Vec3  right;
    Vec3  up;
    // The FRONT normal: out of the visible face, back TOWARDS the viewer. A quad layer's
    // visible face looks along its +Z and the menu panel's orientation has its +Z back at
    // the head, so +Z is the front in both cases.
    //
    // The first build stated this as the back normal and then tested for a ray travelling
    // against it -- but a ray leaving the hand travels the same way the BACK normal points,
    // so the test was inverted and could never report a hit. That is why there was never a
    // pointer and never a hovered row: not the drawing, the sign.
    Vec3  normal;
    // The rotation the three axes came from, kept so the quad layer can be placed with it
    // directly. A composition layer wants a pose, not a basis, and re-deriving one from
    // the basis is three chances to get a sign wrong.
    Quat  orientation;
    float width = 0.0f;
    float height = 0.0f;
};

Vec3 surfacePoint(const Surface& surface, float u, float v) {
    return add(surface.centre,
               add(scale(surface.right, (u - 0.5f) * surface.width),
                   scale(surface.up, (0.5f - v) * surface.height)));
}

// Where a hand's ray meets a surface. False when it misses, points away, or runs parallel.
bool rayToSurface(const Surface& surface, const SharedPose& aim, float& u, float& v) {
    if (!surface.valid || surface.width <= 0.0f || surface.height <= 0.0f) return false;

    const Vec3  origin = positionOf(aim);
    const Vec3  direction = rotate(quatOf(aim), {0.0f, 0.0f, -1.0f});
    const float facing = dot(direction, surface.normal);
    // Travelling INTO the visible face: against the front normal, which points back at
    // the viewer. Parallel and from behind both fail here.
    if (facing > -1e-4f) return false;

    // Both terms are negative for a panel in front of the hand -- the centre is away from
    // the viewer along the front normal -- so the quotient is the positive distance.
    const float distance = dot(sub(surface.centre, origin), surface.normal) / facing;
    if (distance <= 0.0f) return false;

    const Vec3 hit = sub(add(origin, scale(direction, distance)), surface.centre);
    u = 0.5f + dot(hit, surface.right) / surface.width;
    v = 0.5f - dot(hit, surface.up) / surface.height;
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

void fillSurface(Surface& surface, const Vec3& centre, const Quat& orientation, float width,
                 float height) {
    surface.valid = true;
    surface.centre = centre;
    surface.orientation = orientation;
    surface.right = rotate(orientation, {1.0f, 0.0f, 0.0f});
    surface.up = rotate(orientation, {0.0f, 1.0f, 0.0f});
    // +Z, the visible face, towards the viewer. See the comment on Surface::normal.
    surface.normal = rotate(orientation, {0.0f, 0.0f, 1.0f});
    surface.width = width;
    surface.height = height;
}

// Put the panel where the head is looking, ONCE, and state it in the seat's frame so it
// then sits still in the cockpit whatever the head and the car do afterwards.
//
// yawOnly still means what it says here: the seat is a heading about the world's up axis,
// so conjugating by it leaves +Y pointing up. A panel captured with the head's pitch and
// roll thrown away is therefore level in the cockpit, not level in some tilted frame.
bool capturePanel() {
    SharedView views[2] = {};
    SharedPose headLocal = {};
    if (!presenterLatestViews(views, headLocal)) return false;

    SharedPose head = {};
    if (!poseInSeat(headLocal, head)) {
        g_state.panelValid = false;
        return false;
    }

    const Quat heading = yawOnly(quatOf(head));
    const Vec3 forward = rotate(heading, {0.0f, 0.0f, -1.0f});
    g_state.panelOrientation = heading;
    g_state.panelPosition = add(positionOf(head), scale(forward, kPanelDistance));
    g_state.panelValid = true;
    return true;
}

// What the mode draws: the menu's panel straight ahead, or a tweak mode's readout strip
// below it, so the strip is not in front of the thing being adjusted.
Surface hudPanelSurface();

Surface drawnSurface() {
    Surface surface;
    // HUD tweak puts the quad ON the HUD panel and annotates it. Everything the driver
    // needs is then in one place and the ray, the outlines and the cursor are all in the
    // panel's own fractions.
    if (g_state.mode == Mode::HudTweak) {
        Surface panel = hudPanelSurface();
        if (panel.valid) return panel;
    }
    if (!g_state.panelValid) return surface;
    // The dialog hangs where the MENU does, not where the strip does: it is a question about
    // the whole mode, it is what the driver is looking at, and a question tucked under the
    // panel at strip size would be easy to miss and hard to read.
    if (g_state.mode == Mode::Menu || g_state.mode == Mode::Confirm ||
        g_state.mode == Mode::Reset) {
        fillSurface(surface, g_state.panelPosition, g_state.panelOrientation, kPanelWidth,
                    kPanelHeight);
        return surface;
    }
    const Vec3 up = rotate(g_state.panelOrientation, {0.0f, 1.0f, 0.0f});
    fillSurface(surface, add(g_state.panelPosition, scale(up, -kStripDrop)),
                g_state.panelOrientation, kStripWidth, kStripHeight);
    return surface;
}

// WHERE THE HUD PANEL ACTUALLY IS, rebuilt from the same numbers the presenter places its
// quad layer from (GplvrXr.cpp, the hudQuad block). Not an approximation of it: the
// position {right, up, -ahead}, the yaw-then-pitch turn toward the eye and the
// width x width*h/w extent are the presenter's own, and the space they are stated in is
// the head (vrHudLayer 1) or the seat (2).
//
// This is what makes "point the trigger at an element and drag it" mean the element the
// driver can see, rather than a proxy of it.
// The HUD quad's placement in the space the PRESENTER puts it in -- the head's for
// vrHudLayer 1, the seat's for 2 -- with no space transform applied. This is the pose a
// layer drawn over that panel has to be published with, in that same space.
bool hudQuadPlacement(Vec3& offset, Quat& turn, float& widthMetres, float& heightMetres,
                      int32_t& space) {
    int texelWidth = 0;
    int texelHeight = 0;
    if (!pribludaPanelSize(texelWidth, texelHeight)) return false;
    if (texelWidth <= 0 || texelHeight <= 0) return false;

    const HudPlacement* placement = nullptr;
    if (g_options.vrHudLayer == 1) {
        placement = &g_options.vrHudHead;
        space = kMenuSpaceView;
    } else if (g_options.vrHudLayer == 2) {
        placement = &g_options.vrHudSeat;
        space = kMenuSpaceSeat;
    } else {
        return false;
    }

    // The producer publishes hudUp as MINUS the placement's `down`, so the sign is taken
    // from there rather than from the option's name.
    const float right = placement->right;
    const float up = -placement->down;
    const float ahead = placement->ahead;

    const float level = std::sqrt(right * right + ahead * ahead);
    const float yaw = std::atan2(-right, ahead);
    const float pitch = std::atan2(up, level);
    const float cy = std::cos(yaw * 0.5f);
    const float sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f);
    const float sp = std::sin(pitch * 0.5f);

    offset = {right, up, -ahead};
    turn = {cy * sp, sy * cp, -sy * sp, cy * cp};
    widthMetres = placement->width;
    heightMetres = widthMetres * static_cast<float>(texelHeight) /
                   static_cast<float>(texelWidth);
    return true;
}

Surface hudPanelSurface() {
    Surface surface;

    int texelWidth = 0;
    int texelHeight = 0;
    if (!pribludaPanelSize(texelWidth, texelHeight)) return surface;
    if (texelWidth <= 0 || texelHeight <= 0) return surface;

    const HudPlacement* placement = nullptr;
    Vec3                spacePosition;
    Quat                spaceOrientation;

    if (g_options.vrHudLayer == 1) {
        // Head-locked: the presenter hangs it in VIEW space, so its frame is the head's
        // own pose, tilt and all -- restated here in the seat's frame, because that is
        // the frame this file works in.
        SharedView views[2] = {};
        SharedPose headLocal = {};
        SharedPose head = {};
        if (!presenterLatestViews(views, headLocal)) return surface;
        if (!poseInSeat(headLocal, head)) return surface;
        spacePosition = positionOf(head);
        spaceOrientation = quatOf(head);
        placement = &g_options.vrHudHead;
    } else if (g_options.vrHudLayer == 2) {
        // Seat-locked, and the seat IS this file's frame -- so the placement's metres are
        // already the coordinates wanted and the space is the identity.
        spacePosition = {0.0f, 0.0f, 0.0f};
        spaceOrientation = {0.0f, 0.0f, 0.0f, 1.0f};
        placement = &g_options.vrHudSeat;
    } else {
        return surface;
    }

    // The producer publishes hudUp as MINUS the placement's `down`, so the sign is taken
    // from there rather than from the option's name.
    const float right = placement->right;
    const float up = -placement->down;
    const float ahead = placement->ahead;

    const float level = std::sqrt(right * right + ahead * ahead);
    const float yaw = std::atan2(-right, ahead);
    const float pitch = std::atan2(up, level);
    const float cy = std::cos(yaw * 0.5f);
    const float sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f);
    const float sp = std::sin(pitch * 0.5f);
    const Quat  quadTurn = {cy * sp, sy * cp, -sy * sp, cy * cp};

    const Vec3 offset = rotate(spaceOrientation, {right, up, -ahead});
    const Vec3 centre = add(spacePosition, offset);
    const Quat orientation = multiply(spaceOrientation, quadTurn);

    const float width = placement->width;
    const float height =
        width * static_cast<float>(texelHeight) / static_cast<float>(texelWidth);

    // A quad layer's visible face looks along its +Z, so out of the face and away from the
    // viewer is -Z -- which is what fillSurface takes.
    fillSurface(surface, centre, orientation, width, height);
    return surface;
}

// What the ray is tested against, and it is ALWAYS what is drawn.
//
// In HUD tweak mode the drawn surface is the HUD panel itself -- the menu's quad is put
// exactly over it and carries the element outlines and the cursor -- so pointing at an
// element means pointing at the element the driver can see, and there is only ever one
// surface in play. The first build had two, one drawn and one pointed at, and could not
// keep them straight.
Surface pointerSurface() { return drawnSurface(); }

// The canvas the mode's content is laid out in. HUD tweak's follows the HUD panel's own
// shape, because its quad is placed over that panel and a canvas of a different aspect
// would stretch the readout across it.
void canvasSizeFor(int& width, int& height) {
    if (g_state.mode == Mode::Menu || g_state.mode == Mode::Confirm ||
        g_state.mode == Mode::Reset) {
        width = kMenuCanvasWidth;
        height = kMenuCanvasHeight;
        return;
    }
    if (g_state.mode == Mode::HudTweak) {
        int panelWidth = 0;
        int panelHeight = 0;
        if (pribludaPanelSize(panelWidth, panelHeight) && panelWidth > 0 &&
            panelHeight > 0) {
            width = kMenuCanvasWidth;
            height = kMenuCanvasWidth * panelHeight / panelWidth;
            if (height < 64) height = 64;
            if (height > 1024) height = 1024;
            return;
        }
    }
    width = kStripCanvasWidth;
    height = kStripCanvasHeight;
}

// -- rows ---------------------------------------------------------------------------------

float helpFooterHeight(int slot, int lines);

// lineCount() bands above a reservation that never changes size. The pointer reads the layout
// from the same two numbers the draw does, so a row is where it looks.
float rowBand() {
    const float footer =
        helpFooterHeight(kVrTextSmall, kHelpLines) / static_cast<float>(kMenuCanvasHeight);
    return (1.0f - footer) / static_cast<float>(lineCount());
}

int rowAt(float u, float v) {
    int line = static_cast<int>(v / rowBand());
    int count = lineWidth(line);
    if (count <= 0) return -1;
    if (line >= kFirstRowLine) return lineRow(line, 0);
    float left = 0.0f;
    float right = 1.0f;
    lineSpan(line, left, right);
    if (u < left || u >= right) return -1;
    float across = (u - left) / (right - left);
    return lineRow(line, static_cast<int>(across * static_cast<float>(count)));
}

// The names a kRowNamedInt's values go by. Keyed on the option the row points at rather than
// carried in the row, because a per-row array of strings would be a second table to keep in
// step with the first.
const char* namedValue(const MenuRow& row) {
    if (row.intValue == &g_options.vrMonitorEye) {
        switch (*row.intValue) {
            case kMonitorOff: return "off";
            case kMonitorLeft: return "left eye";
            case kMonitorRight: return "right eye";
            default: return "both eyes";
        }
    }
    if (row.intValue == &g_options.frameRate) {
        switch (*row.intValue) {
            case 0: return "vanilla";
            // Not "headset": in flat mode it is the monitor, and the value has to mean the
            // same thing in both modes or it is two settings wearing one name.
            case 1: return "display";
            default: return "unlimited";
        }
    }
    if (row.intValue == &g_options.mirrorMode) {
        switch (*row.intValue) {
            case kMirrorShared: return "shared";
            case kMirrorSharedHeadCentre: return "shared, flat";
            default: return "per glass";
        }
    }
    if (row.intValue == &g_options.vrRendererType) {
        switch (*row.intValue) {
            case 0: return "Vanilla";
            case 1: return "GPL VR";
            default: return "GPL VR (debug)";
        }
    }
    return "";
}

void rowValueText(const MenuRow& row, char* out, size_t size) {
    switch (row.kind) {
        case kRowToggleBool:
            _snprintf_s(out, size, _TRUNCATE, "%s",
                        (row.boolValue && *row.boolValue) ? "on" : "off");
            break;
        case kRowToggleInt:
            _snprintf_s(out, size, _TRUNCATE, "%s",
                        (row.intValue && *row.intValue == static_cast<int>(row.low)) ? "on"
                                                                                     : "off");
            break;
        case kRowChoiceInt:
            _snprintf_s(out, size, _TRUNCATE, "%d", row.intValue ? *row.intValue : 0);
            break;
        case kRowNamedInt:
            _snprintf_s(out, size, _TRUNCATE, "%s", namedValue(row));
            break;
        case kRowRateInt: {
            int every = row.intValue ? *row.intValue : 0;
            if (every <= 0)
                _snprintf_s(out, size, _TRUNCATE, "off");
            else if (every == 1)
                _snprintf_s(out, size, _TRUNCATE, "1");
            else
                _snprintf_s(out, size, _TRUNCATE, "1/%d", every);
            break;
        }
        case kRowSliderFloat:
            _snprintf_s(out, size, _TRUNCATE, "%.2f", row.floatValue ? *row.floatValue : 0.0f);
            break;
        default:
            out[0] = '\0';
            break;
    }
}

// A rate row's values in the order the stick and the bar move through them, cheapest first: off
// where the row has it, then one frame in `high`, and so on up to every frame -- so right is
// better on every row of the page. The rank runs 0 to high - low.
int rateRank(const MenuRow& row, int value) {
    int low = static_cast<int>(row.low);
    int high = static_cast<int>(row.high);
    if (value <= 0) return 0;
    if (value > high) value = high;
    return (low <= 0 ? 1 : 0) + high - value;
}

int rateValue(const MenuRow& row, int rank) {
    int low = static_cast<int>(row.low);
    int high = static_cast<int>(row.high);
    int hasOff = low <= 0 ? 1 : 0;
    if (rank < 0) rank = 0;
    if (rank > high - low) rank = high - low;
    if (hasOff && rank == 0) return 0;
    return high - (rank - hasOff);
}

// The row's value as a 0-to-1 position, for the slider bar. -1 for a row that has none.
float rowFraction(const MenuRow& row) {
    if (row.kind == kRowRateInt && row.intValue) {
        int span = static_cast<int>(row.high - row.low);
        if (span <= 0) return -1.0f;
        return static_cast<float>(rateRank(row, *row.intValue)) / static_cast<float>(span);
    }
    if (row.kind == kRowSliderFloat && row.floatValue) {
        const float span = row.high - row.low;
        if (span <= 0.0f) return -1.0f;
        float f = (*row.floatValue - row.low) / span;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }
    if ((row.kind == kRowChoiceInt || row.kind == kRowRateInt ||
         row.kind == kRowNamedInt) && row.intValue) {
        const float span = row.high - row.low;
        if (span <= 0.0f) return -1.0f;
        float f = (static_cast<float>(*row.intValue) - row.low) / span;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }
    return -1.0f;
}

// -- stick navigation --------------------------------------------------------------------
//
// THE PRIMARY WAY IN, and pointing is the alternative rather than the other way round. A
// driver on one controller, with the panel anywhere, must be able to reach every row
// without aiming at anything -- which is what every other VR menu does and what the first
// build did not offer at all.
//
// The stick is treated as a set of detents rather than as an axis: one push is one row,
// and holding it repeats after a pause, exactly as a held arrow key does.
constexpr float  kStickDetent = 0.55f;   // past this, the stick has been pushed
constexpr float  kStickRelease = 0.30f;  // back under this before it can push again
constexpr double kStickRepeatFirst = 0.40;
constexpr double kStickRepeatThen = 0.13;
// A full stick deflection sweeps a slider's whole range in this long, so a wide range and
// a narrow one take the same time to cross and neither needs a per-setting speed.
constexpr float kSliderSweepSeconds = 2.5f;

void enterMode(Mode mode, int hand);
void closeMenu(const char* why);

// One detent of a stick axis, with the latch and the repeat. Returns -1, 0 or +1: the
// step to take THIS poll.
int stickDetent(float value, int& latch, LONGLONG& repeatAt, LONGLONG now,
                LONGLONG frequency) {
    int want = 0;
    if (value >= kStickDetent)
        want = 1;
    else if (value <= -kStickDetent)
        want = -1;

    if (!want) {
        // The release band is narrower than the push band on purpose: a stick resting
        // just under the threshold would otherwise re-arm and repeat on its own noise.
        if (value > -kStickRelease && value < kStickRelease) {
            latch = 0;
            repeatAt = 0;
        }
        return 0;
    }
    if (latch != want) {
        latch = want;
        repeatAt = now + static_cast<LONGLONG>(static_cast<double>(frequency) *
                                               kStickRepeatFirst);
        return want;
    }
    if (repeatAt && now >= repeatAt) {
        repeatAt = now + static_cast<LONGLONG>(static_cast<double>(frequency) *
                                               kStickRepeatThen);
        return want;
    }
    return 0;
}

// -- resetting one row --------------------------------------------------------------------
//
// THE DEFAULT IS A DEFAULT-CONSTRUCTED Options, not a second table of numbers. A table would
// be a copy of the declarations in Hooks.h that nothing keeps in step, and the first time one
// drifted the menu would quietly reset a setting to the wrong value.
//
// A row points at a member of g_options, so the same member of a default-constructed Options
// is at the same offset from its own base. That is what the arithmetic below says: measure
// how far into g_options this row points, and read the same distance into the defaults.
void setRowBool(MenuRow& item, bool value);
void setRowInt(MenuRow& item, int value);

const Options& optionDefaults() {
    static const Options defaults;
    return defaults;
}

template <typename T>
bool defaultOf(const T* member, T& out) {
    auto base = reinterpret_cast<const unsigned char*>(&g_options);
    auto at = reinterpret_cast<const unsigned char*>(member);
    if (at < base || at + sizeof(T) > base + sizeof(Options)) return false;
    auto from = reinterpret_cast<const unsigned char*>(&optionDefaults());
    std::memcpy(&out, from + (at - base), sizeof(T));
    return true;
}

void resetRow(int row) {
    if (row < 0 || row >= kRowCount) return;
    MenuRow& item = g_rows[row];

    // The one row whose value is not an option at all. Its default is "the HUD is on",
    // which is also what every session starts with.
    if (item.boolValue == &g_pribludaOn) {
        setRowBool(item, true);
        notice("Pribluda HUD back on");
        return;
    }

    if (item.boolValue) {
        bool value = false;
        if (defaultOf(item.boolValue, value)) setRowBool(item, value);
    } else if (item.intValue) {
        int value = 0;
        if (defaultOf(item.intValue, value)) setRowInt(item, value);
    } else if (item.floatValue) {
        float value = 0.0f;
        if (defaultOf(item.floatValue, value) && value != *item.floatValue) {
            *item.floatValue = value;
            logf("vr menu: %s back to %.3f", item.label, static_cast<double>(value));
        }
    } else {
        return;  // an action row has nothing to put back
    }

    char text[96] = {};
    char value[64] = {};
    rowValueText(item, value, sizeof(value));
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s back to %s", item.label,
                value[0] ? value : "its default");
    notice(text);
}

void saveSettings();

void runCommand(int command) {
    ++g_state.commands;
    switch (command) {
        case kCommandClose:
            closeMenu(" -- Close");
            break;
        case kCommandRecentre:
            vrRequestRecentre();
            logf("vr menu: recentre asked for");
            break;
        case kCommandMirrorTweak:
            // The three tweak modes aim at things a race has and the front end does not:
            // a car's glasses, and a pribluda panel that only exists while one is being
            // driven. The menu itself is open out here, which is the point, but these
            // rows say why they are not rather than opening onto nothing.
            if (windowInFrontEnd()) {
                notice("the mirrors belong to a car -- start a session first");
                break;
            }
            // The keyboard modes edit the same numbers. One owner at a time, decided
            // here, where a refusal can say so -- rather than inside TweakApi, which the
            // keyboard path itself goes through and would then refuse its own commands.
            if (mirrorTweakActive()) {
                logf("vr menu: the keyboard mirror tweak mode has the aim -- leave it "
                     "(vrMirrorTweakKey) first");
                break;
            }
            enterMode(Mode::MirrorTweak, g_state.hand);
            break;
        case kCommandHudTweak:
            if (windowInFrontEnd()) {
                notice("pribluda's panel is only drawn in a session -- start one first");
                break;
            }
            if (pribludaTweakActive()) {
                logf("vr menu: the keyboard HUD tweak mode has the panel -- leave it "
                     "(vrHudTweakKey) first");
                break;
            }
            enterMode(Mode::HudTweak, g_state.hand);
            break;
        case kCommandPanelTweak:
            if (windowInFrontEnd()) {
                notice("pribluda's panel is only drawn in a session -- start one first");
                break;
            }
            enterMode(Mode::PanelTweak, g_state.hand);
            break;
        case kCommandSaveSettings:
            saveSettings();
            break;
        case kCommandResetSettings:
            g_state.resetChoice = kResetCancel;  // opens on the harmless answer
            enterMode(Mode::Reset, g_state.hand);
            break;
        default:
            break;
    }
}

// -- Save settings -------------------------------------------------------------------------
//
// THE FIRST THING THAT EVER WRITES gplvr.ini, and it writes only what this page can change.
//
// Every other row is a live value that goes back to the file's at the next start, which is
// what makes the menu safe to experiment in -- and useless for keeping anything. One row
// closes that, and it writes THROUGH WritePrivateProfileString, which replaces each value
// where it stands: the file's comments, its order and every key the menu does not touch come
// out unchanged. A rewrite would lose the comments, which are most of that file.
//
// WHAT A TWEAK MODE CHANGES IS NOT HERE. The mirror aim and the pribluda layout live in
// their own files, the HUD panel's placement in its own eight keys of this one, and each
// tweak mode asks about its own on the way out -- so none of them needs this row.
void saveSettings() {
    // The same file loadOptions read, beside gplvr. Writing one in the game's folder instead
    // would succeed, report success, and change nothing anybody reads.
    char path[MAX_PATH] = {};
    pathsOwnFile(path, sizeof(path), "gplvr.ini");

    int  failed = 0;
    auto writeText = [&](const char* key, const char* value) {
        if (!WritePrivateProfileStringA("gplvr", key, value, path)) ++failed;
    };
    // THE LEADING SPACE IS THE VALUE'S. WritePrivateProfileString keeps the text left of the
    // `=` exactly as the file has it and replaces everything right of it, so "1.000" turns a
    // hand-written `vrWorldScale = 1.000` into `vrWorldScale =1.000`, and a file half of
    // whose lines have been saved reads as two files. GetPrivateProfile* skips it on the way
    // back in, and loadOptions has always read the hand-written lines the same way.
    auto writeInt = [&](const char* key, int value) {
        char text[32] = {};
        _snprintf_s(text, sizeof(text), _TRUNCATE, " %d", value);
        writeText(key, text);
    };
    auto writeFloat = [&](const char* key, float value) {
        char text[32] = {};
        _snprintf_s(text, sizeof(text), _TRUNCATE, " %.3f", static_cast<double>(value));
        writeText(key, text);
    };

    // One line per row of the page that has an ini key, in the page's own order. A row added
    // above without a line here saves silently as nothing, which is the one way this can go
    // quietly wrong -- so they are kept in the same order to make the omission visible.
    writeInt("vrHudLayer", g_options.vrHudLayer);
    writeInt("mirrorUpdateRate", g_options.mirrorUpdateRate);
    writeInt("mirrorMode", g_options.mirrorMode);
    writeInt("frameRate", g_options.frameRate);
    writeInt("vrRendererType", g_options.vrRendererType);
    writeInt("pribludaUpdateRate", g_options.pribludaUpdateRate);
    writeFloat("vrRenderScale", g_options.vrRenderScale);
    writeFloat("vrMenuScale", g_options.vrMenuScale);
    writeFloat("vrHudBackdropOpacity", g_options.vrHudBackdropOpacity);
    writeInt("vrGplTextScale", g_options.vrGplTextScale);
    writeFloat("vrGplTextInset", g_options.vrGplTextInset);
    writeFloat("vrGamma", g_options.vrGamma);
    writeFloat("vrHorizonLock", g_options.vrHorizonLock);
    writeFloat("vrWorldScale", g_options.vrWorldScale);
    writeFloat("vrHeadAudioWidth", g_options.vrHeadAudioWidth);
    writeInt("vrMonitorEye", g_options.vrMonitorEye);

    if (failed) {
        notice("some settings could not be written to gplvr.ini -- is the game folder "
               "read-only?");
        logf("vr menu: %d of 16 settings could not be written to %s (error %lu)", failed, path,
             GetLastError());
        return;
    }
    notice("saved to gplvr.ini");
    logf("vr menu: saved to %s -- vrHudLayer=%d mirrorUpdateRate=%d mirrorMode=%d "
         "frameRate=%d vrRendererType=%d pribludaUpdateRate=%d "
         "vrRenderScale=%.3f vrMenuScale=%.3f vrHudBackdropOpacity=%.3f vrGplTextScale=%d "
         "vrGplTextInset=%.3f vrGamma=%.3f vrHorizonLock=%.3f vrWorldScale=%.3f "
         "vrHeadAudioWidth=%.3f vrMonitorEye=%d",
         path, g_options.vrHudLayer, g_options.mirrorUpdateRate, g_options.mirrorMode,
         g_options.frameRate, g_options.vrRendererType,
         g_options.pribludaUpdateRate, g_options.vrRenderScale, g_options.vrMenuScale,
         g_options.vrHudBackdropOpacity, g_options.vrGplTextScale, g_options.vrGplTextInset,
         g_options.vrGamma, g_options.vrHorizonLock, g_options.vrWorldScale,
         g_options.vrHeadAudioWidth, g_options.vrMonitorEye);
}

// Every write to a row's option goes through one of these two, so a rule about what a setting
// may be has one place to live. The pointer drag, the stick and a press all end up here.
void setRowBool(MenuRow& item, bool value) {
    if (!item.boolValue || *item.boolValue == value) return;
    *item.boolValue = value;
    logf("vr menu: %s now %s", item.label, value ? "on" : "off");
    enforceRowRules(item);
}

void setRowInt(MenuRow& item, int value) {
    if (!item.intValue || *item.intValue == value) return;
    *item.intValue = value;
    logf("vr menu: %s now %d", item.label, value);
    enforceRowRules(item);
}

// A press on a row that is not a slider. Sliders are handled by the drag below, because
// dropping a value wherever the pointer happened to be is what an absolute control does.
void activateRow(int row) {
    if (row < 0 || row >= kRowCount) return;
    MenuRow& item = g_rows[row];
    if (item.kind == kRowTab) {
        openTab(item.tab);
        return;
    }
    if (item.kind == kRowAction) {
        runCommand(item.command);
        return;
    }
    if (item.kind == kRowToggleBool && item.boolValue) setRowBool(item, !*item.boolValue);
    if (item.kind == kRowToggleInt && item.intValue)
        setRowInt(item, *item.intValue == static_cast<int>(item.low)
                            ? static_cast<int>(item.high)
                            : static_cast<int>(item.low));
}

// A slider or a choice being dragged: the pointer's position across the row's track IS
// the value, which is the same absolute rule TweakApi uses for a dragged HUD element.
void dragRow(int row, float u) {
    if (row < 0 || row >= kRowCount) return;
    MenuRow& item = g_rows[row];

    // The track occupies the right-hand part of the row, past the label.
    constexpr float kTrackLeft = 0.58f;
    constexpr float kTrackRight = 0.94f;
    float fraction = (u - kTrackLeft) / (kTrackRight - kTrackLeft);
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    if (item.kind == kRowSliderFloat && item.floatValue) {
        float value = item.low + fraction * (item.high - item.low);
        if (item.step > 0.0f)
            value = item.low + std::floor((value - item.low) / item.step + 0.5f) * item.step;
        if (value < item.low) value = item.low;
        if (value > item.high) value = item.high;
        if (value != *item.floatValue) *item.floatValue = value;
        return;
    }
    if (item.kind == kRowRateInt && item.intValue) {
        int span = static_cast<int>(item.high - item.low);
        setRowInt(item, rateValue(item, static_cast<int>(std::floor(fraction * span + 0.5f))));
        return;
    }
    if ((item.kind == kRowChoiceInt || item.kind == kRowNamedInt) && item.intValue) {
        const int low = static_cast<int>(item.low);
        const int high = static_cast<int>(item.high);
        int value = low + static_cast<int>(std::floor(fraction * (high - low) + 0.5f));
        // Snapped to the row's own grain, so the bar lands on a value the stick can also
        // reach and the two ways in cannot disagree about what the settings are.
        int grain = item.step > 1.0f ? static_cast<int>(item.step) : 1;
        if (grain > 1) value = low + (value - low + grain / 2) / grain * grain;
        if (value < low) value = low;
        if (value > high) value = high;
        setRowInt(item, value);
    }
}

// The row the stick lands on when it moves onto `line`: the open tab on the strip, the button
// last highlighted on the button line, the one row anywhere else.
int entryRow(int line) {
    if (line == kTabLine) return tabRow(g_openTab);
    if (line == kButtonLine) {
        int row = lineRow(line, g_state.buttonColumn);
        return row >= 0 ? row : lineRow(line, 0);
    }
    return lineRow(line, 0);
}

// Stick forward and back moves the highlight a line at a time, wrapping at both ends and
// passing over the empty bands under a short tab. Forward is +Y and moves UP the page, which
// is the direction the highlighted row moves on the panel.
void navigateWithStick(const VrHandInput& hand) {
    const int step = stickDetent(hand.stickY, g_state.navDetent, g_state.navRepeatAt,
                                 qpcNow(), g_state.frequency);
    int lines = lineCount();
    if (!step) return;

    int line = rowLine(g_state.hovered);
    if (line < 0) line = kFirstRowLine;
    for (int tried = 0; tried < lines; ++tried) {
        line = ((line - step) % lines + lines) % lines;
        if (lineWidth(line) > 0) break;
    }
    g_state.hovered = entryRow(line);
    // The pointer no longer owns the highlight: a driver who has just moved it with the
    // stick must not have it snatched back by a ray that is still lying across the panel.
    g_state.stickOwnsHighlight = true;
}

// Stick left and right changes the highlighted row's value. A slider sweeps continuously;
// a choice and a toggle step on a detent, because a value with four settings that swept
// would be impossible to land on.
// True while the stick is pushed far enough sideways to count as adjusting, with a lower
// threshold to let go than to take hold. The row being adjusted keeps the highlight until
// this goes false: aiming a controller steadily while pushing its stick is not something a
// hand does, and without the latch the drift moved the highlight onto the next row and the
// same push started editing THAT setting.
bool stickIsAdjusting(const VrHandInput& hand) {
    float magnitude = hand.stickX < 0.0f ? -hand.stickX : hand.stickX;
    if (g_state.stickAdjusting)
        g_state.stickAdjusting = magnitude > kStickRelease;
    else
        g_state.stickAdjusting = magnitude > kStickDetent;
    return g_state.stickAdjusting;
}

void adjustWithStick(const VrHandInput& hand, float seconds) {
    if (g_state.hovered < 0 || g_state.hovered >= kRowCount) return;
    MenuRow& item = g_rows[g_state.hovered];
    const float x = hand.stickX;

    // On the button line and the strip, sideways moves along the line, and on the strip the
    // tab it reaches opens: a driver on the stick alone cannot otherwise see what a tab holds
    // without a trigger pull per tab.
    int line = rowLine(g_state.hovered);
    if (line == kButtonLine || line == kTabLine) {
        int step = stickDetent(x, g_state.adjustDetent, g_state.adjustRepeatAt, qpcNow(),
                               g_state.frequency);
        if (!step) return;
        int column = rowColumn(g_state.hovered) + step;
        int count = lineWidth(line);
        if (column < 0) column = 0;
        if (column >= count) column = count - 1;
        g_state.hovered = lineRow(line, column);
        if (line == kTabLine) openTab(g_rows[g_state.hovered].tab);
        return;
    }

    if (item.kind == kRowSliderFloat && item.floatValue) {
        if (x == 0.0f || seconds <= 0.0f) return;
        // NOT snapped to the step here. A rate this small moves less than one step per
        // poll, and snapping every poll would round it straight back and the slider would
        // never move at all; the step exists for the pointer drag, which lands anywhere.
        float value = *item.floatValue +
                      x * (item.high - item.low) * seconds / kSliderSweepSeconds;
        if (value < item.low) value = item.low;
        if (value > item.high) value = item.high;
        if (value != *item.floatValue) *item.floatValue = value;
        return;
    }

    const int step = stickDetent(x, g_state.adjustDetent, g_state.adjustRepeatAt, qpcNow(),
                                 g_state.frequency);
    if (!step) return;

    if (item.kind == kRowRateInt && item.intValue) {
        setRowInt(item, rateValue(item, rateRank(item, *item.intValue) + step));
        return;
    }
    if ((item.kind == kRowChoiceInt || item.kind == kRowNamedInt) && item.intValue) {
        const int low = static_cast<int>(item.low);
        const int high = static_cast<int>(item.high);
        // A row whose useful values are coarse says so in `step`, so one push is one useful
        // value and not one unit: 0 to 90 in fifteens is seven pushes, not ninety.
        int grain = item.step > 1.0f ? static_cast<int>(item.step) : 1;
        int value = *item.intValue + step * grain;
        // CLAMPED, NOT WRAPPED. It used to wrap so a two-value setting was one push either
        // way -- but the two-value settings are toggles now and have their own kinds, and on
        // a range a wrap means holding the stick left eventually jumps to the maximum. A
        // control that runs out at its end is the one nobody has to watch.
        if (value < low) value = low;
        if (value > high) value = high;
        setRowInt(item, value);
        return;
    }
    if (item.kind == kRowToggleBool && item.boolValue) setRowBool(item, !*item.boolValue);
    if (item.kind == kRowToggleInt && item.intValue)
        setRowInt(item, *item.intValue == static_cast<int>(item.low)
                            ? static_cast<int>(item.high)
                            : static_cast<int>(item.low));
}

// -- the tweak modes ----------------------------------------------------------------------

TweakTarget targetOf(Mode mode) {
    switch (mode) {
        case Mode::MirrorTweak: return TweakTarget::Mirror;
        case Mode::HudTweak: return TweakTarget::HudElement;
        case Mode::PanelTweak: return TweakTarget::HudPanel;
        default: return TweakTarget::None;
    }
}

// -- what the mode found, and what it left ------------------------------------------------
//
// WHY A SNAPSHOT RATHER THAN A DIRTY FLAG. Two questions need answering on the way out -- has
// anything changed, and put it back -- and a flag answers only the first. It would also answer
// it wrongly: a stick nudged and nudged back is not a change, and a value clamped at its limit
// by a held stick would set the flag on every poll for ever.
//
// It is taken through tweakRange, so it needs to know nothing about what a mirror or a HUD
// element is; an axis a target does not have reports `exists` false and is skipped.
constexpr int kSnapshotAxes = static_cast<int>(TweakAxis::Count);
constexpr int kSnapshotIndices = 24;

struct TweakSnapshot {
    bool        valid = false;
    TweakTarget target = TweakTarget::None;
    int         indices = 0;
    bool        exists[kSnapshotIndices][kSnapshotAxes] = {};
    float       value[kSnapshotIndices][kSnapshotAxes] = {};
    // For the mirror: which car the values are of. They are indexed by glass, and the same
    // index on another car is another glass.
    unsigned    mirrorGeneration = 0;
};

TweakSnapshot g_snapshot;

void snapshotTake(TweakTarget target) {
    g_snapshot = TweakSnapshot();
    if (target == TweakTarget::None) return;
    g_snapshot.mirrorGeneration = mirrorConfigGeneration();

    int32_t count = tweakIndexCount(target);
    if (count < 0) count = 0;
    if (count > kSnapshotIndices) count = kSnapshotIndices;

    g_snapshot.target = target;
    g_snapshot.indices = count;
    for (int index = 0; index < count; ++index)
        for (int axis = 1; axis < kSnapshotAxes; ++axis) {
            TweakRange range =
                tweakRange(target, static_cast<TweakAxis>(axis), static_cast<int32_t>(index));
            g_snapshot.exists[index][axis] = range.exists;
            g_snapshot.value[index][axis] = range.current;
        }
    g_snapshot.valid = true;
}

// THE BASELINE FOLLOWS THE CAR. Changing the viewed car discards its unsaved aim (MirrorConfig),
// so what the new car shows IS what its file holds -- which is the baseline for it. Without this
// the old car's values stayed the baseline: Discard wrote them onto the new car's glasses by
// index, and "changed" compared one car against another.
void snapshotFollowCar() {
    if (!g_snapshot.valid || g_snapshot.target != TweakTarget::Mirror) return;
    if (g_snapshot.mirrorGeneration == mirrorConfigGeneration()) return;
    snapshotTake(TweakTarget::Mirror);
}

// How many axes now read differently from what the mode found. Compared with a tolerance in
// the axis's own units, a thousandth of its range, so a float that came back through a clamp
// and a divide does not read as an edit.
int snapshotChanged() {
    snapshotFollowCar();
    if (!g_snapshot.valid) return 0;
    int changed = 0;
    for (int index = 0; index < g_snapshot.indices; ++index)
        for (int axis = 1; axis < kSnapshotAxes; ++axis) {
            if (!g_snapshot.exists[index][axis]) continue;
            TweakRange range = tweakRange(g_snapshot.target, static_cast<TweakAxis>(axis),
                                          static_cast<int32_t>(index));
            if (!range.exists) continue;
            float span = range.maximum - range.minimum;
            float tolerance = span > 0.0f ? span * 0.001f : 1e-4f;
            if (std::fabs(range.current - g_snapshot.value[index][axis]) > tolerance)
                ++changed;
        }
    return changed;
}

void snapshotRestore() {
    snapshotFollowCar();
    if (!g_snapshot.valid) return;
    for (int index = 0; index < g_snapshot.indices; ++index)
        for (int axis = 1; axis < kSnapshotAxes; ++axis) {
            if (!g_snapshot.exists[index][axis]) continue;
            TweakCommand command;
            command.target = g_snapshot.target;
            command.axis = static_cast<TweakAxis>(axis);
            command.mode = TweakMode::Absolute;
            command.value = g_snapshot.value[index][axis];
            command.index = static_cast<int32_t>(index);
            tweakApply(command);
        }
}

bool buttonDown(const VrHandInput& hand, TweakInputButton button) {
    switch (button) {
        case TweakInputButton::Trigger: return hand.triggerHeld.down;
        case TweakInputButton::Squeeze: return hand.squeezeHeld.down;
        case TweakInputButton::StickClick: return hand.stick.down;
        case TweakInputButton::Primary: return hand.primary.down;
        case TweakInputButton::Secondary: return hand.secondary.down;
        case TweakInputButton::Menu: return hand.menu.down;
        default: return false;
    }
}

bool buttonPressed(const VrHandInput& hand, TweakInputButton button) {
    switch (button) {
        case TweakInputButton::Trigger: return hand.triggerHeld.pressed;
        case TweakInputButton::Squeeze: return hand.squeezeHeld.pressed;
        case TweakInputButton::StickClick: return hand.stick.pressed;
        case TweakInputButton::Primary: return hand.primary.pressed;
        case TweakInputButton::Secondary: return hand.secondary.pressed;
        case TweakInputButton::Menu: return hand.menu.pressed;
        default: return false;
    }
}

bool axisValue(const VrHandInput& hand, TweakInputAxis axis, float& value) {
    switch (axis) {
        case TweakInputAxis::StickX: value = hand.stickX; return true;
        case TweakInputAxis::StickY: value = hand.stickY; return true;
        case TweakInputAxis::Trigger: value = hand.trigger; return true;
        case TweakInputAxis::Squeeze: value = hand.squeeze; return true;
        case TweakInputAxis::PointerU:
            if (!g_state.pointerOn) return false;
            value = g_state.pointerU;
            return true;
        case TweakInputAxis::PointerV:
            if (!g_state.pointerOn) return false;
            value = g_state.pointerV;
            return true;
        default: return false;
    }
}

// Walk the mapping. FIRST MATCH WINS per physical control, which is why a row carrying a
// modifier has to precede the unmodified row for the same control -- TweakApi.h says so
// beside the tables, and this loop is the reason.
void applyBindings(Mode mode, const VrHandInput& hand, float seconds) {
    const TweakTarget target = targetOf(mode);
    if (target == TweakTarget::None) return;
    // Before any edit of this poll, so an edit made on the new car is measured against it.
    snapshotFollowCar();

    int32_t             count = 0;
    const TweakBinding* bindings = tweakDefaultBindings(target, count);
    if (!bindings) return;

    bool axisTaken[static_cast<int>(TweakInputAxis::Count)] = {};
    bool buttonTaken[static_cast<int>(TweakInputButton::Count)] = {};

    for (int32_t i = 0; i < count; ++i) {
        const TweakBinding& binding = bindings[i];
        if (binding.modifier != TweakInputButton::None &&
            !buttonDown(hand, binding.modifier))
            continue;

        // A held grip that is DOING something is not a tap, so the release that ends it must
        // not step back out of the mode. It is claimed where the command is applied and not
        // here: the grip is down from the first poll of any press, so claiming it on the
        // modifier alone would mean a tap in HUD tweak mode could never go back at all.
        auto claimGrip = [&]() {
            if (binding.modifier == TweakInputButton::Squeeze && g_state.hand >= 0 &&
                g_state.hand < kHands)
                g_state.gripConsumed[g_state.hand] = true;
        };

        TweakCommand command;
        command.target = target;
        command.axis = binding.axis;
        command.action = binding.action;
        command.mode = binding.mode;
        command.seconds = seconds;
        command.index = -1;

        if (binding.inputButton != TweakInputButton::None) {
            const int slot = static_cast<int>(binding.inputButton);
            if (buttonTaken[slot]) continue;
            if (!buttonPressed(hand, binding.inputButton)) continue;
            buttonTaken[slot] = true;
            // A Select row says "this button selects"; WHAT it selects is whatever the
            // pointer is over, which is the walker's to know and not the table's. Nothing
            // under the pointer means nothing to select, and the row does not fire.
            if (binding.action == TweakAction::Select) {
                if (g_state.hoveredElement < 0) continue;
                command.index = g_state.hoveredElement;
            }
            command.value = binding.scale;
            // The same rule the axis rows below follow: the grip is claimed by a command
            // that changed something, never by one that only ran.
            if (tweakApply(command)) claimGrip();
            continue;
        }

        if (binding.inputAxis == TweakInputAxis::None) continue;
        const int slot = static_cast<int>(binding.inputAxis);
        if (axisTaken[slot]) continue;
        float raw = 0.0f;
        if (!axisValue(hand, binding.inputAxis, raw)) continue;
        axisTaken[slot] = true;
        if (binding.mode != TweakMode::Absolute && raw == 0.0f) continue;
        command.value = raw * binding.scale;

        // A pointer placing a HUD element writes its TOP-LEFT corner, so the grab offset
        // goes back on. Here rather than in TweakApi: where the element was picked up is
        // a fact about this drag, not about what an X axis means.
        //
        // No grab, no drag. Without this, closing the grip over an element that has no
        // position would put the next one's corner wherever the ray happened to be. And
        // no travel, no drag either: until the pointer has moved kGrabSlop from where the
        // grip closed the hand is holding a button still, not dragging anything, so
        // nothing is written and the grip stays a tap.
        if (target == TweakTarget::HudElement && binding.mode == TweakMode::Absolute &&
            (binding.inputAxis == TweakInputAxis::PointerU ||
             binding.inputAxis == TweakInputAxis::PointerV)) {
            if (!g_state.grabbed || !g_state.grabDragging) continue;
            command.value -= binding.inputAxis == TweakInputAxis::PointerU
                                 ? g_state.grabOffsetU
                                 : g_state.grabOffsetV;
        }
        // Only a command that MOVED something claims the grip. tweakApply returns false for a
        // clamp that refused, and a stick already at its limit should not turn a tap into a
        // hold; a drag that actually writes a position does.
        if (tweakApply(command)) claimGrip();
    }
}

// The one-line readout a tweak mode's strip shows.
void tweakSummary(Mode mode, char* out, size_t size) {
    switch (mode) {
        // Which glass the stick is moving, since that is the one thing the glasses themselves
        // do not show.
        case Mode::MirrorTweak:
            _snprintf_s(out, size, _TRUNCATE, "Mirror setup: %s", mirrorConfigSelectedName());
            break;
        // WHAT IS BEING ADJUSTED, not what it currently reads. The numbers were there from
        // the keyboard modes, where there was nothing else to look at; in the headset the
        // thing itself is right in front of the driver and moving as they push the stick,
        // so a row of coordinates is a second, worse copy of what they can already see.
        // The one exception is the element's NAME, which the outline alone does not say.
        case Mode::HudTweak:
            _snprintf_s(out, size, _TRUNCATE, "Pribluda layout: %s",
                        pribludaElementName(pribludaElementSelected()));
            break;
        case Mode::PanelTweak:
            _snprintf_s(out, size, _TRUNCATE, "Pribluda position");
            break;
        default:
            out[0] = '\0';
            break;
    }
}

// -- modes ---------------------------------------------------------------------------------

void closeMenu(const char* why) {
    if (g_state.mode == Mode::Closed) return;
    logf("vr menu: closed%s", why ? why : "");
    g_state.mode = Mode::Closed;
    g_state.hand = -1;
    g_state.hovered = -1;
    g_state.pressed = -1;
    g_state.draggingSlider = false;
    g_state.pointerOn = false;
    g_state.smoothValid = false;
    g_state.panelValid = false;
    g_state.lastLoggedRefusal = -1;
    vrInputRelease("the VR menu");
}

void enterMode(Mode mode, int hand) {
    if (hand < 0) return;
    if (!vrInputClaim(hand, "the VR menu")) return;

    g_state.mode = mode;
    g_state.hand = hand;
    // The open tab's first row, not "nothing". A menu that opens with no highlight has nothing
    // for the trigger to act on until the driver has aimed at it, which is exactly the state
    // a one-controller driver got stuck in.
    g_state.hovered = mode == Mode::Menu ? firstRow() : -1;
    g_state.stickOwnsHighlight = mode == Mode::Menu;
    // The dialog opens on Save: it is what someone who has just spent a minute aiming their
    // mirrors means, and the answer that loses work is the one they have to move to.
    if (mode == Mode::Confirm) g_state.confirmChoice = kConfirmSave;
    g_state.navDetent = 0;
    g_state.navRepeatAt = 0;
    g_state.adjustDetent = 0;
    g_state.adjustRepeatAt = 0;
    g_state.pressed = -1;
    g_state.draggingSlider = false;
    g_state.lastLoggedRefusal = -1;
    // The strip and the panel are different sizes and hang at different heights, so the
    // pose is recaptured: entering a tweak mode with the head turned away would otherwise
    // leave the strip behind the driver. The pointer's filter is forgotten for the same
    // reason -- its state is fractions of the old surface, which are somewhere else on
    // the new one.
    g_state.smoothValid = false;
    // A panel is placed in the SEAT'S frame, and there may never have been a recentre to
    // make one -- which is the ordinary case in the front end, and the case in a race up
    // to the driver's first recentre. Without a seat capturePanel refuses and the menu
    // opens invisibly. The pose they are in now becomes the seat, which is what the
    // HUD-layer key already does when it finds itself in the same position.
    if (!presenterSeatPublished() && vrPublishSeat())
        logf("vr menu: there was no seat to hang the panel in, so this pose is now the "
             "seat -- recentre if it is not where you want it");
    capturePanel();
    ++g_state.opens;

    // What the mode found, so leaving it can say whether anything changed and can put it back.
    //
    // ONE BASELINE PER SESSION OF TWEAKING, not per entry. The dialog does not touch it, and
    // cancelling out of the dialog comes back here for the same target -- taking a fresh
    // snapshot then would adopt the half-finished edits as the baseline and the next question
    // would find nothing changed. Returning to the menu is what ends a session.
    if (mode == Mode::Menu) {
        g_snapshot = TweakSnapshot();
    } else if (mode != Mode::Confirm && mode != Mode::Reset) {
        TweakTarget target = targetOf(mode);
        if (!g_snapshot.valid || g_snapshot.target != target) snapshotTake(target);
    }

    switch (mode) {
        case Mode::Menu: {
            logf("vr menu: open, %s hand -- stick moves the highlight, stick sideways "
                 "changes the value, trigger selects, a grip tap closes. Pointing at "
                 "a row works too.",
                 hand == kHandLeft ? "left" : "right");
            // WHICH CONTROLS THAT HAND ACTUALLY HAS. A runtime that fell back to the
            // simple controller profile binds no stick at all, and "the stick does
            // nothing" then has one cause and one line saying so, rather than being
            // indistinguishable from a menu that is not listening.
            const VrHandInput& owner = vrInputHand(hand);
            const bool         hasStick = owner.stickX != 0.0f || owner.stickY != 0.0f;
            logf("vr menu: that hand reports trigger %.2f, grip %.2f, stick (%.2f, %.2f)%s",
                 owner.trigger, owner.squeeze, owner.stickX, owner.stickY,
                 hasStick ? "" : " -- a stick reading exactly zero at rest is normal; if "
                                 "it never moves, the runtime bound no thumbstick and the "
                                 "highlight can only be aimed at");
            break;
        }
        case Mode::MirrorTweak:
            logf("vr menu: mirror aim from the %s hand -- %s",
                 hand == kHandLeft ? "left" : "right",
                 "stick aims, trigger+stick widens, face buttons pick a glass, stick click "
                 "resets, menu leaves");
            break;
        case Mode::HudTweak:
            logf("vr menu: HUD layout from the %s hand -- %s",
                 hand == kHandLeft ? "left" : "right",
                 "trigger picks up, grip drags, stick nudges, stick click resets, menu "
                 "leaves");
            break;
        case Mode::PanelTweak:
            logf("vr menu: HUD panel placement from the %s hand -- %s",
                 hand == kHandLeft ? "left" : "right",
                 "stick moves it, trigger+stick pushes it away, stick click resets, menu "
                 "leaves");
            break;
        default:
            break;
    }
}

// The gesture that opens the menu, on either hand. The menu click where there is one; both
// face buttons together where there is not, because the Valve Index binds no menu at all
// and its system button is the runtime's.
// ONE BUTTON, AND IT IS THE GRIP. Opening with two face buttons together was not something
// anybody would guess, and a `menu` click does not exist on every controller.
//
// A TAP opens the menu and steps back out of it; a HOLD is left alone, because the HUD drag is
// "hold the grip and move the pointer" and the two have to share the control. The tap is
// decided on RELEASE: down, then up again inside kGripTapSeconds, having done nothing else.
constexpr double kGripTapSeconds = 0.35;

// True on the release that ends a short press. `since` is the hand's own press time.
bool gripTapped(const VrHandInput& input, double& since, bool& consumed) {
    if (input.squeezeHeld.pressed) {
        since = menuNow();
        consumed = false;
    }
    if (input.squeezeHeld.released) {
        bool tap = !consumed && since > 0.0 && menuNow() - since <= kGripTapSeconds;
        since = 0.0;
        consumed = false;
        return tap;
    }
    return false;
}

int openingHand() {
    for (int hand = 0; hand < kHands; ++hand) {
        const VrHandInput& input = vrInputHand(hand);
        if (!input.present) {
            g_state.gripSince[hand] = 0.0;
            g_state.gripConsumed[hand] = false;
            continue;
        }
        if (gripTapped(input, g_state.gripSince[hand], g_state.gripConsumed[hand]))
            return hand;
    }
    return -1;
}

// RECENTRE WITHOUT THE MENU, because it is the one thing a driver wants mid-lap and opening a
// panel to reach it defeats the point. Any of the two face buttons or the stick click, on a
// hand that is tracked, while nothing is open. Inside a mode those same controls belong to the
// mode -- the stick click is Reset in all three binding tables -- so this only looks when the
// menu is closed, which is also the only time it is called. `stickClick` false leaves the stick
// click to the replay controls, whose play/pause it is.
void pollQuickRecentre(bool stickClick) {
    for (int hand = 0; hand < kHands; ++hand) {
        const VrHandInput& input = vrInputHand(hand);
        if (!input.present) continue;
        if (!input.primary.pressed && !input.secondary.pressed &&
            !(stickClick && input.stick.pressed))
            continue;
        vrRequestRecentre();
        logf("vr menu: recentred from the controller, with the menu closed");
        return;
    }
}

// -- "save changes?" -----------------------------------------------------------------------

const char* const kConfirmLabel[kConfirmCount] = {"Save", "Discard", "Keep tweaking"};

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::MirrorTweak: return "mirror aim";
        case Mode::HudTweak: return "HUD layout";
        case Mode::PanelTweak: return "HUD panel";
        default: return "the menu";
    }
}

void answerConfirm(int choice) {
    State& state = g_state;
    Mode   from = state.confirmFrom;
    TweakTarget target = targetOf(from);

    if (choice == kConfirmCancel) {
        // Back into the mode with the SAME baseline. enterMode keeps a snapshot that is
        // already for this target, so the edits made so far stay visible to the next
        // question: cancelling continues one session of tweaking, it does not start another.
        enterMode(from, state.hand);
        logf("vr menu: kept tweaking");
        return;
    }

    if (choice == kConfirmSave) {
        // Each mode writes its own: the mirror aim to gplvr_mirrors.ini, the HUD layout to
        // gplvr_pribluda.ini, the panel's placement to gplvr.ini. None of them needs the
        // page's Save settings button, which writes a different set of keys entirely.
        TweakCommand save;
        save.target = target;
        save.action = TweakAction::Commit;
        bool wrote = tweakApply(save);
        logf("vr menu: %s -- %s", modeName(from),
             wrote ? "saved" : "NOT saved, and the tweak: line above says why");
    } else {
        snapshotRestore();
        logf("vr menu: %s -- discarded, back to what it was on the way in", modeName(from));
    }
    enterMode(Mode::Menu, state.hand);
}

void answerReset(int choice) {
    State& state = g_state;
    if (choice == kResetCancel) {
        enterMode(Mode::Menu, state.hand);
        return;
    }

    bool all = choice == kResetEverything;
    char done[192] = {};
    int  written = 0;

    if (all || choice == kResetVrSettings) {
        // The live values only. Writing gplvr.ini here would be a second, hidden save, and
        // the page already has one row that does that and says so.
        for (int row = 0; row < kRowCount; ++row) resetRow(row);
        written += _snprintf_s(done, sizeof(done), _TRUNCATE, "settings");
    }
    if (all || choice == kResetMirrors) {
        bool ok = mirrorConfigClearAll();
        written += _snprintf_s(done + written, sizeof(done) - written, _TRUNCATE, "%s%s",
                               written ? ", " : "", ok ? "mirror presets" : "MIRRORS FAILED");
    }
    if (all || choice == kResetPribluda) {
        bool ok = pribludaResetLayout();
        written += _snprintf_s(done + written, sizeof(done) - written, _TRUNCATE, "%s%s",
                               written ? ", " : "", ok ? "Pribluda layout" : "PRIBLUDA FAILED");
    }

    char text[256] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE,
                "reset: %s. The settings are live only -- Save settings keeps them", done);
    enterMode(Mode::Menu, state.hand);
    notice(text);
}

void pollReset(const VrHandInput& hand) {
    State& state = g_state;
    int step = stickDetent(hand.stickY, state.navDetent, state.navRepeatAt, qpcNow(),
                           state.frequency);
    if (step) {
        state.resetChoice =
            ((state.resetChoice - step) % kResetCount + kResetCount) % kResetCount;
        state.stickOwnsHighlight = true;
    }
    int pointed = -1;
    if (state.pointerOn) {
        int row = static_cast<int>(state.pointerV * static_cast<float>(kResetCount + 1)) - 1;
        if (row >= 0 && row < kResetCount) pointed = row;
    }
    if (pointed >= 0 && !step) state.resetChoice = pointed;
    // The trigger takes the answer the ray is on; the stick click takes the highlighted one.
    if (hand.triggerHeld.released && pointed >= 0 && pointed == state.resetChoice)
        answerReset(state.resetChoice);
    else if (hand.stick.pressed)
        answerReset(state.resetChoice);
}

void pollConfirm(const VrHandInput& hand) {
    State& state = g_state;

    // The stick walks the three answers and its click takes one. Pointing works too, and
    // the rows are laid out by the same rowBand the menu uses, so the hit test is the same.
    int step = stickDetent(hand.stickY, state.navDetent, state.navRepeatAt, qpcNow(),
                           state.frequency);
    if (step) {
        state.confirmChoice =
            ((state.confirmChoice - step) % kConfirmCount + kConfirmCount) % kConfirmCount;
        state.stickOwnsHighlight = true;
    }
    // The dialog's own grid, not the menu's: it has three bands and a title, the menu has
    // fourteen and a footer, and the draw below lays this one out the same way.
    int pointed = -1;
    if (state.pointerOn) {
        int row = static_cast<int>(state.pointerV * static_cast<float>(kConfirmCount + 1)) - 1;
        if (row >= 0 && row < kConfirmCount) pointed = row;
    }
    if (pointed >= 0 && !step) state.confirmChoice = pointed;
    // The trigger takes the answer the ray is on; the stick click takes the highlighted one.
    if (hand.triggerHeld.released && pointed >= 0 && pointed == state.confirmChoice)
        answerConfirm(state.confirmChoice);
    else if (hand.stick.pressed)
        answerConfirm(state.confirmChoice);
}

// -- the front end's pointer -----------------------------------------------------------

// Only while the hand is doing something. Otherwise a controller resting on the desk would
// pin the pointer to a corner and the mouse would be unusable.
bool pointerAwake(const VrHandInput& hand, const Vec3& direction) {
    bool moving = hand.triggerHeld.down || hand.squeezeHeld.down || hand.primary.down ||
                  hand.secondary.down || hand.stick.down || hand.stickX != 0.0f ||
                  hand.stickY != 0.0f;
    if (g_state.pointerHaveLast && !moving) {
        // The angle between this frame's ray and the last one's. A hand held still reads
        // as a few tenths of a milliradian of tracking noise, which is well under the
        // threshold.
        const Vec3  previous = g_state.pointerLastDirection;
        const float cosine = dot(previous, direction);
        moving = cosine < std::cos(kPointerWakeRadians);
    }
    g_state.pointerLastDirection = direction;
    g_state.pointerHaveLast = true;

    const LONGLONG now = qpcNow();
    if (moving) g_state.pointerMovedAt = now;
    if (!g_state.pointerMovedAt) return false;
    const double idle = static_cast<double>(now - g_state.pointerMovedAt) /
                        static_cast<double>(g_state.frequency);
    return idle < kPointerIdleSeconds;
}

// Where the aim ray meets the control plane, in 0 to 1 fractions of it, clamped to its
// edges. `aspect` is the front end's own width over height, so the plane has the shape of
// the thing being pointed at and a diagonal sweep of the hand is a diagonal sweep of the
// pointer.
//
// /input/aim/pose is the right input and the grip pose is not: OpenXR specifies aim as
// the POINTING pose, with -Z along the device's own aiming ray, while grip follows how
// the hand holds the controller. Both are sampled; this uses aim.
bool frontEndRay(const SharedPose& head, const SharedPose& aim, float aspect, float& u,
                 float& v) {
    const Quat headInverse = conjugate(quatOf(head));
    // Into the head's frame: the plane is head-locked, because the front end's image is
    // -- the presenter puts it on a projection layer, which moves with the head.
    const Vec3 origin = rotate(headInverse, sub(positionOf(aim), positionOf(head)));
    const Vec3 direction = rotate(headInverse, rotate(quatOf(aim), {0.0f, 0.0f, -1.0f}));

    // Pointing along the plane, away from it, or from beyond it: there is nothing to hit,
    // and the caller keeps the pointer where it was rather than throwing it at a corner.
    if (direction.z > -0.05f) return false;
    const float distance = (-kFrontEndDistance - origin.z) / direction.z;
    if (distance <= 0.0f) return false;

    const float hitX = origin.x + direction.x * distance;
    const float hitY = origin.y + direction.y * distance;

    const float halfWidth = kFrontEndHalfWidth;
    const float halfHeight =
        aspect > 0.01f ? kFrontEndHalfWidth / aspect : kFrontEndHalfWidth;

    u = 0.5f + hitX / (2.0f * halfWidth);
    // The plane's centre is kFrontEndUpwardBias below the eye line, so a hit at that
    // height is the middle of the image.
    v = 0.5f - (hitY + kFrontEndUpwardBias) / (2.0f * halfHeight);

    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return true;
}

// A one-pole low-pass, over an elapsed time rather than a poll: at a cutoff of c hertz a
// step of `seconds` moves this fraction of the way to the new sample. Stating it as an
// exponential rather than as a constant per-frame alpha is what makes 45 Hz and 90 Hz
// behave the same.
float lowPassAlpha(float cutoffHz, float seconds) {
    return 1.0f - std::exp(-6.2831853f * cutoffHz * seconds);
}

// THE ONE POINTER FILTER. Every pointer in this file goes through it -- the front end's
// mapped position and the in-race ray's hit on the panel -- so there is one behaviour to
// tune and one set of constants at the top of the file to tune it with.
//
// A 1-euro filter, as the constants describe: the speed of the raw signal is low-passed,
// the cutoff is taken from that speed, and the position is low-passed at that cutoff. The
// dead band on top of it is what stops a still hand shaking; the moving cutoff is what
// stops a moving one lagging.
//
// `rawU`, `rawV` and the outputs are all fractions of the surface being pointed at, and
// the caller invalidates smoothValid whenever that surface changes, so the filter never
// slides a hit across two rectangles that are not the same size or in the same place.
void smoothPointer(float rawU, float rawV, float& outU, float& outV) {
    State&         state = g_state;
    const LONGLONG now = qpcNow();
    const double   seconds =
        (state.smoothValid && state.smoothAt)
              ? static_cast<double>(now - state.smoothAt) /
                  static_cast<double>(state.frequency)
              : 0.0;
    state.smoothAt = now;

    if (!state.smoothValid || seconds <= 0.0 || seconds > kPointerSnapSeconds) {
        state.smoothValid = true;
        state.smoothU = rawU;
        state.smoothV = rawV;
        state.smoothRawU = rawU;
        state.smoothRawV = rawV;
        state.smoothSpeedU = 0.0f;
        state.smoothSpeedV = 0.0f;
        outU = rawU;
        outV = rawV;
        return;
    }

    float step = static_cast<float>(seconds);

    // The speed of the RAW signal, low-passed. The cutoff below follows this and not the
    // raw difference: a hand held still produces a raw speed that is all tremor, and a
    // cutoff driven by it would open up on exactly the hand that needs filtering most.
    float speedAlpha = lowPassAlpha(kPointerSpeedCutoffHz, step);
    state.smoothSpeedU +=
        speedAlpha * ((rawU - state.smoothRawU) / step - state.smoothSpeedU);
    state.smoothSpeedV +=
        speedAlpha * ((rawV - state.smoothRawV) / step - state.smoothSpeedV);
    state.smoothRawU = rawU;
    state.smoothRawV = rawV;

    float speed = std::sqrt(state.smoothSpeedU * state.smoothSpeedU +
                            state.smoothSpeedV * state.smoothSpeedV);
    float cutoff = kPointerMinCutoffHz + kPointerSpeedToCutoff * speed;
    if (cutoff > kPointerMaxCutoffHz) cutoff = kPointerMaxCutoffHz;

    float du = rawU - state.smoothU;
    float dv = rawV - state.smoothV;
    if (std::sqrt(du * du + dv * dv) < kPointerDeadband) {
        // Inside the dead band nothing moves at all. Tracking noise on a still hand lives
        // here, and a filter alone cannot remove it -- it only slows it down, which reads
        // as the pointer drifting rather than as it shaking. The speed state above is kept
        // up to date through it, so the cutoff is right on the first sample that leaves.
        outU = state.smoothU;
        outV = state.smoothV;
        return;
    }

    float alpha = lowPassAlpha(cutoff, step);
    state.smoothU += alpha * du;
    state.smoothV += alpha * dv;
    outU = state.smoothU;
    outV = state.smoothV;
}

void frontEndPointerRelease(const char* why);

// THE REPLAY SCREEN IS CLICKABLE AND IS NOT THE FRONT END.
//
// menuScaleClientRect only answers for the scaled 640x480 front end. In a replay the controls
// are on the replay panel (ReplayPanel.h), a quad of their own in the seat's frame, so the ray is
// tested against that quad and the hit is turned back into the window point GPL drew it at.
// Mapping the ray onto the whole client area instead put the pointer somewhere unrelated to
// where the buttons could be seen, and none of them could be pressed.
Surface replayPanelSurface() {
    Surface surface;
    float   centre[3];
    float   orientation[4];
    float   width = 0.0f;
    float   height = 0.0f;
    replayPanelPlacement(centre, orientation, width, height);
    fillSurface(surface, {centre[0], centre[1], centre[2]},
                {orientation[0], orientation[1], orientation[2], orientation[3]}, width, height);
    return surface;
}

void pollFrontEndPointer() {
    // The scaled front end is what knows where the 640x480 menu sits inside the window; the
    // replay panel knows where its bands are. Without either there is nothing to map into, and
    // that is a silent nothing unless it says so once.
    static bool reportedNoRect = false;
    static bool reportedReplay = false;
    RECT        rect = {};
    bool        onPanel = false;
    if (!menuScaleClientRect(rect)) {
        onPanel = !windowInFrontEnd() && replayPanelShown();
        if (!onPanel) {
            if (!reportedNoRect) {
                reportedNoRect = true;
                logf("vr menu: the front end is not being scaled (MenuScale is off or its "
                     "framebuffer did not build) and the replay panel is not up, so the "
                     "controller cannot drive a pointer");
            }
            // A button pressed over the panel is still let go when the panel goes.
            if (g_state.frontEndButtonDown) frontEndPointerRelease("the replay panel went away");
            return;
        }
        if (!reportedReplay) {
            reportedReplay = true;
            logf("vr menu: a replay is being watched, so the controller's ray drives the pointer "
                 "over the replay panel");
        }
    }
    reportedNoRect = false;

    SharedView views[2] = {};
    SharedPose head = {};
    if (!presenterLatestViews(views, head)) return;

    // Either hand, THE ONE THAT IS MOVING FIRST. Ownership is not claimed for this -- there is
    // nothing for two hands to fight over when both write the same single pointer position --
    // but which one is asked first decides who wins while both are tracked, and a fixed
    // right-first order meant a left-handed driver's pointer was dead whenever the right
    // controller was awake on the desk. Awake is the same test the loop below applies.
    int order[kHands] = {kHandRight, kHandLeft};
    for (int i = 0; i < kHands; ++i) {
        const VrHandInput& candidate = vrInputHand(order[i]);
        if (!candidate.present) continue;
        if (!pointerAwake(candidate, rotate(quatOf(candidate.aimPose), {0.0f, 0.0f, -1.0f})))
            continue;
        order[0] = order[i];
        order[1] = order[i] == kHandRight ? kHandLeft : kHandRight;
        break;
    }
    for (int i = 0; i < kHands; ++i) {
        const VrHandInput& hand = vrInputHand(order[i]);
        if (!hand.present) continue;

        const Vec3 direction = rotate(quatOf(hand.aimPose), {0.0f, 0.0f, -1.0f});
        const bool awake = pointerAwake(hand, direction);
        if (onPanel) {
            // The ray follows this hand while it is awake, on the panel or not.
            g_state.replayHand = awake ? order[i] : -1;
            if (!awake) g_state.replayHitOn = false;
        }
        if (!awake && g_state.pointerHasMouse) {
            g_state.pointerHasMouse = false;
            g_state.smoothValid = false;
            logf("vr menu: the controller stopped moving -- the mouse has the front end's "
                 "pointer again");
        }

        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        // MOVING THE POINTER AND RELEASING THE BUTTON ARE SEPARATE. Every path that
        // refuses to move it falls through to the click below instead of returning, or a
        // ray that goes bad between the trigger going down and coming up would leave the
        // left button held for the rest of the session.
        bool onTarget = false;
        if (awake && (onPanel || (width > 0 && height > 0))) {
            const float aspect =
                onPanel ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
            float rawU = 0.0f;
            float rawV = 0.0f;
            // The replay panel is a quad in the seat's frame, so the ray goes there too; the
            // front end's plane is head-locked.
            SharedPose aimSeat = {};
            bool       hit = onPanel ? poseInSeat(hand.aimPose, aimSeat) &&
                                     rayToSurface(replayPanelSurface(), aimSeat, rawU, rawV)
                                     : frontEndRay(head, hand.aimPose, aspect, rawU, rawV);
            if (onPanel) g_state.replayHitOn = hit;
            if (!hit) {
                // Off the panel the filter has nothing to carry forward, as with the menu's.
                if (onPanel) g_state.smoothValid = false;
            } else {
                float u = rawU;
                float v = rawV;
                smoothPointer(rawU, rawV, u, v);
                if (onPanel) {
                    g_state.replayHitU = u;
                    g_state.replayHitV = v;
                }

                POINT point;
                bool  placed = true;
                if (onPanel) {
                    placed = replayPanelClientPoint(u, v, point);
                } else {
                    point.x = rect.left + static_cast<LONG>(u * static_cast<float>(width));
                    point.y = rect.top + static_cast<LONG>(v * static_cast<float>(height));
                }

                HWND window = gameWindow();
                if (placed && window && ClientToScreen(window, &point)) {
                    onTarget = true;
                    // Only when the whole pixel changed. A sub-pixel difference would
                    // still be a SetCursorPos every frame, and the pointer would sit on a
                    // boundary and flicker between two positions -- which is the shake,
                    // at the very last step.
                    if (!g_state.haveLastCursor || point.x != g_state.lastCursorX ||
                        point.y != g_state.lastCursorY) {
                        SetCursorPos(point.x, point.y);
                        g_state.lastCursorX = point.x;
                        g_state.lastCursorY = point.y;
                        g_state.haveLastCursor = true;
                    }

                    if (!g_state.pointerHasMouse) {
                        g_state.pointerHasMouse = true;
                        if (onPanel)
                            logf("vr menu: the %s controller now drives the pointer over the "
                                 "replay panel",
                                 order[i] == kHandLeft ? "left" : "right");
                        else
                            logf("vr menu: the %s controller now drives the front end's "
                                 "pointer -- an aim ray onto a %.2f x %.2f m plane %.1f m "
                                 "ahead, biased %.2f m up",
                                 order[i] == kHandLeft ? "left" : "right",
                                 kFrontEndHalfWidth * 2.0f, kFrontEndHalfWidth * 2.0f / aspect,
                                 kFrontEndDistance, kFrontEndUpwardBias);
                    }
                }
            }
        }

        // The click. SendInput rather than posting a message, so the game sees exactly
        // what a mouse would -- its own WM_LBUTTONDOWN with the position in lParam, which
        // the window proc already remaps. This mechanism is proven in game; only where
        // the pointer is put changed.
        //
        // The down is REMEMBERED. Whatever happens between this poll and the release --
        // the menu opening on the other hand, the race starting, the controller going to
        // sleep -- frontEndPointerRelease finds the button still down and lets it go.
        //
        // Over the replay panel a press counts only when the ray is on the panel: the pointer
        // stays where it last was when the ray leaves, and a press aimed at the world would
        // click whichever button that is. The release always goes.
        bool press = hand.triggerHeld.pressed && (!onPanel || onTarget);
        bool release = hand.triggerHeld.released && g_state.frontEndButtonDown;
        if (press || release) {
            INPUT click = {};
            click.type = INPUT_MOUSE;
            click.mi.dwFlags = press ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            SendInput(1, &click, sizeof(click));
            g_state.frontEndButtonDown = press;
        }
        return;
    }
}

// EVERYTHING THE FRONT END'S POINTER LEAVES BEHIND, in one place, so every way of losing it
// unwinds the same way. There are four -- the VR menu opening, the race starting, the
// controllers going away and the hand being put down -- and each used to clear its own
// subset of this.
//
// The mouse button is the one that matters. A trigger held while the pointer changes hands
// has already sent its LEFTDOWN, and the LEFTUP is sent by a poll that will not run: the
// game is then left with the left button down for the rest of the session, clicking
// whatever the real mouse touches.
void frontEndPointerRelease(const char* why) {
    State& state = g_state;
    // Idempotent, so every caller can say it unconditionally. It clears the pointer
    // filter, and a race frame calling it every poll would reset the in-race ray's
    // smoothing on every sample.
    if (!state.frontEndButtonDown && !state.pointerHasMouse && !state.pointerHaveLast &&
        !state.haveLastCursor)
        return;

    if (state.frontEndButtonDown) {
        INPUT click = {};
        click.type = INPUT_MOUSE;
        click.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &click, sizeof(click));
        state.frontEndButtonDown = false;
        logf("vr menu: the front end's pointer had the left button down when it %s -- "
             "released it, so it does not stay held",
             why);
    }

    if (state.pointerHasMouse) {
        state.pointerHasMouse = false;
        logf("vr menu: the mouse has the front end's pointer again -- %s", why);
    }
    state.pointerHaveLast = false;
    state.haveLastCursor = false;
    state.smoothValid = false;
}

// -- the canvas's content ------------------------------------------------------------
//
// Everything here is in canvas pixels with the origin top-left, which is the space
// vrMenuUpdate sets up over the layer's own texture. The cursor is drawn here too -- the
// layer is the only surface there is once the menu is a composition layer -- which is
// why the rebuild is rate-limited rather than left to the frame rate.

// The text colour of the page's Reset button and of Reset ALL, light enough to read on the
// highlight's blue as well as on the panel.
void setResetColour(const GlFunctions& gl) { gl.color4f(1.0f, 0.42f, 0.38f, 1.0f); }

void canvasQuad(const GlFunctions& gl, float x0, float y0, float x1, float y1) {
    gl.begin(GL_QUADS);
    gl.vertex2f(x0, y0);
    gl.vertex2f(x1, y0);
    gl.vertex2f(x1, y1);
    gl.vertex2f(x0, y1);
    gl.end();
}

void canvasOutline(const GlFunctions& gl, float x0, float y0, float x1, float y1) {
    gl.begin(GL_LINE_LOOP);
    gl.vertex2f(x0, y0);
    gl.vertex2f(x1, y0);
    gl.vertex2f(x1, y1);
    gl.vertex2f(x0, y1);
    gl.end();
}

// A line of text with its baseline placed so the cap height straddles `centreY` rather
// than hanging from it.
void canvasText(const GlFunctions& gl, int slot, float x, float centreY, const char* text) {
    vrTextDraw(gl, slot, x, centreY + static_cast<float>(vrTextAscent(slot)) * 0.5f, text);
}

// The one-line readout a tweak mode's strip shows, and the menu's help line, both built
// here so the signature below and the draw cannot disagree about what is on the canvas.
// The name the RUNTIME gives a control on the owning hand, or "" when it has not said.
//
// OpenXR has no glyph or icon API -- nothing in the headers returns a picture of a button
// -- but it does answer the question that matters more: what is this action bound to on
// the hardware attached right now, in words this driver will recognise. Our own tables
// can only name a control in our words, and we do not know which interaction profile a
// given headset binds, so "the trigger" can simply be wrong.
const char* boundName(int action) {
    static SharedInputNames names;
    static int32_t          generation = 0;
    int32_t                 latest = 0;
    if (presenterLatestInputNames(names, latest)) generation = latest;
    if (!generation) return "";
    int hand = g_state.hand;
    if (hand < 0 || hand >= kHands) hand = kHandRight;
    if (action < 0 || action >= static_cast<int>(kInputActions)) return "";
    return names.text[hand][action];
}

// -- the footer's text --------------------------------------------------------------------

// What the trigger does on an action row, for the footer's control line.
const char* actionEffect(int command) {
    switch (command) {
        case kCommandClose: return "close the menu";
        case kCommandRecentre: return "recentre now";
        case kCommandMirrorTweak: return "open Mirror setup";
        case kCommandHudTweak: return "open Pribluda setup";
        case kCommandPanelTweak: return "open Pribluda position";
        case kCommandSaveSettings: return "write the settings to gplvr.ini";
        case kCommandResetSettings: return "choose what to reset";
        default: return "select it";
    }
}

struct HelpBlock {
    char lines[kHintLines][kHelpLineChars];
    int  count = 0;
    bool warning = false;  // a refusal rather than a hint, and drawn in the warning colour
};

// Greedy word wrap, in the slot's own metrics rather than a character count: the font is
// proportional and a character budget would either waste a third of the width or overflow it.
// A block that is full stops taking lines, so a long list is cut at the bottom and nothing is
// pushed off the canvas.
void wrapInto(HelpBlock& block, int slot, float width, const char* text) {
    if (!text || !text[0] || width <= 0.0f) return;

    char line[kHelpLineChars] = {};
    int  used = 0;
    const char* word = text;
    while (*word && block.count < kHintLines) {
        while (*word == ' ') ++word;
        if (!*word) break;
        const char* end = word;
        while (*end && *end != ' ') ++end;
        int length = static_cast<int>(end - word);
        if (length >= kHelpLineChars) length = kHelpLineChars - 1;

        char candidate[kHelpLineChars] = {};
        int  head = used;
        if (head) {
            std::memcpy(candidate, line, static_cast<size_t>(head));
            candidate[head++] = ' ';
        }
        if (head + length >= kHelpLineChars) length = kHelpLineChars - 1 - head;
        if (length > 0) std::memcpy(candidate + head, word, static_cast<size_t>(length));
        candidate[head + length] = '\0';

        if (used && static_cast<float>(vrTextWidth(slot, candidate)) > width) {
            _snprintf_s(block.lines[block.count], kHelpLineChars, _TRUNCATE, "%s", line);
            ++block.count;
            used = 0;
            line[0] = '\0';
            continue;  // the same word again, now onto an empty line
        }
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s", candidate);
        used = static_cast<int>(std::strlen(line));
        word = end;
    }
    if (used && block.count < kHintLines) {
        _snprintf_s(block.lines[block.count], kHelpLineChars, _TRUNCATE, "%s", line);
        ++block.count;
    }
}

// A binding's words, as the driver should read them. The "UNDECIDED" prefix is a note to
// whoever retunes the table and means nothing in a headset.
const char* bindingText(const char* description) {
    const char* undecided = "UNDECIDED -- ";
    size_t      length = std::strlen(undecided);
    if (std::strncmp(description, undecided, length) == 0) return description + length;
    return description;
}

// What the footer says: a refusal if one is outstanding, the hovered row's help in the menu,
// and in a tweak mode the mode's WHOLE control list -- built from the binding table itself, so
// the hints and the mapping cannot drift apart.
void buildHelpBlock(HelpBlock& block, int slot, float width) {
    block.count = 0;
    block.warning = false;

    if (g_notice[0] && menuNow() < g_noticeUntil) {
        block.warning = true;
        wrapInto(block, slot, width, g_notice);
        return;
    }

    if (g_state.mode == Mode::Menu) {
        if (g_state.hovered < 0 || g_state.hovered >= kRowCount) return;

        // WHAT THE ROW IS, then HOW TO WORK IT, as two lines rather than one sentence with
        // the scheme stapled on. The first is the row's own words and says nothing about
        // buttons; the second is the same answer for every row of that kind, in the names
        // this runtime gave the controls.
        wrapInto(block, slot, width, g_rows[g_state.hovered].help);

        const char* trigger = boundName(kInputTrigger);
        const char* stick = boundName(kInputStick);
        const char* grip = boundName(kInputSqueeze);
        if (!trigger[0]) trigger = "Trigger";
        if (!stick[0]) stick = "Stick";
        if (!grip[0]) grip = "Grip";

        // Every part is "control: effect", and the effect names what it acts on. The line sits
        // under the row's own help, so a bare "does it" read as belonging to that text.
        const MenuRow& item = g_rows[g_state.hovered];
        char           how[kHelpLineChars * 2] = {};
        switch (item.kind) {
            case kRowAction:
                _snprintf_s(how, sizeof(how), _TRUNCATE,
                            "%s on it, or %s click: %s   |   %s: move the highlight   |   %s: "
                            "close the menu",
                            trigger, stick, actionEffect(item.command), stick, grip);
                break;
            case kRowTab:
                _snprintf_s(how, sizeof(how), _TRUNCATE,
                            "%s on it, or %s click: open the %s tab   |   %s left/right: walk "
                            "the tabs   |   %s: close the menu",
                            trigger, stick, item.label, stick, grip);
                break;
            case kRowToggleBool:
            case kRowToggleInt:
                _snprintf_s(how, sizeof(how), _TRUNCATE,
                            "%s on it, or %s left/right: turn %s on or off   |   %s click: "
                            "back to default   |   %s: close the menu",
                            trigger, stick, item.label, stick, grip);
                break;
            case kRowChoiceInt:
            case kRowRateInt:
            case kRowNamedInt:
                _snprintf_s(how, sizeof(how), _TRUNCATE,
                            "%s left/right: step %s through its values   |   hold %s on the "
                            "bar and slide: set it directly   |   %s: close the menu",
                            stick, item.label, trigger, grip);
                break;
            case kRowSliderFloat:
                _snprintf_s(how, sizeof(how), _TRUNCATE,
                            "%s left/right: slide %s   |   hold %s on the bar and drag: set it "
                            "directly   |   %s: close the menu",
                            stick, item.label, trigger, grip);
                break;
            default: break;
        }
        if (how[0]) wrapInto(block, slot, width, how);
        return;
    }

    TweakTarget target = targetOf(g_state.mode);
    if (target == TweakTarget::None) return;
    int32_t             count = 0;
    const TweakBinding* bindings = tweakDefaultBindings(target, count);
    if (!bindings) return;

    // Consecutive rows often describe one gesture twice -- the pointer drag is a U row and a
    // V row with the same words -- and printing it twice would waste a line of a short strip.
    const char* previous = "";
    for (int32_t i = 0; i < count && block.count < kHintLines - 1; ++i) {
        const char* text = bindingText(bindings[i].description);
        if (std::strcmp(text, previous) == 0) continue;
        previous = text;
        wrapInto(block, slot, width, text);
    }

    // How to leave, and what leaving does NOT do. The grip is shared: a tap goes back, a hold
    // is whatever this mode's table above uses it for, so the two are named apart.
    const char* grip = boundName(kInputSqueeze);
    char        back[kHelpLineChars * 2] = {};
    _snprintf_s(back, sizeof(back), _TRUNCATE,
                "a TAP of %s goes back to the menu and saves NOTHING -- the menu's Save rows "
                "write the file",
                grip[0] ? grip : "the grip");
    wrapInto(block, slot, width, back);
}

// A digest of the footer, for the rebuild test. The block itself is up to nine lines and the
// signature has a thousand bytes for the whole canvas, so what goes in is a hash.
unsigned helpBlockHash(const HelpBlock& block) {
    unsigned hash = 2166136261u;
    for (int line = 0; line < block.count; ++line)
        for (const char* c = block.lines[line]; *c; ++c)
            hash = (hash ^ static_cast<unsigned char>(*c)) * 16777619u;
    return hash ^ (block.warning ? 0x9E3779B9u : 0u);
}

// The reserved height, which does not depend on what the block says.
float helpFooterHeight(int slot, int lines) {
    return static_cast<float>(vrTextHeight(slot)) * kHelpLineSpacing *
               static_cast<float>(lines) +
           static_cast<float>(vrTextHeight(slot)) * 0.5f;
}

void drawHelpBlock(const GlFunctions& gl, const HelpBlock& block, int slot, float x,
                   float top) {
    float step = static_cast<float>(vrTextHeight(slot)) * kHelpLineSpacing;
    if (block.warning)
        gl.color4f(1.0f, 0.78f, 0.30f, 1.0f);
    else
        gl.color4f(0.62f, 0.70f, 0.82f, 1.0f);
    for (int line = 0; line < block.count; ++line)
        canvasText(gl, slot, x, top + step * (static_cast<float>(line) + 0.5f),
                   block.lines[line]);
}

// The cursor, in the canvas because the canvas IS the panel now: there is no other
// surface to draw it on once the menu is a composition layer. A crosshair with a dark
// surround rather than a dot -- a few pixels of solid colour is invisible against a
// bright track, and the first build's small square was reported as no pointer at all.
void drawCanvasCursor(const GlFunctions& gl, float w, float h) {
    if (!g_state.pointerOn) return;
    const float x = g_state.pointerU * w;
    const float y = g_state.pointerV * h;
    // Square in canvas pixels IS square in metres here: every mode's canvas is built with
    // its quad's own aspect, so the two map 1:1. canvasSizeFor is what guarantees it.
    const float arm = w * 0.020f;
    const float thick = w * 0.0035f;

    gl.color4f(0.0f, 0.0f, 0.0f, 0.8f);
    canvasQuad(gl, x - arm, y - thick * 2.0f, x + arm, y + thick * 2.0f);
    canvasQuad(gl, x - thick * 2.0f, y - arm, x + thick * 2.0f, y + arm);
    gl.color4f(1.0f, 0.88f, 0.20f, 1.0f);
    canvasQuad(gl, x - arm, y - thick, x + arm, y + thick);
    canvasQuad(gl, x - thick, y - arm, x + thick, y + arm);
}

void drawCanvasContent(const GlFunctions& gl, int width, int height) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    // HUD TWEAK: the quad sits ON the HUD panel, so the background stays transparent --
    // the panel underneath is the thing being arranged and must stay visible. What goes
    // on top is the element boxes, the cursor, and one readout line in a box of its own.
    if (g_state.mode == Mode::HudTweak) {
        const int selected = pribludaElementSelected();
        for (int element = 0; element < pribludaElementCount(); ++element) {
            float x = 0.0f;
            float y = 0.0f;
            float bw = 0.0f;
            float bh = 0.0f;
            if (!pribludaElementUsable(element)) continue;
            if (!pribludaElementBox(element, x, y, bw, bh)) continue;
            if (element == selected)
                gl.color4f(1.0f, 0.85f, 0.25f, 1.0f);
            else if (element == g_state.hoveredElement)
                gl.color4f(0.55f, 0.85f, 1.0f, 1.0f);
            else
                gl.color4f(0.55f, 0.60f, 0.70f, 0.55f);
            // The boxes are already fractions of the panel and the canvas is the panel,
            // so this is a multiply and no mapping at all.
            canvasOutline(gl, x * w, y * h, (x + bw) * w, (y + bh) * h);

            // A HIDDEN ELEMENT IS CROSSED OUT, not left out. It is still outlined, still
            // selectable and still movable -- what the cross says is that it will not be
            // drawn in the race until it is switched back on. Leaving it out instead is
            // what made hiding one a one-way door, and a separate list of hidden things
            // would be a second widget to build and learn.
            if (!pribludaElementShown(element)) {
                gl.color4f(1.0f, 0.25f, 0.25f, 0.9f);
                gl.begin(GL_LINES);
                gl.vertex2f(x * w, y * h);
                gl.vertex2f((x + bw) * w, (y + bh) * h);
                gl.vertex2f((x + bw) * w, y * h);
                gl.vertex2f(x * w, (y + bh) * h);
                gl.end();
            }
        }

        char summary[192] = {};
        tweakSummary(g_state.mode, summary, sizeof(summary));

        HelpBlock block;
        buildHelpBlock(block, kVrTextSmall, w - kHelpMargin * 2.0f);
        float readout = static_cast<float>(vrTextHeight(kVrTextBody)) * 1.6f;
        float hints = helpFooterHeight(kVrTextSmall, block.count);
        // The band is the panel's own bottom edge and the panel is what is being arranged,
        // so the list gives way rather than covering the thing it is about.
        if (readout + hints > h * 0.55f) hints = h * 0.55f - readout;
        if (hints < 0.0f) hints = 0.0f;
        float bandHeight = readout + hints;

        gl.color4f(0.05f, 0.06f, 0.09f, 0.85f);
        canvasQuad(gl, 0.0f, h - bandHeight, w, h);
        gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
        canvasText(gl, kVrTextBody, kHelpMargin, h - bandHeight + readout * 0.5f, summary);
        drawHelpBlock(gl, block, kVrTextSmall, kHelpMargin, h - hints);
        drawCanvasCursor(gl, w, h);
        return;
    }

    gl.color4f(0.05f, 0.06f, 0.09f, 0.88f);
    canvasQuad(gl, 0.0f, 0.0f, w, h);
    gl.color4f(0.55f, 0.62f, 0.75f, 0.95f);
    canvasOutline(gl, 1.0f, 1.0f, w - 1.0f, h - 1.0f);

    // "SAVE CHANGES?" -- three answers, laid out on the same row grid the menu uses so the
    // pointer test is the one that already exists.
    if (g_state.mode == Mode::Confirm) {
        float band = h / static_cast<float>(kConfirmCount + 1);
        char  question[160] = {};
        _snprintf_s(question, sizeof(question), _TRUNCATE, "Save the %s?  %d value%s changed",
                    modeName(g_state.confirmFrom), g_state.confirmChanged,
                    g_state.confirmChanged == 1 ? "" : "s");
        gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
        canvasText(gl, kVrTextBody, kHelpMargin, band * 0.5f, question);

        for (int choice = 0; choice < kConfirmCount; ++choice) {
            float top = band * static_cast<float>(choice + 1);
            if (choice == g_state.confirmChoice) {
                gl.color4f(0.22f, 0.45f, 0.78f, 0.95f);
                canvasQuad(gl, w * 0.02f, top + band * 0.06f, w * 0.98f, top + band * 0.94f);
                gl.color4f(1.0f, 0.85f, 0.25f, 1.0f);
                canvasOutline(gl, w * 0.02f, top + band * 0.06f, w * 0.98f,
                              top + band * 0.94f);
            }
            gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
            canvasText(gl, kVrTextBody, w * 0.05f, top + band * 0.5f,
                       kConfirmLabel[choice]);
        }
        drawCanvasCursor(gl, w, h);
        return;
    }

    // "RESET WHAT?" -- the same three-part shape as the save dialog, with five answers. The
    // destructive ones say what file they clear, because "reset" on its own does not tell
    // anyone whether their per-car mirror work is about to go.
    if (g_state.mode == Mode::Reset) {
        float band = h / static_cast<float>(kResetCount + 1);
        gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
        canvasText(gl, kVrTextBody, kHelpMargin, band * 0.5f, "Reset what?");

        for (int choice = 0; choice < kResetCount; ++choice) {
            float top = band * static_cast<float>(choice + 1);
            if (choice == g_state.resetChoice) {
                gl.color4f(0.22f, 0.45f, 0.78f, 0.95f);
                canvasQuad(gl, w * 0.02f, top + band * 0.06f, w * 0.98f, top + band * 0.94f);
                gl.color4f(1.0f, 0.85f, 0.25f, 1.0f);
                canvasOutline(gl, w * 0.02f, top + band * 0.06f, w * 0.98f,
                              top + band * 0.94f);
            }
            // Reset ALL is red, the colour the page's Reset button is drawn in.
            if (choice == kResetEverything)
                setResetColour(gl);
            else
                gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
            canvasText(gl, kVrTextBody, w * 0.05f, top + band * 0.5f, kResetLabel[choice]);
        }
        drawCanvasCursor(gl, w, h);
        return;
    }

    // A TWEAK MODE'S STRIP: the readout across the top, the mode's whole control list under
    // it. The list is why the strip is as tall as it is -- the menu's help line is not
    // reachable from in here, and leaving the mode to find out what the grip does is the trip
    // this saves.
    if (g_state.mode != Mode::Menu) {
        char summary[192] = {};
        tweakSummary(g_state.mode, summary, sizeof(summary));
        float readout = static_cast<float>(vrTextHeight(kVrTextBody)) * 1.6f;
        gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
        canvasText(gl, kVrTextBody, kHelpMargin, readout * 0.5f, summary);

        HelpBlock block;
        buildHelpBlock(block, kVrTextSmall, w - kHelpMargin * 2.0f);
        gl.color4f(0.30f, 0.34f, 0.42f, 0.9f);
        gl.begin(GL_LINES);
        gl.vertex2f(kHelpMargin, readout);
        gl.vertex2f(w - kHelpMargin, readout);
        gl.end();
        drawHelpBlock(gl, block, kVrTextSmall, kHelpMargin, readout + 6.0f);
        drawCanvasCursor(gl, w, h);
        return;
    }

    // The rows share what the footer leaves, so a hint that wraps onto three lines moves
    // nothing: the reservation is the same whether it holds one line or none.
    HelpBlock block;
    buildHelpBlock(block, kVrTextSmall, w - kHelpMargin * 2.0f);
    float footer = helpFooterHeight(kVrTextSmall, kHelpLines);
    int   lines = lineCount();
    float band = (h - footer) / static_cast<float>(lines);

    gl.color4f(0.88f, 0.91f, 0.97f, 1.0f);
    canvasText(gl, kVrTextBody, 20.0f, band * 0.5f, "GPLVR");

    // THE BUTTONS AND THE STRIP: boxes side by side, each with its label centred. The open tab
    // is filled and underlined so it reads as the page below it; the highlight is the same
    // bar-and-outline every row gets, on top of either.
    for (int line = kButtonLine; line <= kTabLine; ++line) {
        int   count = lineWidth(line);
        float left = 0.0f;
        float right = 1.0f;
        lineSpan(line, left, right);
        float cell = count > 0 ? (right - left) / static_cast<float>(count) : 0.0f;
        float top = band * static_cast<float>(line) + band * 0.10f;
        float bottom = band * static_cast<float>(line + 1) - band * 0.10f;
        for (int column = 0; column < count; ++column) {
            int   row = lineRow(line, column);
            float x0 = w * (left + cell * static_cast<float>(column)) + 4.0f;
            float x1 = w * (left + cell * static_cast<float>(column + 1)) - 4.0f;
            bool  open = g_rows[row].kind == kRowTab && g_rows[row].tab == g_openTab;

            if (row == g_state.hovered)
                gl.color4f(0.22f, 0.45f, 0.78f, 0.95f);
            else if (open)
                gl.color4f(0.30f, 0.34f, 0.42f, 0.95f);
            else
                gl.color4f(0.17f, 0.19f, 0.23f, 0.95f);
            canvasQuad(gl, x0, top, x1, bottom);
            if (open) {
                gl.color4f(0.45f, 0.70f, 0.95f, 1.0f);
                canvasQuad(gl, x0, bottom - 5.0f, x1, bottom);
            }
            if (row == g_state.hovered) {
                gl.color4f(1.0f, 0.85f, 0.25f, 1.0f);
                canvasOutline(gl, x0, top, x1, bottom);
            }

            float labelWidth = static_cast<float>(vrTextWidth(kVrTextBody, g_rows[row].label));
            if (g_rows[row].command == kCommandResetSettings)
                setResetColour(gl);
            else if (g_rows[row].command == kCommandSaveSettings)
                gl.color4f(0.45f, 0.95f, 0.50f, 1.0f);
            else
                gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
            canvasText(gl, kVrTextBody, (x0 + x1 - labelWidth) * 0.5f,
                       (top + bottom) * 0.5f, g_rows[row].label);
        }
    }
    gl.color4f(0.30f, 0.34f, 0.42f, 0.9f);
    gl.begin(GL_LINES);
    gl.vertex2f(kHelpMargin, band * static_cast<float>(kFirstRowLine));
    gl.vertex2f(w - kHelpMargin, band * static_cast<float>(kFirstRowLine));
    gl.end();

    gl.color4f(0.30f, 0.34f, 0.42f, 0.9f);
    gl.begin(GL_LINES);
    gl.vertex2f(kHelpMargin, h - footer);
    gl.vertex2f(w - kHelpMargin, h - footer);
    gl.end();
    drawHelpBlock(gl, block, kVrTextSmall, kHelpMargin, h - footer + 6.0f);

    const float labelX = w * 0.05f;
    // The longest label is "Pribluda update rate", which ran into the value column at 0.40.
    // The value column sits just left of the slider track rather than being measured against
    // any one label, so a longer label added later still has somewhere to stop.
    const float valueX = w * 0.48f;
    const float trackLeft = w * 0.58f;
    const float trackRight = w * 0.94f;

    char value[64] = {};
    for (int line = kFirstRowLine; line < lines; ++line) {
        const int row = lineRow(line, 0);
        if (row < 0) break;
        const float top = band * static_cast<float>(line);
        const float centre = top + band * 0.5f;

        if (row == g_state.hovered) {
            // A bar, an outline and a caret. Three marks rather than one because the
            // first build's single translucent bar was reported as no highlighted row at
            // all -- against a bright sky it is not a difference anyone can see.
            gl.color4f(0.22f, 0.45f, 0.78f, 0.95f);
            canvasQuad(gl, w * 0.02f, top + band * 0.06f, w * 0.98f, top + band * 0.94f);
            gl.color4f(1.0f, 0.85f, 0.25f, 1.0f);
            canvasOutline(gl, w * 0.02f, top + band * 0.06f, w * 0.98f,
                          top + band * 0.94f);
            canvasQuad(gl, w * 0.025f, top + band * 0.22f, w * 0.042f,
                       top + band * 0.78f);
        }

        const MenuRow& item = g_rows[row];
        const float    fraction = rowFraction(item);
        if (fraction >= 0.0f) {
            // The track and its filled part. dragRow reads the same two fractions of the
            // panel, so what is drawn and what is grabbed are the same rectangle.
            gl.color4f(0.25f, 0.27f, 0.32f, 0.95f);
            canvasQuad(gl, trackLeft, centre - band * 0.16f, trackRight,
                       centre + band * 0.16f);
            gl.color4f(0.45f, 0.70f, 0.95f, 1.0f);
            canvasQuad(gl, trackLeft, centre - band * 0.16f,
                       trackLeft + (trackRight - trackLeft) * fraction,
                       centre + band * 0.16f);
        }

        gl.color4f(0.94f, 0.96f, 1.0f, 1.0f);
        canvasText(gl, kVrTextBody, labelX, centre, item.label);
        rowValueText(item, value, sizeof(value));
        if (value[0]) canvasText(gl, kVrTextBody, valueX, centre, value);
    }

    drawCanvasCursor(gl, w, h);
}

// WHAT THE HUD TWEAK CANVAS DRAWS, as one number.
//
// That canvas is not the mode and the selection. It is every element's outline, in the box
// pribluda last measured it at, with a red cross over each one that is switched off -- and
// all of that changes while the mode stays open and the selection stays where it is.
// Hiding the SELECTED element is exactly that case, and it is why the cross used to appear
// only once something else had forced a rebuild: the signature said nothing had changed,
// so nothing was redrawn.
unsigned hudTweakHash(int width, int height) {
    unsigned hash = 2166136261u;
    auto     fold = [&hash](unsigned value) { hash = (hash ^ value) * 16777619u; };

    fold(static_cast<unsigned>(g_state.hoveredElement + 1));
    fold(static_cast<unsigned>(pribludaElementSelected() + 1));
    for (int element = 0; element < pribludaElementCount(); ++element) {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        bool  usable =
            pribludaElementUsable(element) && pribludaElementBox(element, x, y, w, h);
        fold(usable ? 1u : 0u);
        if (!usable) continue;
        fold(pribludaElementShown(element) ? 1u : 0u);
        // The box in the canvas pixels it is outlined at, which is all of it the picture
        // can tell apart, so a drag that moves an element by less than a pixel does not
        // rebuild the canvas.
        fold(static_cast<unsigned>(static_cast<int>(x * static_cast<float>(width))));
        fold(static_cast<unsigned>(static_cast<int>(y * static_cast<float>(height))));
        fold(static_cast<unsigned>(static_cast<int>(w * static_cast<float>(width))));
        fold(static_cast<unsigned>(static_cast<int>(h * static_cast<float>(height))));
    }
    return hash;
}

// What the canvas would say, as one string. Cheap to build, cheap to compare, and exact:
// anything that changes the picture changes this, and nothing that does not, does.
void buildCanvasSignature(char* out, size_t size, int width, int height) {
    // The cursor is drawn on every mode's canvas, so where it is drawn belongs here.
    // Quantised to the pixel it lands on, which is what stops a hand's last fraction of a
    // pixel rebuilding four hundred glyphs.
    int cursorX = g_state.pointerOn
                      ? static_cast<int>(g_state.pointerU * static_cast<float>(width))
                      : -1;
    int cursorY = g_state.pointerOn
                      ? static_cast<int>(g_state.pointerV * static_cast<float>(height))
                      : -1;
    int written = _snprintf_s(out, size, _TRUNCATE, "%d|%d|%dx%d|%d,%d|",
                              static_cast<int>(g_state.mode), g_state.hovered, width, height,
                              cursorX, cursorY);
    if (written < 0) return;

    // The dialog says nothing the rest of this function knows about, and its whole picture is
    // the question plus which of three answers is highlighted.
    if (g_state.mode == Mode::Confirm) {
        _snprintf_s(out + written, size - static_cast<size_t>(written), _TRUNCATE, "%d|%d|%d",
                    static_cast<int>(g_state.confirmFrom), g_state.confirmChanged,
                    g_state.confirmChoice);
        return;
    }
    if (g_state.mode == Mode::Reset) {
        _snprintf_s(out + written, size - static_cast<size_t>(written), _TRUNCATE, "%d",
                    g_state.resetChoice);
        return;
    }

    // The footer goes in as a hash. It is up to nine wrapped lines and the whole signature
    // has a thousand bytes; what matters is that it changes when the text does, which a hash
    // does exactly. The width it wraps to is the drawn one, so a rewrap counts as a change.
    HelpBlock block;
    buildHelpBlock(block, kVrTextSmall, static_cast<float>(width) - kHelpMargin * 2.0f);
    unsigned footer = helpBlockHash(block);

    if (g_state.mode != Mode::Menu) {
        char summary[192] = {};
        tweakSummary(g_state.mode, summary, sizeof(summary));
        // HUD tweak has a picture on the panel as well as a readout, and the outlines and
        // the crosses in it move and change with nothing else on the canvas moving.
        if (g_state.mode == Mode::HudTweak) {
            _snprintf_s(out + written, size - static_cast<size_t>(written), _TRUNCATE,
                        "%s|%08X|%08X", summary, footer, hudTweakHash(width, height));
            return;
        }
        _snprintf_s(out + written, size - static_cast<size_t>(written), _TRUNCATE, "%s|%08X",
                    summary, footer);
        return;
    }

    int used = written;
    // The open tab and lineCount() go in because they decide which row is drawn where: a tab
    // switch or a row that stops being offered moves rows without changing any value, and the
    // canvas has to be rebuilt for that alone.
    used += _snprintf_s(out + used, size - static_cast<size_t>(used), _TRUNCATE, "%08X|%d|%d|",
                        footer, g_openTab, lineCount());

    char value[64] = {};
    for (int row = 0; row < kRowCount && used > 0 && static_cast<size_t>(used) + 40 < size;
         ++row) {
        rowValueText(g_rows[row], value, sizeof(value));
        // The fill is quantised, so a slider dragged by less than a five-hundredth of its
        // track does not rebuild the canvas for a change nobody can see.
        const float fraction = rowFraction(g_rows[row]);
        const int   quantised = fraction < 0.0f ? -1 : static_cast<int>(fraction * 512.0f);
        const int   added = _snprintf_s(out + used, size - static_cast<size_t>(used),
                                        _TRUNCATE, "%s,%d;", value, quantised);
        if (added < 0) break;
        used += added;
    }
}

}  // namespace

void vrMenuPoll() {
    State& state = g_state;

    if (!g_options.vr || !vrInputAvailable()) {
        if (state.mode != Mode::Closed) closeMenu(" -- there is no controller input");
        frontEndPointerRelease("the controllers went away");
        return;
    }

    // THE FRONT END IS SOMEWHERE THE MENU OPENS TOO, and it used to be the one place it did
    // not. Everything a driver would reach for outside a race -- the render scale, the
    // gamma, how much of the eye the front end itself covers -- was behind a mode that only
    // existed in the cockpit, so setting the headset up meant starting a session first.
    //
    // The controller does one of two jobs here and never both at once: menu shut, it drives
    // the front end's own pointer; menu open, it drives the menu. The handover runs
    // frontEndPointerRelease, which is what keeps a trigger held over a front-end button
    // from leaving that button down once the pointer stops being polled.
    bool frontEnd = windowInFrontEnd();
    if (frontEnd != state.wasFrontEnd) {
        state.wasFrontEnd = frontEnd;

        // ANYTHING OPEN IS REOPENED AT THE MENU, both ways across. A tweak mode's target does
        // not survive the crossing -- the glasses and the pribluda panel belong to a car --
        // and reopening is also the one call that clears the snapshot, recaptures the panel
        // against the seat and forgets the pointer filter, so nothing measured on one side of
        // the transition is left describing the other.
        //
        // Values a mode changed stay live. Only a Save writes anything, which is exactly
        // where the driver would have been had they stepped back by hand; the count is logged
        // so the crossing is not a silent loss.
        if (state.mode != Mode::Closed) {
            if (state.mode != Mode::Menu) {
                int changed = snapshotChanged();
                logf("vr menu: %s was open crossing into the %s, with %d changed value%s -- "
                     "reopened at the menu. Nothing was written; the values are still live.",
                     modeName(state.mode), frontEnd ? "front end" : "race", changed,
                     changed == 1 ? "" : "s");
            }
            if (state.hand >= 0 && state.hand < kHands)
                enterMode(Mode::Menu, state.hand);
            else
                closeMenu(" -- the hand that opened it went away across the transition");
        }
        frontEndPointerRelease(frontEnd ? "the game left the race" : "the race started");
    }

    if (frontEnd && state.mode == Mode::Closed) {
        // The grip tap is asked about BEFORE the pointer is driven, so the poll that opens
        // the menu does not also push the front end's cursor and send it a click on the
        // way past.
        const int hand = openingHand();
        if (hand >= 0) {
            frontEndPointerRelease("the VR menu opened");
            enterMode(Mode::Menu, hand);
            return;
        }
        pollFrontEndPointer();
        // Setting the seat from out here is worth having: it is where a driver puts the
        // headset on, and the trigger is the front end's click, so the face buttons and
        // the stick click are free.
        pollQuickRecentre(true);
        return;
    }

    // A WATCHED REPLAY GETS THE POINTER TOO, without taking the world-mode poll away from the
    // rest of this function. The replay screen's controls are clicked with the same mechanism
    // the front end's are, and the block above is reached only in the front end -- which is why
    // the controller was dead over the replay band however well the band was placed.
    // Leaving the replay -- "live", or the session ending -- with the trigger held lets the button
    // go; the poll that would have sent the release does not run outside a replay.
    //
    // While the replay radial is open the pointer stays where it was: the hand that holds the
    // trigger is choosing a slice, and its aim is not meant for GPL's widgets. The radial opens
    // only on a press whose ray is off the panel, which this poll never turns into a click, and
    // refuses while a click is held -- so the button is not down here; if it ever were, it is
    // let go rather than left held behind the radial.
    if (!frontEnd && state.mode == Mode::Closed) {
        if (replayQuickMenuOpen()) {
            if (state.frontEndButtonDown) frontEndPointerRelease("the replay radial opened");
        } else if (gplReplayScreenShown()) {
            pollFrontEndPointer();
        } else {
            frontEndPointerRelease("the replay ended");
        }
    }

    const float seconds = vrInputSeconds();

    if (state.mode == Mode::Closed) {
        const int hand = openingHand();
        if (hand >= 0) {
            // The grip still opens the menu over an open quick menu, which closes unfired and
            // hands the controller over first.
            quickMenuCancel("the VR menu opened");
            replayQuickMenuCancel("the VR menu opened");
            enterMode(Mode::Menu, hand);
            return;
        }
        // While a quick menu is open the stick is its own, and so is the rest of the hand. On the
        // replay screen the stick click is play/pause; the face buttons still recentre.
        if (!quickMenuOpen() && !replayQuickMenuOpen())
            pollQuickRecentre(!replayControlsHaveStick());
        return;
    }

    if (state.hand < 0 || state.hand >= kHands) {
        closeMenu(" -- the owning hand went away");
        return;
    }
    const VrHandInput& hand = vrInputHand(state.hand);
    if (!hand.present) {
        closeMenu(" -- the owning hand stopped being tracked");
        return;
    }

    // A grip TAP steps back one level, whichever mode is open -- the same control that opened
    // the menu, so there is one button to learn and it always means "back".
    bool back = gripTapped(hand, state.gripSince[state.hand], state.gripConsumed[state.hand]);
    if (back) {
        if (state.mode == Mode::Menu) {
            closeMenu(" -- the grip");
        } else if (state.mode == Mode::Confirm) {
            // A tap on the dialog is Cancel: the safe answer to a question nobody meant to
            // ask is the one that changes nothing and goes back to where they were.
            enterMode(state.confirmFrom, state.hand);
        } else if (state.mode == Mode::Reset) {
            enterMode(Mode::Menu, state.hand);
        } else {
            // LEAVING A TWEAK MODE DOES NOT WRITE ANYTHING BY ITSELF. It used to commit, which
            // made the menu's two Save rows meaningless and left no way out of a mode that did
            // not keep what had just been done.
            //
            // So: nothing changed, leave silently. Something changed, ask -- because the
            // adjustment stays live either way and "did that get saved?" is not a question a
            // driver should have to answer by quitting and looking.
            int changed = snapshotChanged();
            if (!changed) {
                enterMode(Mode::Menu, state.hand);
            } else {
                Mode from = state.mode;
                state.confirmChanged = changed;
                enterMode(Mode::Confirm, state.hand);
                state.confirmFrom = from;
                logf("vr menu: leaving %s with %d changed value%s -- asking", modeName(from),
                     changed, changed == 1 ? "" : "s");
            }
        }
        return;
    }

    // Where the ray meets the surface. A tweak mode still needs it: the HUD drag is an
    // absolute pointer command, and the strip's readout wants to know whether the driver
    // is looking at it.
    float         u = 0.0f;
    float         v = 0.0f;
    const Surface surface = pointerSurface();
    // The ray has to be in the panel's frame, which is the seat's. Both the hand and the
    // panel come from the presenter's LOCAL space and the conversion is the same one.
    SharedPose aimSeat = {};
    state.pointerOn =
        poseInSeat(hand.aimPose, aimSeat) && rayToSurface(surface, aimSeat, u, v);
    if (state.pointerOn) {
        // THE SAME FILTER THE FRONT END'S POINTER USES. /input/aim/pose carries the hand's
        // real tremor at the display rate, and the throw from the hand to the panel turns
        // a couple of millimetres of it into a visible wobble on a slider. Smoothed here,
        // at the one place the hit becomes a position, so the row hit test, the element
        // hit test, the drag and the ray's own far end all follow the same steady point.
        smoothPointer(u, v, state.pointerU, state.pointerV);
        ++state.rayHits;
    } else {
        // Off the surface the filter has nothing to carry forward: the next hit is a new
        // place on the panel and sliding to it from where the ray left would draw a
        // pointer travelling across rows nobody aimed at.
        state.smoothValid = false;
        state.pointerU = 0.0f;
        state.pointerV = 0.0f;
        ++state.rayMisses;
    }

    // Which element the ray is inside, for the Select binding and for the outline the draw
    // puts round it. First match wins, which is the top-most in the element table; the
    // boxes overlap only when the driver has stacked them.
    // AFTER the ray, because the dialog can be pointed at, and BEFORE the tweak bindings,
    // because while a question about the mode is up the mode's own controls must not run --
    // the stick answering it must not also be moving the thing being asked about.
    if (state.mode == Mode::Confirm) {
        pollConfirm(hand);
        return;
    }
    if (state.mode == Mode::Reset) {
        pollReset(hand);
        return;
    }

    state.hoveredElement = -1;
    if (state.mode == Mode::HudTweak && state.pointerOn) {
        for (int element = 0; element < pribludaElementCount(); ++element) {
            float x = 0.0f;
            float y = 0.0f;
            float w = 0.0f;
            float h = 0.0f;
            if (!pribludaElementUsable(element)) continue;
            if (!pribludaElementBox(element, x, y, w, h)) continue;
            if (state.pointerU >= x && state.pointerU <= x + w && state.pointerV >= y &&
                state.pointerV <= y + h) {
                state.hoveredElement = element;
                break;
            }
        }
    }

    if (state.mode == Mode::HudTweak) {
        // The grab, and the only place the drag's frame of reference is decided. The grip
        // closing over the panel captures where the pointer sits inside the selected
        // element; opening it lets go.
        if (hand.squeezeHeld.pressed && state.pointerOn) {
            float x = 0.0f;
            float y = 0.0f;
            if (pribludaElementPosition(pribludaElementSelected(), x, y)) {
                state.grabbed = true;
                state.grabDragging = false;
                state.grabOffsetU = state.pointerU - x;
                state.grabOffsetV = state.pointerV - y;
                state.grabStartU = state.pointerU;
                state.grabStartV = state.pointerV;
            }
        }
        // A GRIP THAT HAS NOT MOVED THE POINTER IS NOT A DRAG, and this is what tells the
        // two apart. The grip is a modifier AND the "back" tap, so the moment the drag
        // starts writing positions the tap is gone -- and it used to start on the poll
        // after the press, because a hand holding a laser pointer still drifts a few
        // tenths of a millimetre and the drag wrote every one of them. A tap over the
        // panel could therefore never go back. Nothing is written until the pointer has
        // travelled kGrabSlop from where the grip closed.
        if (state.grabbed && !state.grabDragging && state.pointerOn) {
            float du = state.pointerU - state.grabStartU;
            float dv = state.pointerV - state.grabStartV;
            if (du * du + dv * dv >= kGrabSlop * kGrabSlop) state.grabDragging = true;
        }
        if (!hand.squeezeHeld.down) {
            state.grabbed = false;
            state.grabDragging = false;
        }
    } else {
        state.grabbed = false;
        state.grabDragging = false;
    }

    if (state.mode != Mode::Menu) {
        applyBindings(state.mode, hand, seconds);
        return;
    }

    // THE HIGHLIGHT. Two ways in, and the highlight never goes away: whichever was used
    // last owns it, so a driver who can only point and a driver who never points both
    // have a row selected at all times and the trigger always has something to act on.
    //
    // A WIDGET BEING EDITED KEEPS THE FOCUS. Dragging along a slider is a horizontal gesture
    // made with an arm on the end of a shoulder, so the hit point drifts vertically while it
    // runs; without this the drag crosses into the next row and starts writing THAT setting.
    // The lock lasts exactly as long as the drag, which is as long as the trigger is held.
    //
    // A press on an ACTION row is not a lock: nothing is being edited until it fires, and being
    // able to pull the trigger, notice it is the wrong row and drag off it is worth keeping.
    //
    // THE STICK IS THE SECOND WAY, and it needed the same protection for the same reason.
    // Holding a controller still while pushing its stick sideways is not something a hand
    // does: the ray drifts, the highlight follows it onto the next row, and the push that
    // was adjusting one setting carries on adjusting another. While the stick is over, the
    // row it started on keeps the highlight, until the stick comes back to neutral.
    bool adjusting = stickIsAdjusting(hand);
    bool dragging = state.draggingSlider && state.pressed >= 0 && state.pressed < kRowCount;
    bool editing = dragging || adjusting;
    if (dragging) {
        state.hovered = state.pressed;
    } else if (!editing && state.pointerOn) {
        const int row = rowAt(state.pointerU, state.pointerV);
        if (row >= 0 && (!state.stickOwnsHighlight || row != state.hovered)) {
            state.hovered = row;
            state.stickOwnsHighlight = false;
        }
    }
    if (!editing) navigateWithStick(hand);
    // A row that stops being offered while the page is open takes the highlight with it, so
    // the clamp asks whether the highlighted row is still on the page rather than only
    // whether it is in range.
    if (rowLine(state.hovered) < 0) state.hovered = firstRow();
    if (rowLine(state.hovered) == kButtonLine) state.buttonColumn = rowColumn(state.hovered);

    // A STICK CLICK PUTS THE HIGHLIGHTED SETTING BACK, which is what it already means inside
    // every tweak mode. Same control, same meaning, wherever the driver happens to be. On a
    // button or a tab, which have nothing to put back, it presses it: the stick alone selects
    // and activates, and the trigger only ever acts on what the ray is on.
    if (hand.stick.pressed) {
        int kind = state.hovered >= 0 ? g_rows[state.hovered].kind : -1;
        if (kind == kRowAction || kind == kRowTab) {
            // It may have left the page -- closed the menu, opened a tweak mode or the reset
            // dialog -- so nothing below runs against it this poll.
            activateRow(state.hovered);
            return;
        }
        resetRow(state.hovered);
    }

    // The stick only adjusts while the trigger is not doing something of its own, so a
    // drag and a stick sweep cannot both write the same value in one poll.
    if (!state.draggingSlider) adjustWithStick(hand, seconds);

    // Press, drag, release -- and a release only counts on the row it was pressed on,
    // which is what lets a mis-aimed press be dragged off and abandoned.
    //
    // THE TRIGGER ACTS ONLY ON THE ROW THE RAY IS ON, at the press and again at the release.
    // The highlight stays on the last row the ray crossed, so pointing at the floor, or at an
    // empty band under a short tab, still leaves a row highlighted -- and a press there used
    // to drag that slider to wherever the ray happened to be, or fire that button. Without
    // pointing, the stick does both jobs.
    const bool triggerDown = hand.triggerHeld.down;
    int        pointed = state.pointerOn ? rowAt(state.pointerU, state.pointerV) : -1;
    if (hand.triggerHeld.pressed) {
        state.pressed = pointed >= 0 && pointed == state.hovered ? pointed : -1;
        state.draggingSlider = state.pressed >= 0 &&
                               (g_rows[state.pressed].kind == kRowSliderFloat ||
                                g_rows[state.pressed].kind == kRowChoiceInt ||
                                g_rows[state.pressed].kind == kRowRateInt ||
                                g_rows[state.pressed].kind == kRowNamedInt);
        if (state.draggingSlider) dragRow(state.pressed, state.pointerU);
    } else if (triggerDown && state.draggingSlider && state.pointerOn) {
        dragRow(state.pressed, state.pointerU);
    } else if (hand.triggerHeld.released) {
        // Released with the ray still on the row it was pressed on.
        if (!state.draggingSlider && state.pressed >= 0 && state.pressed == pointed)
            activateRow(state.pressed);
        state.pressed = -1;
        state.draggingSlider = false;
    }
}


void vrMenuUpdate(const GlFunctions& gl) {
    State& state = g_state;

    if (state.mode == Mode::Closed) {
        if (state.layerVisible) {
            state.layerVisible = false;
            state.canvasSignatureValid = false;
            presenterMenuHide();
        }
        // The layer is free while the menu is closed: a quick menu has it while the trigger
        // holds it open -- the car's or the replay's, never both -- and otherwise, in a replay or
        // under a GPL dialog, GPL's own 2D does. The replay panel yields it to the replay radial
        // and publishes again, from scratch, the frame the radial lets it go.
        bool quick = quickMenuUpdate(gl);
        bool replayRadial = replayQuickMenuUpdate(gl);
        replayPanelUpdate(gl, !quick && !replayRadial);
        return;
    }
    replayPanelUpdate(gl, false);

    if (!gl.begin || !gl.end || !gl.vertex2f || !gl.color4f || !gl.loadIdentity ||
        !gl.ortho || !gl.matrixMode || !gl.pushMatrix || !gl.popMatrix || !gl.pushAttrib ||
        !gl.popAttrib || !gl.getIntegerv || !gl.disable || !gl.enable || !gl.blendFunc ||
        !gl.colorMask || !gl.polygonMode) {
        noteDrawRefusal(kDrawNoGl);
        return;
    }

    // The quad's pose: the surface this mode draws, in the seat's space. drawnSurface's
    // orientation is the quad's own -- +X right, +Y up, +Z out of the visible face, which
    // is the convention a quad layer is placed with.
    const Surface drawn = drawnSurface();
    if (!drawn.valid || drawn.width <= 0.0f || drawn.height <= 0.0f) {
        noteDrawRefusal(state.mode == Mode::HudTweak ? kDrawNoHudPanel : kDrawNoSeat);
        return;
    }

    int canvasWidth = 0;
    int canvasHeight = 0;
    canvasSizeFor(canvasWidth, canvasHeight);
    // The dialog is drawn on the menu's canvas at the menu's size, so it takes the menu's font.
    const bool menuMode = state.mode == Mode::Menu || state.mode == Mode::Confirm ||
                          state.mode == Mode::Reset;
    if (!vrTextEnsure(gl, kVrTextBody, menuMode ? kMenuBodyPixels : kStripBodyPixels) ||
        !vrTextEnsure(gl, kVrTextSmall, kMenuSmallPixels)) {
        noteDrawRefusal(kDrawNoText);
        return;
    }

    // Rebuilt only when what it says changes, and never faster than kMenuRebuildHz. The
    // cursor is part of the picture, so a moving hand does rebuild it -- about four
    // hundred glyphs, which is why the rate is capped rather than left to the frame rate.
    char signature[sizeof(state.canvasSignature)] = {};
    buildCanvasSignature(signature, sizeof(signature), canvasWidth, canvasHeight);
    const LONGLONG now = qpcNow();
    const bool     changed = !state.canvasSignatureValid ||
                         std::strcmp(signature, state.canvasSignature) != 0;
    const bool due = !state.lastBuildQpc || !state.frequency ||
                     now - state.lastBuildQpc >= state.frequency / kMenuRebuildHz;
    if (state.layerVisible && (!changed || !due)) {
        noteDrawRefusal(kDrawNone);
        return;
    }

    // The GL state FIRST, so the clear inside presenterMenuBegin is not clipped by a
    // scissor box the eye passes left behind.
    GLint savedMatrixMode = GL_PROJECTION;
    gl.getIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
    gl.pushAttrib(static_cast<GLbitfield>(GL_ENABLE_BIT | GL_CURRENT_BIT |
                                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                                          GL_POLYGON_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT |
                                          GL_PIXEL_MODE_BIT | GL_VIEWPORT_BIT | GL_LINE_BIT));
    gl.disable(GL_TEXTURE_2D);
    gl.disable(GL_TEXTURE_1D);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_STENCIL_TEST);
    gl.disable(GL_SCISSOR_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_COLOR_LOGIC_OP);
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int actualWidth = 0;
    int actualHeight = 0;
    if (!presenterMenuBegin(canvasWidth, canvasHeight, actualWidth, actualHeight)) {
        gl.popAttrib();
        gl.matrixMode(static_cast<GLenum>(savedMatrixMode));
        noteDrawRefusal(kDrawNoLayer);
        return;
    }

    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    // Origin top-left, the convention every 2D pass in this project uses and the one the
    // HUD panel's texture already has -- which is what lets the presenter's shader and
    // the monitor's copy treat the two images identically.
    gl.ortho(0.0, static_cast<double>(actualWidth), static_cast<double>(actualHeight), 0.0,
             -1.0, 1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    drawCanvasContent(gl, actualWidth, actualHeight);

    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();

    presenterMenuEnd();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMatrixMode));

    // THE POSE, AND THE SPACE IT IS STATED IN.
    //
    // Everything but HUD tweak is the seat's: the surface was built there and its own
    // orientation is the quad's. HUD tweak publishes the HUD quad's own placement in the
    // HUD quad's own space instead -- head-locked when vrHudLayer is 1 -- because the
    // menu's quad is being put ON that panel, and a layer nailed to the seat cannot
    // follow a panel the runtime re-places against the head every display frame.
    SharedPose pose;
    int32_t    space = kMenuSpaceSeat;
    float      publishWidth = drawn.width;
    float      publishHeight = drawn.height;

    Vec3  offset;
    Quat  turn;
    float hudWidth = 0.0f;
    float hudHeight = 0.0f;
    if (state.mode == Mode::HudTweak &&
        hudQuadPlacement(offset, turn, hudWidth, hudHeight, space)) {
        pose.positionX = offset.x;
        pose.positionY = offset.y;
        pose.positionZ = offset.z;
        pose.orientationX = turn.x;
        pose.orientationY = turn.y;
        pose.orientationZ = turn.z;
        pose.orientationW = turn.w;
        publishWidth = hudWidth;
        publishHeight = hudHeight;
    } else {
        space = kMenuSpaceSeat;
        pose.positionX = drawn.centre.x;
        pose.positionY = drawn.centre.y;
        pose.positionZ = drawn.centre.z;
        pose.orientationX = drawn.orientation.x;
        pose.orientationY = drawn.orientation.y;
        pose.orientationZ = drawn.orientation.z;
        pose.orientationW = drawn.orientation.w;
    }

    if (!presenterMenuPublish(pose, space, publishWidth, publishHeight)) {
        noteDrawRefusal(kDrawNoLayer);
        return;
    }

    state.layerVisible = true;
    state.lastBuildQpc = now;
    _snprintf_s(state.canvasSignature, sizeof(state.canvasSignature), _TRUNCATE, "%s",
                signature);
    state.canvasSignatureValid = true;
    ++state.draws;
    noteDrawRefusal(kDrawNone);
}

void vrMenuNoteRayNeedsOffscreen() { noteRayRefusal(kRayNoOffscreen); }

bool vrMenuPribludaHidden() { return !g_pribludaOn; }

// The hand the ray comes from: the menu's owner, or over the replay panel the hand driving the
// pointer while it is awake. -1 when there is no ray to draw.
int rayHand() {
    const State& state = g_state;
    int hand = state.hand;
    if (state.mode == Mode::Closed) {
        if (!replayPanelShown() || windowInFrontEnd()) return -1;
        hand = state.replayHand;
    }
    if (hand < 0 || hand >= kHands) return -1;
    return vrInputHand(hand).present ? hand : -1;
}

bool vrMenuRayWanted() { return rayHand() >= 0; }

void vrMenuDrawRay(const GlFunctions& gl) {
    State& state = g_state;
    int    handIndex = rayHand();
    if (handIndex < 0) return;

    // WHY THIS ONE IS NOT A COMPOSITION LAYER, when the panel is.
    //
    // A quad layer is a flat image at a pose, and the compositor has no way to express
    // anything else. The panel is exactly that shape of thing, so it became a layer and
    // every fault that came with projecting it ourselves went away. A ray is the
    // opposite: it runs through DEPTH, from the hand to wherever it lands, and there is
    // no layer type -- the headers define projection, quad, cube, cylinder and equirect
    // and nothing else -- that can carry it. So it is real 3D geometry in the projection
    // layer, drawn per eye, and the per-eye projection lives here for its sake alone.
    if (!gl.begin || !gl.end || !gl.vertex2f || !gl.color4f || !gl.loadIdentity ||
        !gl.ortho || !gl.matrixMode || !gl.pushMatrix || !gl.popMatrix || !gl.pushAttrib ||
        !gl.popAttrib || !gl.getIntegerv || !gl.viewport || !gl.disable || !gl.enable ||
        !gl.blendFunc || !gl.colorMask || !gl.polygonMode || !gl.scissor)
        return;

    // The eye images only carry the world when the eye cameras were re-aimed at the
    // runtime's own frustum; without that the mapping below is not the one the world was
    // drawn with and the ray would point somewhere the geometry is not.
    if (!vrFrustumInjected()) {
        noteRayRefusal(kRayNoFrustum);
        return;
    }
    SharedView rendered[2] = {};
    if (!vrLastRenderedViews(rendered)) {
        noteRayRefusal(kRayNoViews);
        return;
    }
    int rects[2][4] = {};
    if (!stereoEyeRect(0, rects[0]) || !stereoEyeRect(1, rects[1])) {
        noteRayRefusal(kRayNoEyeRect);
        return;
    }

    const VrHandInput& hand = vrInputHand(handIndex);
    SharedPose         aim = {};
    if (!poseInSeat(hand.aimPose, aim)) {
        noteRayRefusal(kRayNoSeat);
        return;
    }

    const Vec3 origin = positionOf(aim);
    const Vec3 direction = normalise(rotate(quatOf(aim), {0.0f, 0.0f, -1.0f}));
    if (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f) {
        noteRayRefusal(kRayNoAim);
        return;
    }

    // Where it ends. On the panel when it is pointing at it -- the hit the poll already
    // worked out, so the ray and the crosshair cannot disagree -- and at a fixed length
    // otherwise, which reads as pointing at nothing rather than as the ray vanishing.
    // With the menu closed the ray is the replay panel's, and ends where the pointer poll last
    // found it on that panel.
    const bool    replay = state.mode == Mode::Closed;
    const Surface drawn = replay ? replayPanelSurface() : drawnSurface();
    const bool    on = replay ? state.replayHitOn : state.pointerOn;
    const float   onU = replay ? state.replayHitU : state.pointerU;
    const float   onV = replay ? state.replayHitV : state.pointerV;
    Vec3          farPoint;
    if (on && drawn.valid) {
        farPoint = surfacePoint(drawn, onU, onV);
    } else {
        farPoint = add(origin, scale(direction, kRayMissLength));
    }
    // Started a little ahead of the grip so it does not appear to come out of the
    // driver's own hand or through the wheel rim it is resting on.
    const Vec3 nearPoint = add(origin, scale(direction, kRayStartOffset));

    GLint surface[4] = {0, 0, 0, 0};
    int   targetWidth = 0;
    int   targetHeight = 0;
    if (renderTargetActive() && renderTargetSize(targetWidth, targetHeight) &&
        targetWidth > 0 && targetHeight > 0) {
        surface[2] = targetWidth;
        surface[3] = targetHeight;
    } else {
        gl.getIntegerv(GL_VIEWPORT, surface);
    }
    if (surface[2] <= 0 || surface[3] <= 0) {
        noteRayRefusal(kRayNoTarget);
        return;
    }

    GLint savedMatrixMode = GL_PROJECTION;
    gl.getIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
    gl.pushAttrib(static_cast<GLbitfield>(GL_ENABLE_BIT | GL_CURRENT_BIT |
                                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                                          GL_POLYGON_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT |
                                          GL_VIEWPORT_BIT));
    gl.viewport(surface[0], surface[1], surface[2], surface[3]);
    gl.matrixMode(GL_PROJECTION);
    gl.pushMatrix();
    gl.loadIdentity();
    gl.ortho(0.0, static_cast<double>(surface[2]), static_cast<double>(surface[3]), 0.0,
             -1.0, 1.0);
    gl.matrixMode(GL_MODELVIEW);
    gl.pushMatrix();
    gl.loadIdentity();

    gl.disable(GL_TEXTURE_2D);
    gl.disable(GL_TEXTURE_1D);
    gl.disable(GL_ALPHA_TEST);
    gl.disable(GL_STENCIL_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_LIGHTING);
    gl.disable(GL_FOG);
    gl.disable(GL_COLOR_LOGIC_OP);
    gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // DEPTH: NEITHER TESTED NOR WRITTEN, and that is a decision rather than an oversight.
    //
    // Testing it would need the ray's vertices to carry depths comparable with the ones
    // GPL wrote, which means reconstructing the engine's projection and its depth range
    // -- unverifiable from here and exactly the kind of guess that costs a test run. And
    // being occluded by the cockpit would be INCONSISTENT with what the ray points at:
    // the menu is a composition layer, drawn by the runtime over everything, so a ray
    // that stopped at the wheel while the panel it aims at floated in front of the wheel
    // would look more wrong, not less. Both ends of the pointer are therefore on top.
    //
    // What keeps it from reading as a solid rod through the bodywork is the shape: it
    // starts ahead of the hand, it is a couple of millimetres thick, it tapers, and its
    // alpha falls away along its length so the far end is nearly gone.
    gl.disable(GL_DEPTH_TEST);
    if (gl.depthMask) gl.depthMask(GL_FALSE);

    const int surfaceHeight = surface[3];
    for (int eye = 0; eye < 2; ++eye) {
        const SharedView& view = rendered[eye];
        SharedPose        eyeSeat = {};
        if (!poseInSeat(view.pose, eyeSeat)) continue;

        const Vec3  eyePosition = positionOf(eyeSeat);
        const Quat  inverseEye = conjugate(quatOf(eyeSeat));
        const float tanLeft = std::tan(view.fov.angleLeft);
        const float tanRight = std::tan(view.fov.angleRight);
        const float tanUp = std::tan(view.fov.angleUp);
        const float tanDown = std::tan(view.fov.angleDown);
        if (!(tanRight > tanLeft) || !(tanUp > tanDown)) continue;

        const float rx = static_cast<float>(rects[eye][0]);
        const float ry = static_cast<float>(rects[eye][1]);
        const float rw = static_cast<float>(rects[eye][2]);
        const float rh = static_cast<float>(rects[eye][3]);
        if (!(rw > 0.0f) || !(rh > 0.0f)) continue;

        // Clipped to its own eye, so a segment whose projection runs away near the eye
        // plane cannot reach the other eye's half of the buffer.
        gl.enable(GL_SCISSOR_TEST);
        gl.scissor(static_cast<GLint>(rx),
                   surfaceHeight - static_cast<GLint>(ry) - static_cast<GLint>(rh),
                   static_cast<GLint>(rw), static_cast<GLint>(rh));

        auto project = [&](const Vec3& point, float& px, float& py) -> bool {
            const Vec3 local = rotate(inverseEye, sub(point, eyePosition));
            if (local.z > -0.05f) return false;
            px = rx + (local.x / -local.z - tanLeft) / (tanRight - tanLeft) * rw;
            py = ry + (tanUp - local.y / -local.z) / (tanUp - tanDown) * rh;
            return true;
        };

        // A ribbon that turns to face this eye, one quad per segment. Facing the eye is
        // what makes a flat strip read as a round beam from any angle; the alternative,
        // a fixed-up ribbon, disappears edge-on.
        float previousPixels[2][2] = {};
        bool  havePrevious = false;
        for (int step = 0; step <= kRaySegments; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(kRaySegments);
            const Vec3  point = add(nearPoint, scale(sub(farPoint, nearPoint), t));
            const Vec3  toEye = normalise(sub(eyePosition, point));
            const Vec3  side = normalise(cross(normalise(sub(farPoint, nearPoint)), toEye));
            const float half = kRayNearHalfWidth +
                               (kRayFarHalfWidth - kRayNearHalfWidth) * t;

            float pixels[2][2] = {};
            const bool ok = project(add(point, scale(side, half)), pixels[0][0],
                                    pixels[0][1]) &&
                            project(sub(point, scale(side, half)), pixels[1][0],
                                    pixels[1][1]);

            if (ok && havePrevious) {
                const float previousT =
                    static_cast<float>(step - 1) / static_cast<float>(kRaySegments);
                const float alphaA =
                    kRayNearAlpha + (kRayFarAlpha - kRayNearAlpha) * previousT;
                const float alphaB = kRayNearAlpha + (kRayFarAlpha - kRayNearAlpha) * t;
                gl.begin(GL_QUADS);
                gl.color4f(1.0f, 0.88f, 0.20f, alphaA);
                gl.vertex2f(previousPixels[0][0], previousPixels[0][1]);
                gl.vertex2f(previousPixels[1][0], previousPixels[1][1]);
                gl.color4f(1.0f, 0.88f, 0.20f, alphaB);
                gl.vertex2f(pixels[1][0], pixels[1][1]);
                gl.vertex2f(pixels[0][0], pixels[0][1]);
                gl.end();
            }

            havePrevious = ok;
            previousPixels[0][0] = pixels[0][0];
            previousPixels[0][1] = pixels[0][1];
            previousPixels[1][0] = pixels[1][0];
            previousPixels[1][1] = pixels[1][1];
        }

        gl.disable(GL_SCISSOR_TEST);
    }

    ++state.rayDraws;
    noteRayRefusal(kRayNone);

    gl.matrixMode(GL_MODELVIEW);
    gl.popMatrix();
    gl.matrixMode(GL_PROJECTION);
    gl.popMatrix();
    gl.popAttrib();
    gl.matrixMode(static_cast<GLenum>(savedMatrixMode));
}

bool vrMenuActive() { return g_state.mode != Mode::Closed; }

bool vrMenuReplayPanelHit(int hand) {
    if (hand < 0 || hand >= kHands) return false;
    if (windowInFrontEnd() || !replayPanelShown()) return false;
    const VrHandInput& input = vrInputHand(hand);
    if (!input.present) return false;
    // The pointer's own test, on the raw ray: it clicks only where this is true.
    SharedPose aimSeat = {};
    float      u = 0.0f;
    float      v = 0.0f;
    return poseInSeat(input.aimPose, aimSeat) &&
           rayToSurface(replayPanelSurface(), aimSeat, u, v);
}

bool vrMenuReplayClickHeld() { return g_state.frontEndButtonDown; }

void vrMenuReleaseGl(const GlFunctions& gl, bool contextAlive) {
    vrTextRelease(gl, contextAlive);
    quickMenuReleaseGl();
    replayQuickMenuReleaseGl();
    // The layer's texture belongs to the context that is going, and Producer releases it
    // with the rest. Forgetting the signature is what makes the next frame rebuild the
    // image into the new one rather than deciding nothing has changed.
    g_state.canvasSignatureValid = false;
    g_state.layerVisible = false;
    g_state.lastBuildQpc = 0;
}

bool vrMenuFormatStatus(char* out, size_t size) {
    State& state = g_state;
    unsigned refused = 0;
    int      worst = 1;
    for (int i = 1; i < kDrawRefusalCount; ++i) {
        refused += state.refusals[i];
        if (state.refusals[i] > state.refusals[worst]) worst = i;
    }
    if (!state.draws && !refused && !state.opens && !state.rayHits && !state.rayMisses)
        return false;

    const char* modeName = "closed";
    switch (state.mode) {
        case Mode::Menu: modeName = "menu"; break;
        case Mode::MirrorTweak: modeName = "mirror tweak"; break;
        case Mode::HudTweak: modeName = "HUD tweak"; break;
        case Mode::PanelTweak: modeName = "panel tweak"; break;
        case Mode::Confirm: modeName = "save changes?"; break;
        case Mode::Reset: modeName = "reset what?"; break;
        default: break;
    }

    // The ray counts are the ones to read when the menu is open and nothing responds:
    // all misses means the ray never reaches the panel, and that is a different fault
    // from the panel not being drawn.
    unsigned rayRefused = 0;
    int      rayWorst = 1;
    for (int i = 1; i < kRayRefusalCount; ++i) {
        rayRefused += state.rayRefusals[i];
        if (state.rayRefusals[i] > state.rayRefusals[rayWorst]) rayWorst = i;
    }

    _snprintf_s(out, size, _TRUNCATE,
                "vr menu: %s, %u opened, %u commands, %u layers built, %u refused%s%s | "
                "ray %u on the panel, %u missed | beam drawn %u, refused %u%s%s | %s tab, row %d "
                "(%s)",
                modeName, state.opens, state.commands, state.draws, refused,
                refused ? " -- mostly " : "", refused ? drawRefusalName(worst) : "",
                state.rayHits, state.rayMisses, state.rayDraws, rayRefused,
                rayRefused ? " -- mostly " : "", rayRefused ? rayRefusalName(rayWorst) : "",
                g_rows[tabRow(g_openTab)].label, state.hovered,
                state.stickOwnsHighlight ? "stick" : "pointer");

    state.draws = 0;
    state.opens = 0;
    state.commands = 0;
    state.rayHits = 0;
    state.rayMisses = 0;
    state.rayDraws = 0;
    for (unsigned& count : state.refusals) count = 0;
    for (unsigned& count : state.rayRefusals) count = 0;
    return true;
}

}  // namespace gplvr
