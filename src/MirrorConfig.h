#pragma once

#include <cstddef>

namespace gplvr {

// Per-car, PER-GLASS mirror aim (docs/audit-2026-09-14/48-per-car-mirror-hud.md sections 2, 4, 5).
//
// The car is keyed `<cardir>/<teamfolder>`, lower-case, from gpl_carsetDirName (0x5A1928) and
// mod_teamFolderNamePtrs (0xA0F798), e.g. `cars67/lotus`. gplvr_mirrors.ini in gplvr's folder holds
// one section per key, and in it three values per glass, named after the glass:
//
//     [cars18/c07]
//     leftYaw=12.00
//     leftPitch=-3.00
//     leftFovScale=1.000
//     rightYaw=-12.00
//     ...
//     centreFovScale=1.250
//
// A glass with no values gets the aim GPL's own geometry gives it: 0, 0, 1.0. Everything here runs
// on the fg render thread: the key poll and stereoRender.
//
// ONE AIM PER GLASS, because one per car could not express what a car's mirrors need. It was a
// single yaw applied to both wings with the sign flipped per side, so left and right could not
// differ, and a glass on the centreline -- the 330P4 has one -- had no side to flip and could not
// be aimed at all.
//
// YAW IS THE SAME WAY ROUND ON EVERY GLASS: positive turns that glass's view toward the car's
// right, so "right" on the stick shows more of what is to the right whichever glass is selected.
// A symmetric pair therefore stores opposite signs, and "all glasses" in tweak mode moves them
// that way -- both inboard or both outboard together -- which is what the single car-wide yaw
// always did.
struct MirrorAim {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovScale = 1.0f;
};

// The longest glass name, terminator included: "centre" plus a digit for a car with two of them.
constexpr int kMirrorGlassNameBytes = 12;

// As many glasses as there can be panes -- kMaxMirrorSurfaces, which Stereo checks.
constexpr int kMaxMirrorGlasses = 8;

// Called at the end of loadOptions: every car starts neutral.
void mirrorConfigReset();

// Delete gplvr_mirrors.ini and forget every cached car, so every car is back to neutral and
// can be aimed again. For the menu's Reset dialog.
bool mirrorConfigClearAll();

// Once a frame from stereoRender's mirror path. True when the car-set directory string has
// changed since the last key was built, which drops every cached car and asks the caller to
// resolve the viewed team again.
bool mirrorConfigCarSetChanged();

// The viewed car's team index changed, as gpl_driverGetTeamIndex returned it; negative when the
// team could not be read, which leaves the glasses neutral. `reason` says why in that case.
// Not called when there is no viewed car, so the last car's aim stays. Reads gplvr_mirrors.ini at
// most once per team per car set; logs `mirror config: ...` when the key changes. An unsaved
// tweak is DISCARDED when the viewed car changes -- nothing ever writes a car that is not in view.
void mirrorConfigViewedTeam(int team, const char* reason);

// Bumped every time the live aims are replaced wholesale -- the viewed car changed, the car set
// changed, the presets were cleared -- and when the glasses they are indexed by change. A caller
// holding a copy indexed by glass -- the VR menu's save/discard baseline -- compares this to
// know its copy is of a car, or a set of glasses, no longer in view.
unsigned mirrorConfigGeneration();

// THE GLASSES THE PANES ARE DRAWN FOR, in pane order, each with the side it is on: +1 left,
// -1 right, 0 on the centreline. Stereo calls this whenever it lays the panes out. The names
// are what the ini is keyed on, so the same glass finds its aim whether the car has a pane per
// glass or the two shared halves.
void mirrorConfigSetGlasses(const char* const names[], const int sides[], int count);
int  mirrorConfigGlassCount();
const char* mirrorConfigGlassName(int glass);  // never null
int  mirrorConfigGlassSide(int glass);

// The aim aimPane uses for one pane's glass. Neutral for a glass with none.
MirrorAim mirrorAimFor(int glass);

// The live aim of one glass, for tweak mode to write. Made on first use, so a glass nobody has
// aimed is not written to the file as a row of zeroes. Null when the glass does not exist.
MirrorAim* mirrorAimSlot(int glass);

// WHICH GLASS TWEAK MODE IS ADJUSTING: -1 for all of them together, else a pane index. Back to
// all whenever the glasses change.
int         mirrorConfigSelected();
bool        mirrorConfigSelect(int glass);  // true when the selection changed
int         mirrorConfigSelectStep(int direction);  // through all, 0, 1, ... and round again
const char* mirrorConfigSelectedName();  // "all glasses" or the glass's name

// Every glass's aim on one line, for the log.
void mirrorConfigFormatAims(char* out, size_t size);

// The current key, or nullptr while no car has been identified.
const char* mirrorConfigKey();

// True when the current key has its own section in gplvr_mirrors.ini.
bool mirrorConfigPerCar();

// Tweak mode changed an aim.
void mirrorConfigMarkEdited();

// Write the live aims to the current key's section if tweak mode changed them, creating the
// file and the section as needed. Writes the car in view and nothing else. Logs the outcome.
//
// ONLY AN EXPLICIT SAVE WRITES. Every other way out of tweak mode -- leaving the race, changing
// car, changing car set, switching to the HUD tweak, quitting -- discards, because a save button
// that is also what happens when you walk away is not a save button.
void mirrorConfigSave();

// Put the live aims back to what the file holds for the current car and forget the edit. Logs
// the values dropped, so an aim that vanishes is never silent.
void mirrorConfigDiscard(const char* why);

}  // namespace gplvr
