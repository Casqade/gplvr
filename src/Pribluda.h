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

}  // namespace gplvr
