#pragma once

// The in-race menu, the tweak modes and the front end's pointer -- everything a motion
// controller does.
//
// TWO SURFACES, because the game has two and they are nothing alike.
//
//   IN A RACE the menu is an OpenXR QUAD COMPOSITION LAYER of its own, exactly as
//   pribluda's HUD panel already is: its content is drawn flat into the layer's texture
//   and the runtime places and rotates the quad. That is what makes the text turn with
//   the panel -- glBitmap is screen-axis-aligned and can never be rotated, so text drawn
//   into the eye buffers stayed level while the panel rolled -- and it removes the whole
//   class of faults that came with projecting a quad into the eye rects ourselves: one
//   eye's panel bleeding into the other's half, the tangent running away near the eye
//   plane, text dropped because its raster position fell outside the viewport. It is
//   also sharper, being composited at the layer's own resolution, and steadier, being
//   reprojected at display time rather than at ours.
//
//   What stays ours is the placement -- a pose in the SEAT's space, captured yaw-only
//   from the head when the menu opens -- and the hit test, a ray against that same quad,
//   which hands back the UV the cursor is drawn at.
//
//   The monitor is the exception and needs the projection anyway: a composition layer
//   never reaches GPL's own window, so Producer.cpp draws the quad there a second time
//   through drawPanelInEye, the same function that has always done it for the HUD panel.
//
//   IN THE FRONT END there is no stereo, no world and no frustum -- one 640x480 image
//   shown to both eyes. There is nothing to place a quad against, so the controller
//   drives the REAL WINDOWS POINTER instead: the aim direction relative to the head maps
//   over a fixed angular span onto the scaled front end's client rect (menuScaleClientRect),
//   SetCursorPos puts the pointer there, and the trigger is a left click through SendInput.
//   Everything downstream then works unchanged -- gpl_mouseReadClientDelta reads the
//   pointer it always read, MenuScale's ScreenToClient hook maps it, and CursorOverlay
//   draws the arrow into the frame so the headset can see it.
//
// WHAT IT ADJUSTS. Nothing directly. Every change goes through TweakApi as a semantic
// command, and which physical control produces which command is TweakApi's binding table.
// This file owns the modes, the panel, the hit tests and the drawing, and knows what a
// thumbstick is only in so far as it hands one to the table.
//
// ONE HAND AT A TIME. Both controllers offer the same menu; whichever opens it claims the
// interactive surface from VrInput and holds it until it exits or stops being tracked.

#include "GlInterop.h"

#include <cstddef>

namespace gplvr {

// Once a frame, from the key poll on the render thread, after vrInputPoll. Decides the
// mode, hit-tests the pointer, applies the bindings and drives the front end's pointer.
void vrMenuPoll();

// Build the menu's layer image and publish it, from the present hook, with the game's GL
// context current. It does NOT draw into the frame: the menu is an OpenXR quad
// composition layer of its own, which the runtime composites at the display's rate.
//
// Inert unless a mode is open, and it rebuilds the image only when what the panel says
// has changed. Every refusal is counted and named in the once-a-second line rather than
// passing silently -- a menu that is open and invisible looks exactly like a menu that
// never opened.
void vrMenuUpdate(const GlFunctions& gl);

// The controller beam the presenter draws, on a layer of its own over every panel, from its own
// aim sample: the hand pointing and how many metres along its aim ray the beam reaches -- to the
// spot the pointer is on, or a fixed length when it points at nothing. One beam for every
// pointer: a mode's (the menu, a tweak mode, a dialog) wherever it is open, the replay panel's
// with the menu closed, and the front end's on GPL's fixed menu panel (vrGplMenuMode 1). `mark`
// asks for a cross at the end, for that last one only: the other panels draw their own. False
// when no hand is pointing. Headset only; the monitor never shows it.
bool vrMenuBeam(int& hand, float& length, bool& mark);

// A controller is driving the front end's pointer onto GPL's fixed menu panel. The beam marks
// where it points, so the Windows pointer is not drawn into the picture meanwhile.
bool vrMenuBeamOnGplMenu();

// True while vrHud (the menu's "Pribluda HUD" row) is off: skip the HUD pass entirely.
// The quad layer needs no separate handling: the presenter already drops it when the game stops
// drawing the panel, so not calling pribludaDrawHud takes the layer down with the monitor's
// overlay.
bool vrMenuPribludaHidden();

// True while the menu or a tweak mode is open, so the key poll can keep the game from
// also acting on whatever the driver is doing.
bool vrMenuActive();

// True when `hand`'s aim ray meets the replay panel this poll -- the same test the replay
// pointer applies before it clicks, so a press it is true for belongs to GPL's widgets and one it
// is false for is never turned into a click. False when the panel is not up.
bool vrMenuReplayPanelHit(int hand);

// True while the replay pointer holds GPL's left button down: a click on the panel is in
// progress, and the replay radial does not open over it.
bool vrMenuReplayClickHeld();

// The GL context is going away: forget the font's display lists.
void vrMenuReleaseGl(const GlFunctions& gl, bool contextAlive);

// Every font size the VR menu, its tweak strips and the radials draw with, each once, into
// `out`; returns how many. For the prebuild, which makes them all before the first race.
int vrMenuFontPixels(int* out, int capacity);

// The once-a-second line. False when nothing has happened worth a line.
bool vrMenuFormatStatus(char* out, size_t size);

}  // namespace gplvr
