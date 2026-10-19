#pragma once

// GPL'S REPLAY CONTROLS, ON A PANEL OF THEIR OWN IN THE HEADSET.
//
// The replay screen is laid out for the window: gpl_replayScreenLayout (0x41A120) centres a
// 640-wide design band horizontally and puts its two groups at fixed rows -- HiResTop at
// y = (50H/480 - 50)/2 and HiResBottom at y = 430H/480 - 430, each 50 design rows tall. In VR
// those window pixels land in the off-screen frame unchanged, which is the left eye's top-left
// corner at infinity, while the pointer is mapped somewhere else entirely -- so the buttons
// could be seen but not pressed.
//
// So in a replay every GPL 2D call that falls inside one of the two bands -- the button art
// through bitmapDraw, the labels through the fast text path -- is drawn into a layer of our own
// instead of the eye frame, with roglv2's own transform shifted so GPL's pixel (x, y) lands on
// the layer's (x, y). The two bands are then stacked into one image and shown on the VR menu's
// quad layer, which is free whenever the menu is closed: a flat panel in front of and below
// the driver, seat-locked, at its own depth. The controller's ray is tested against that same
// panel and the hit is turned back into window coordinates, so the Windows pointer -- which
// GPL's widgets respond to -- is put on the button the driver is pointing at.
//
// Everything outside the two bands keeps the path it had: the notices and the HUD are still
// placed per eye by the fast text path.
//
// GPL'S IN-RACE DIALOGS GO ON THE SAME LAYER. F1's help is a list widget (sub_4FD860): a 640x480
// root at the centre of the layout, filled at alpha 0, over a translucent box, with frame art,
// scroll arrows and twenty lines of text. Placed per eye, the text was spread apart to stay
// legible and the box and its art could not follow it (2026-10-18). A race draws no fills of its
// own, so every fill on GPL's 2D frame in a race marks a dialog: while one is up, every GPL 2D
// call inside the rectangle the frame's fills cover goes into the layer, and that rectangle is
// shown seat-locked ahead of the driver -- the box, its art and its text where GPL laid them out,
// like the scaled front end, over the stereo picture. The pointer is not put on it: the dialogs
// are keyboard-driven, and a race may be steering with the mouse.

#include "GlInterop.h"

namespace gplvr {

// True while GPL's foreground state is its replay screen -- an instant replay from the race or a
// replay loaded from the menu alike.
bool gplReplayScreenShown();

// True while GPL's 2D is being collected: VR, a world mode, the off-screen target active, a
// layout of at least 640x480, and either the replay screen showing or a dialog up in a race.
bool replayPanelCapturing();

// A fill GPL drew on its own 2D frame, in its layout pixels. Outside a replay it marks a dialog:
// the rectangle this frame's fills cover is what the panel collects and shows.
void replayPanelNoteFill(int x, int y, int width, int height);

// Whether GPL's draw of this rectangle -- its own layout pixels, the destination's origin
// included -- belongs on the panel. Counts what it takes.
bool replayPanelTakes(int x, int y, int width, int height);

// The panel's layer as the draw target for one GPL 2D call, for the lifetime of the object.
// roglv2's viewport is moved so the call's pixels land where GPL put them in a layer the size of
// its layout; the previous framebuffer and viewport come back on destruction. The first bind
// after GPL repainted its 3D view clears the layer, which is what the 3D view does to the bars'
// area in flat mode. bound() is false when the layer could not be made, and the call then draws
// where it always did.
class ReplayPanelTarget {
public:
    ReplayPanelTarget();
    ~ReplayPanelTarget();
    ReplayPanelTarget(const ReplayPanelTarget&) = delete;
    ReplayPanelTarget& operator=(const ReplayPanelTarget&) = delete;
    bool bound() const { return m_bound; }

private:
    bool  m_bound = false;
    GLint m_framebuffer = 0;
    GLint m_viewport[4] = {};
};

// EVERY DISTINCT 2D DRAW WHILE THE REPLAY SCREEN IS UP, said once each per replay: what kind of
// call, where in GPL's layout and how big, whether the panel took it, and a detail (the text of a
// drawDebugText, the class of a destination). For finding a widget that is on screen and not on
// the panel. Glyphs only when the panel did NOT take them, or one line of text is forty entries.
void replayPanelNoteDraw(const char* kind, int x, int y, int width, int height, bool taken,
                         const char* detail);

// GPL repainted its 3D view: the next capture starts from an empty layer. From the scene hook,
// for GPL's own frame only -- an extra render repaints the eyes, not GPL's layout.
void replayPanelNoteScene();

// Once a frame from the VR menu's update, on the render thread with the game's context
// current, after GPL's 2D for the frame. `layerFree` is true while the VR menu is closed and its
// quad layer is unused; the panel then publishes its image there, and hides the layer again
// when the replay ends or the dialog closes.
void replayPanelUpdate(const GlFunctions& gl, bool layerFree);

// True while the replay panel is on the layer, so the pointer is tested against it and the cursor
// is drawn on it rather than into the eyes. False for a dialog, which takes no pointer.
bool replayPanelShown();

// Where the panel hangs, in the SEAT's frame and OpenXR's axes: centre, orientation (x, y, z,
// w; +Z out of the visible face) and extent in metres.
void replayPanelPlacement(float centre[3], float orientation[4], float& width, float& height);

// A point on the panel, in fractions of it (u right, v down), as a point in the game window's
// client area. False when the layout is unknown.
bool replayPanelClientPoint(float u, float v, POINT& client);

// Once a second from the frame report.
void replayPanelReport();

}  // namespace gplvr
