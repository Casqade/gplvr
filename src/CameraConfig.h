#pragma once

#include <cstddef>

namespace gplvr {

// Per-car COCKPIT CAMERA POSITION, VR only: where the driver's eyes sit in the cockpit, moved
// from where GPL puts them by an offset in the car's own axes.
//
// Keyed exactly as the mirror presets are -- `<cardir>/<teamfolder>`, lower case, through
// mirrorConfigCarKey -- so a car is the same section name in both files. gplvr_camera.ini in
// gplvr's folder holds one section per car, three values in metres:
//
//     [cars67/lotus]
//     right=0.000
//     up=-0.030
//     forward=0.050
//
// A car with no section sits where GPL puts it: 0, 0, 0. Everything here runs on the render
// thread: stereoRender reads and resolves, the VR menu's camera mode edits.
//
// The offset is applied only while the view is a cockpit camera (GPL's camera modes 1..3). A TV
// or chase camera keeps GPL's own eye.

// The three directions, in the order the camera mode's trigger cycles them.
enum CameraDirection {
    kCameraForward = 0,  // Z: toward the car's nose
    kCameraUp,           // Y: toward the car's roof
    kCameraRight,        // X: toward the car's right
    kCameraDirections
};

// Called at the end of loadOptions: forget the car, so the next frame reads its section again.
void cameraConfigReset();

// The viewed car's team, as gpl_driverGetTeamIndex returned it, from stereoRender on a cockpit
// frame; negative when it could not be read, with the reason. Cheap when nothing changed: the
// file is read only when the team or the car set does. An unsaved edit is DISCARDED when the
// viewed car changes -- nothing ever writes a car that is not in view.
void cameraConfigViewedTeam(int team, const char* reason);

// The live offset in GPL's local axes -- X forward, Y left, Z up -- in metres. False when it is
// zero, so the caller can skip the arithmetic.
bool cameraConfigOffset(float local[3]);

// One direction of the live offset, in metres.
float cameraConfigValue(int direction);
// Set it, clamped to +/- kCameraMaxOffset. True when it changed, which marks the edit.
bool  cameraConfigSetValue(int direction, float metres);
constexpr float kCameraMaxOffset = 0.5f;

// WHICH DIRECTION THE CAMERA MODE IS ADJUSTING. Only one at a time.
int         cameraConfigSelected();
bool        cameraConfigSelect(int direction);       // true when the selection changed
int         cameraConfigSelectStep(int step);        // round the three, wrapping
const char* cameraConfigDirectionName(int direction);  // "X (left/right)", ...

// Bumped whenever the live offset is replaced wholesale -- the viewed car or the car set changed.
// The VR menu's save/discard baseline compares it to know its copy is of another car.
unsigned cameraConfigGeneration();

// The current key, or nullptr while no car has been identified.
const char* cameraConfigKey();

// Write the live offset to the current key's section if it was edited, creating the file and the
// section as needed. Only an explicit save writes; the outcome is logged.
void cameraConfigSave();

// Put the live offset back to what the file holds for the current car and forget the edit.
void cameraConfigDiscard(const char* why);

// Remove the loaded car set's sections from gplvr_camera.ini, so that car set's cars sit where GPL
// puts them again; other car sets' presets stay. The car in view is read again on the next frame.
// carSet receives the car set's name in lower case, cleared the number of sections removed. False
// when no car set is loaded or a section could not be removed. For the menu's Reset dialog.
bool cameraConfigClearCarSet(char* carSet, size_t carSetSize, int& cleared);

}  // namespace gplvr
