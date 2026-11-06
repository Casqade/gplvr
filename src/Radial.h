#pragma once

// THE RADIAL BOTH QUICK MENUS ARE MADE OF: a ring of slices ahead of the eyes, picked by turning
// the hand or pushing the stick, on the VR menu's quad layer.
//
// What it does not know is what the slices mean. The in-car quick menu (QuickMenu.cpp, four
// slices) and the replay menu (ReplayQuickMenu.cpp, eight) each decide which slice is greyed and
// why, what fires on release, and when the radial must close; this file only steers it and draws
// it, so both look and choose the same way.
//
// STEERING. At the press the aim ray's direction is captured, with right and up across it taken
// against the world's up, so wrist roll does not turn "up". Turning the hand kRadialSelectDegrees
// from that direction, or pushing the stick to kRadialStickSelect, selects; whichever is further
// along its own scale wins. The angle of the turn (or of the stick) picks the slice whose sector
// contains it. A selection holds until the deflection falls under three quarters of the
// threshold, and the sector of the selected slice is widened by the layout's hysteresis, so a
// hand resting on a boundary does not flicker between two slices.
//
// PLACEMENT. Like the VR menu's panel: put where the head faces at the press, heading only,
// 0.62 m ahead and 6 cm below the eyes, and stated in the seat's frame, so it then stays still in
// the cockpit while the head moves. Before the first recentre there is no seat, and that press's
// ring follows the head instead (logged).
//
// DRAWING. A 640 px square canvas, 0.30 m across: annular slices round an empty centre disc
// (letting go there cancels), each slice's label, a greyed slice's reason under its label in a
// smaller size, and a yellow dot where the hand or stick is pushing. The image is rebuilt only
// when what it shows changes, at most 45 times a second.

#include "GlInterop.h"
#include "VrInput.h"

#include <cstddef>

namespace gplvr {

constexpr int kRadialNone = -1;
constexpr int kRadialMaxSlices = 8;

// One slice as the menu that owns it decides it this poll.
struct RadialSlice {
    bool        enabled = false;
    // A release counts only once the slice has been held for the owner's dwell; drawn as a ring
    // filling along the slice's outer edge, with "hold to confirm" under the label.
    bool        dwell = false;
    const char* label = "";
    char        reason[40] = {};
};

// Greys the slice and says why, under its label.
void radialSetReason(RadialSlice& slice, const char* text);

// The ring's shape.
struct RadialLayout {
    int          count;       // slices, at most kRadialMaxSlices
    const float* angles;      // where each slice points, degrees anticlockwise from the right
    float        halfWidth;   // each slice spans its angle plus and minus this, in degrees
    float        hysteresis;  // degrees the selected slice's sector is widened by
    // A label wider than this many canvas pixels is broken at the space nearest its middle onto
    // two lines; the narrow slices of an eight-way ring need it.
    float        labelWidth;
};

// The steering and the layer, one per menu.
struct Radial {
    float    pressDirection[3] = {0.0f, 0.0f, -1.0f};
    float    pressRight[3] = {1.0f, 0.0f, 0.0f};
    float    pressUp[3] = {0.0f, 1.0f, 0.0f};
    int      selected = kRadialNone;
    LONGLONG selectedAt = 0;
    float    level = 0.0f;      // the winning deflection, 1 at the selection threshold
    float    direction = 0.0f;  // its angle, degrees anticlockwise from the right
    bool     usedStick = false;

    // Where the panel sits, captured at the press: in kMenuSpaceSeat, or kMenuSpaceView when
    // there was no seat to state it in.
    SharedPose panelPose = {};
    int32_t    panelSpace = 0;

    bool     layerVisible = false;
    bool     signatureValid = false;
    char     signature[768] = {};
    LONGLONG lastBuild = 0;
    // For the owner's status line, which zeroes them.
    unsigned layersBuilt = 0;
    unsigned drawRefused = 0;
};

// The trigger went down: capture the press frame from `hand` and start with nothing selected.
void radialBegin(Radial& radial, const VrHandInput& hand, LONGLONG now);

// Once a poll while open: the selection from the hand's turn and the stick.
void radialSteer(Radial& radial, const RadialLayout& layout, const VrHandInput& hand,
                 LONGLONG now);

// Closed: nothing selected.
void radialEnd(Radial& radial);

// Build and publish the radial's image on the VR menu's quad layer, with the game's GL context
// current. `dwell` is the selected slice's hold progress from 0 to 1, or negative when it has
// none. True while the radial holds the layer (`open`); false once it is closed, after hiding the
// layer once if this radial had it, which hands it back to the replay panel and GPL's dialogs.
bool radialUpdate(Radial& radial, const RadialLayout& layout, const RadialSlice* slices,
                  bool open, float dwell, LONGLONG now, LONGLONG frequency,
                  const GlFunctions& gl);

// The GL context is going away; the next open builds its image afresh.
void radialReleaseGl(Radial& radial);

// The two font sizes a radial draws with, for the menus' prebuild.
void radialFontPixels(int& bodyPixels, int& smallPixels);

}  // namespace gplvr
