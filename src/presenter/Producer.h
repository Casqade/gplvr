#pragma once

// The game's half of the VR channel.
//
// Creates the shared control block, launches gplvr-xr.exe as the presenter, waits to be
// told which adapter the OpenXR runtime wants, then hands it one shared D3D11 texture
// per frame containing both eyes.
//
// It runs entirely from the frame hook on the game's render thread, and it is a state
// machine rather than a blocking sequence: the presenter takes a second or two to bring
// SteamVR up, and the game must not stop while that happens. Every step that can fail
// logs once and drops the whole thing back to inactive, because a mod that stalls the
// game waiting for a headset is worse than one that quietly does not start.

#include "presenter/SharedPresentation.h"

namespace gplvr {

// Called once, after discovery. False means VR is off or the presenter could not be
// launched; the game carries on as a flat stereo mod.
bool presenterStart();

// Called from the frame hook, before the game presents, so the back buffer still holds
// the finished frame. Drives the handshake, then copies and publishes.
void presenterFrame(bool sideBySide, bool stretchToFill);

void presenterStop();

// The most recent eye views the presenter published, or false if none yet. This is what
// Phase 3 will feed into the scene parameter block; nothing consumes it today beyond the
// log line that proves the return path works.
bool presenterLatestViews(SharedView views[2], SharedPose& head);

}  // namespace gplvr
