#pragma once

// Pribluda's in-race HUD, in the headset, on a panel.
//
// This is the overlay a GPL driver actually reads: lap and sector times, the
// leader/player/track boards, tyre temperatures, oil pressure and temperature, fuel,
// gear, rpm, its own fps counter. It is NOT GPL's own in-race text -- that one is
// `stampDraw` and `drawDebugText`, and it is handled in Hud.cpp.
//
// WHY NINE RASTERIZER HOOKS NEVER SAW IT. pribluda.dll imports glViewport, glCallLists,
// glListBase, wglUseFontBitmapsA, glColor3f and glTranslatef from OPENGL32, TextOutA from
// GDI32 and GetClientRect from USER32, nothing from roglv2, and exports nothing. It draws
// raw OpenGL display-list fonts inside roglv2's present, after gplvr has taken the frame.
//
// HOW IT IS INVOKED. Its DllMain patches roglv2+0x9B83 with `nop; call rel32`, so roglv2's
// own per-frame path lands in its hook, and gplc67 at 0x45CA61 (the telemetry tick) and
// 0x4EF134 (enter-to-pits).
//
// WHAT MAKES A COPY POSSIBLE. The draw is one __cdecl function of three globals and is pure
// drawing -- the race state is collected by the gplc67 hook -- and every string is placed
// from two screen-size ints with `gluOrtho2D(0, w, 0, h)`. Writing those two ints and
// setting the viewport is enough to put the whole overlay anywhere.
//
// HOW IT REACHES THE HEADSET. Once a frame the overlay is drawn into a shared texture that
// the presenter composites as an OpenXR quad layer, head-locked or fixed to the seat, placed
// from gplvr.ini (vrHudLayer, vrHudHead* and vrHudSeat*). The runtime
// places the quad itself on every display frame, so it holds still however the game's own
// frames are paced, and its text is sampled once, straight to the display. Pribluda's own
// call still draws the monitor.
//
// A LAYOUT OF ITS OWN. pribluda.ini is read once into one config object, so gplvr keeps a
// copy, refreshed from the live one every frame so hotkeys still reach it, overwrites the
// positions from gplvr_pribluda.ini -- fractions of the panel -- and passes the copy to the
// draw. The monitor keeps its own layout.
//
// Addresses and reasoning: prib_hudDraw, prib_rasterHook_drawsHud and
// prib_gplHook_telemetryTick in the pribluda.dll IDB.

