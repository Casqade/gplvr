#pragma once

// Whether one of GPL's text fields has the keyboard, so gplvr's plain-key hotkeys can stay out of
// a name being typed: a car setup or championship save name, the driver's name, an address or a
// chat line.
//
// GPL has one text-entry widget, the edit box (vtables gpl_editBoxVtbl 0x53CC50 and its subclass
// gpl_editBox2Vtbl 0x53CCD8, both dispatching to gpl_editBoxOnEvent 0x4F69C0). A typed character
// reaches it as fg event 0xB0B through gpl_widgetMgrBroadcastEvent, and gpl_editBoxOnEvent takes
// it only when the box is writable (dword +0x149 clear) and focused (byte +9 set). Every box GPL
// builds lives in the child vector (+0x39: count, capacity, growth, data) of a window
// (gpl_windowVtbl 0x53D6F0) in the widget manager's list (gpl_widgetMgrSingleton 0x94D77C, list
// head at +0x14, nodes {window, next}), and gpl_windowOnEvent refuses keys for a window whose
// register (+4) has byte +0x18 clear, i.e. one not shown. So the question is answered by walking
// that list, which reads one rebased global and patches nothing.
//
// Focus is cleared on every switch of window (gpl_widgetMgrSetActiveWindow ->
// gpl_windowLoseFocus), on a click elsewhere (gpl_windowSetFocusChild) and by the in-car chat's
// Enter and Esc, so a shown window cannot hold a box that is still marked focused after it lost
// the keyboard. Not covered: the controls dialog's "press a key" capture is not an edit box.
//
// The walk runs on GPL's fg thread (the present hook's poll), which is where the widgets change.

namespace gplvr {

// Check the code and data the walk relies on and place the manager's global on this image. Logs
// what it found. Patches nothing; flat mode too.
void textEntryInstall();

// True once textEntryInstall has verified everything the walk reads.
bool textEntryReady();

// True while a shown window holds a focused, writable edit box -- GPL is taking typed text. False
// when the check is not ready. A walk that faults answers true and is counted.
bool gplTextEntryActive();

// For a hotkey's press edge: false, with a log line naming the key, while GPL is taking typed text.
bool hotkeyPressAllowed(int virtualKey, const char* name);

}  // namespace gplvr
