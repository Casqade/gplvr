#pragma once

// The game window: who owns its geometry, and what the engine is told its mode is.
//
// GPL asks the rasterizer for a display mode, the rasterizer changes the desktop to it
// and then resizes the window to match. That is the whole of its window policy, and
// every part of it is wrong for a machine from this decade: the desktop flickers through
// 640x480 twice per race, the window is pinned to whatever core.ini happens to say, and
// a mode change costs up to five seconds of polling before the game continues.
//
// Three decompiled facts make this cheap to take over, and each is commented in the
// roglv2 IDB:
//
//   - `screenBegin` (the rasterizer entry point GPL calls to enter a mode) takes ONE
//     __cdecl argument: a pointer to a 0xFC-byte mode record, {+0 id, +4 width,
//     +8 height, +0x10 bpp, ...}. Every caller in gplc67 builds that record in a LOCAL
//     STACK BUFFER filled by screenGetFirstMode/screenGetNextMode -- verified in
//     gpl_startupScreenInit_640x480, sub_4F3310 and sub_4F3250, which are all three of
//     them -- so rewriting its width and height on the way past is safe. It is a
//     temporary, not an entry in the rasterizer's mode table.
//
//   - `rogl_windowSetSize` is guarded by `if (wm[2] != cx || wm[3] != cy)`, and inside
//     that guard live the SetWindowPos, the HWND_TOPMOST demand and the 500 x Sleep(10)
//     poll. Writing those two DWORDs before the call skips all three.
//
//   - roglv2's WM_GETMINMAXINFO arm answers with wm[0] and wm[1], and nothing else in
//     the DLL reads them. So the pair that pins the window is separate from the pair
//     that gates the resize, and they can be given different values: the window's size
//     for the first, the mode's for the second.
//
// The window manager object is not addressed by a constant. roglv2's window proc is a
// two-instruction stub -- `mov ecx, rogl_windowManager` / `jmp WindowManager::WndProc`
// -- so the pointer to it is read out of the instruction that loads it.

#include "Discovery.h"

#include <windows.h>

namespace gplvr {

// Resolve the window manager and thunk `screenBegin`. A discovery that fails leaves the
// game with its own window policy and says so.
void windowInstall(const Binding& binding);

// Once a frame, from the present hook: re-assert the draw gate.
void windowFrame();

// Whether the game window had the foreground at the last focus edge.
bool windowHasFocus();

// Once a second, from the present hook: re-assert the window geometry and report the
// window's state when it changes.
void windowEnforceGeometry();


// The client rectangle the window should occupy, in screen coordinates. False when
// the window manager has not been resolved, which leaves the game its own policy.
bool windowTargetRect(RECT& out);

// Ask the engine which mode it is in. Once a frame, from the present hook.
//
// Read rather than tracked: `screenBegin` is a stack push and `screenEnd` is the pop, so
// following the calls we hook gets the pushes and misses every pop -- which is how a
// front end came to be drawn at 640x480 into the corner of a window gplvr still thought
// was in the world mode.
void windowRefreshEngineMode();

// What the engine says its display mode is. Zero until the read above has succeeded.
void windowEngineModeSize(int& width, int& height);

// True while that mode is GPL's 640x480 front end.
bool windowInFrontEnd();

// GPL fixes the HORIZONTAL field of view and derives the vertical from the surface's
// shape -- `tan(vhalf) = tan(fov/2) * H/W` with square pixels -- so a wider window shows
// LESS, not more. A 2560x1440 window keeps three quarters of the design vertical field
// with the bars in and 95% with them out; a 21:9 keeps far less. This returns the fov
// that restores the 4:3 design view exactly. False means leave the game's own alone.
bool windowCorrectMainFov(float fov, float& corrected);

// The window gained or lost the foreground. Driven from a POLLED GetForegroundWindow
// edge in the frame hook, because WM_ACTIVATEAPP is not delivered to GPL's window on
// every system -- measured: 1373 messages reached gplvr's proc across four focus changes
// and not one was WM_ACTIVATEAPP, and roglv2's draw gate, which the same message drives,
// did not clear either. Idempotent, so the message path may also call it.
void windowOnFocusChanged(bool focused);

// Messages the outermost window proc offers up before passing them down the chain.
// Returns true when the message was answered, and `result` is the answer.
bool windowHandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM& lParam,
                         LRESULT& result);

}  // namespace gplvr