namespace gplvr {

// IS PRIBLUDA THERE AT ALL. Three answers and not two, because pribluda.dll is loaded by
// gpl_rasterBegin long after gplvr attaches: until a frame has been presented, "no module"
// means "not yet" as readily as it means "not installed", and a menu that hid its rows on
// the first answer would hide them on every install. `kPribludaUnknown` is therefore the
// answer to treat as yes -- offer the rows, and let them refuse the way they already do.
//
// It turns Absent when a run of presented frames has gone by with no pribluda.dll in the
// process (gplc67 built without the GEM+ pribluda patch, or the 2020 demo), or when the
// module is there and is not a build gplvr can call -- which the log has already explained.
enum PribludaPresence {
    kPribludaUnknown = 0,
    kPribludaAbsent,
    kPribludaPresent,
};

PribludaPresence pribludaPresence();

// Draw the overlay into the HUD panel texture. Call it from the present hook, where the
// game's GL context is current. Inert unless vr and vrHudLayer are on, the game is in a
// race, and pribluda is loaded with its font lists built. Resolution is lazy: pribluda is
// loaded by gpl_rasterBegin long after gplvr attaches, and is absent entirely if the player
// has not installed it.
void pribludaDrawHud();

// HUD tweak mode: vrHudTweakKey, the arrows and Page Up/Down, polled once a frame from the
// key poll on the render thread, before pribludaDrawHud. Entered only while the panel is
// being drawn; left, with the positions written to gplvr_pribluda.ini, on the key or when
// the panel stops being drawn. The outline of the selected element is drawn by
// pribludaDrawHud.
void pribludaPollTweak();

// True for a key press tweak mode is using while it is on, which the window proc then
// keeps from the game.
bool pribludaTweakSwallowsKey(unsigned virtualKey);

// Leave HUD tweak mode if it is on, saving as the key does. `why` is appended to the log
// line. Mirror tweak mode calls it on entering, since the two share the arrows and Page Up/Down.
void pribludaTweakExit(const char* why);

// -- the panel's layout, for TweakApi -----------------------------------------------------
//
// The element table, the selection and the positions are this file's, and they stay this
// file's: these are the narrowest accessors that let the semantic tweak API move a HUD
// element without a second copy of the clamps, the fraction arithmetic or the save rules.
// The keyboard mode above and the API below both go through them, so the two cannot
// disagree about where an element is -- but they must not both be ON at once, which is
// what pribludaTweakActive is for.
//
// Positions are FRACTIONS of the panel, which is what gplvr_pribluda.ini holds.

int         pribludaElementCount();
const char* pribludaElementName(int element);

// True when the layout file gives the element a position -- which is what an outline is drawn
// round and what the stick moves. It does NOT require the element to be shown: a hidden one
// has to stay reachable or there is no way to turn it back on.
bool pribludaElementUsable(int element);

// Whether the element is currently switched on. A tweak mode crosses out the ones that are
// not, rather than leaving them out, so they can be found and toggled back.
bool pribludaElementShown(int element);

// Delete gplvr_pribluda.ini and load the shipped layout again. For the menu's Reset dialog.
bool pribludaResetLayout();

int  pribludaElementSelected();
void pribludaElementSelect(int element);
// The next usable element `step` places away, wrapping. Returns the new selection.
int  pribludaElementCycle(int step);

// The element's position, as fractions of the panel. False when the layout file gives it
// none.
bool pribludaElementPosition(int element, float& x, float& y);

// Move it by, or put it at, a fraction of the panel. Kept on the panel in the direction it
// is moving and never pulled back from where the file already put it -- the same rule the
// arrow keys move by, because it is the same code. False when nothing changed.
bool pribludaElementMove(int element, float dx, float dy);
bool pribludaElementSetPosition(int element, float x, float y);

// The position gplvr_pribluda.ini holds for it, re-read now. The file IS the default:
// there is no separate factory value, so this is what a reset goes back to and what a
// slider draws as its home mark.
bool pribludaElementFilePosition(int element, float& x, float& y);
bool pribludaElementResetPosition(int element);

// The element's show flag as the layout override holds it: -1 when the file says nothing
// about it, otherwise 0 or 1. Setting it reaches the next frame's copies, and saveLayout
// writes it back. An element the file says nothing about keeps saying nothing, so it goes
// on following pribluda's own hotkeys.
int  pribludaElementShow(int element);
bool pribludaElementSetShow(int element, int show);

// The dash's width, as a fraction of the panel. -1 when the file gives none.
float pribludaDashWidth();
bool  pribludaSetDashWidth(float fraction);

// Write the layout to gplvr_pribluda.ini, as leaving the keyboard mode does: every
// position, the dash's width and every show flag the file sets.
void pribludaLayoutSave();

// True while the KEYBOARD tweak mode holds the panel. The tweak API refuses while it does.
bool pribludaTweakActive();

// True while the panel is being drawn often enough to count as on screen -- a race, with
// the panel on. Nothing can be placed on a panel nobody is looking at.
bool pribludaPanelLive();

// The panel texture's size in texels, as the last draw was given it. False before the
// first draw. Its aspect is what the presenter sizes the quad layer by, so this is what a
// ray test against the panel in the world has to use.
bool pribludaPanelSize(int& width, int& height);

// The selected element's outline, in fractions of the panel: where it is and how big the
// last draw measured it. False when it has no position or has never been drawn. This is
// what lets a pointer hit-test the elements without knowing anything about pribluda.
bool pribludaElementBox(int element, float& x, float& y, float& width, float& height);

// The panel on the monitor, VR only. prib_rasterHook_drawsHud (RVA 0x11950) -- the hook pribluda patches into
// roglv2's present -- polls its hotkeys, builds its font lists, counts the frame and then
// calls prib_hudDraw with `call rel32` at RVA 0x119B7. gplvr points that one call at its own
// function, which skips the monitor draw on frames presenterMonitorHudTaken() reports and
// calls prib_hudDraw unchanged on every other. True once that redirect is in place.
bool pribludaMonitorDrawRedirected();

}  // namespace gplvr
