#pragma once

// Whether there is a headset, answered before anything is installed for one.
//
// XrRuntimeProbe answers "is a runtime registered"; a runtime with no headset behind it --
// Windows Mixed Reality left active on a Quest machine, SteamVR with the headset off -- passes
// that and only fails inside the presenter. VR and flat install different hooks, once, so the
// presenter's answer has to be in before the install: the presenter is launched as soon as the
// install thread starts, and the game's own thread waits for its answer when it loads the
// rasterizer, which is where everything gplvr installs starts to matter.
//
// No headset means flat, exactly as with vr = 0: the options are read again with vr off, so
// every value derived from vr takes its flat answer before any installer reads it.

namespace gplvr {

// The install thread, first thing. Launches the presenter when vr = 1; nothing otherwise.
void headsetCheckStart();

// Blocks until the presenter has answered, then acts on it, once. `where` names the caller for
// the log. Returns at once when vr is off or the answer is already in.
void headsetCheckSettle(const char* where);

// True once the presenter has found no headset; loadOptions then plays flat.
bool headsetCheckRefused();

}  // namespace gplvr
