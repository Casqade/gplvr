#pragma once

// THE VIEWED CAR IN ITS OWN MIRRORS (mirrorMode kMirrorPerGlass with mirrorPlayerCar). GPL never
// draws it there:
// gpl_sceneRender queues every other car into the render tree before the mirror walk (0x487733),
// and the viewed car, its render record p[86], only after it (0x487DFF), for the main walk. Its
// record's word +48 (set at 0x48B0C5 for the cockpit view) makes gpl_carRecordDraw draw the
// `cockpit` model; clear, it draws the `car` model, the body seen from outside.
//
// So, while a mirror pane of this mode is aimed:
// - the record is queued right after GPL's loop of the other cars (the gpl_matrixPop at 0x487748),
//   with the main eye's view in force and the arguments GPL's own call at 0x487DFF takes, which
//   then skips. The tree's key for it is the main eye's distance, as GPL's queueing makes it.
// - around the mirror walk (BakedMirror.cpp) word +48 is cleared, so the car hook (CarWalk.cpp)
//   lists the outside model for CarRenderer, and set back after. A walk the GPL VR renderer does
//   not draw skips the record instead: GPL would draw the cockpit from behind the glass.
// - CarRenderer cuts every car of the pass at the glass's plane. The pane's camera is the eye
//   reflected through the glass, out beyond it, and between the two sit the car's own mirror
//   housing and bodywork, which face it.
// - the car's own mirrors are left out of its list (CarDrawList::hideMirrorBody): each is an
//   object of its own binding mirror.mip (CarBake.cpp markMirrorBody), and a housing's rim
//   reaches past the glass's plane, so the cut alone left it in the picture.
// - the car can be moved along its forward axis per glass (MirrorAim::carOffset, mirror setup's
//   car page), and left out of a glass altogether (MirrorAim::carReflection).
//
// Every patch is checked against GPL's bytes first; with any refused the mode draws no car.

namespace gplvr {

// From bakedMirrorInstall, once the mirror walk is redirected: the two call redirects. Logs what
// it did and why not.
void mirrorOwnCarInstall();

// Stereo.cpp aimPane: this pane's glass plane in world axes, {normal toward the eye, the pane
// camera's signed distance from the plane}, and the move of the viewed car in this glass (world
// metres, MirrorAim::carOffset along the car's forward axis), valid for the sceneRender that
// follows. Cleared before each aim and after each eye's render. A glass whose carReflection is off
// is never set, so its pane draws no car of its own.
void mirrorOwnCarPaneSet(const float plane[4], const float shift[3]);
void mirrorOwnCarPaneClear();

// The plane cars are cut at in this mirror pass; false when there is none. Keep where
// dot(position - camera, plane.xyz) + plane.w >= 0.
bool mirrorOwnCarClip(float plane[4]);

// The move of the viewed car in this mirror pass (ViewInputs::carShift); zero when there is none.
// A car whose mirror housing is part of its body (the Ferrari's are in ferrari.3do, with no mirror
// object to leave out), or whose outside model's housing sits ahead of the cockpit's glass, is
// moved back until the housing is behind the glass's plane and the cut takes it.
void mirrorOwnCarShift(float shift[3]);

// BakedMirror.cpp around the mirror walk. `drawn`: the walk is a baked pass whose cars CarRenderer
// draws.
void mirrorOwnCarWalkBegin(bool drawn);
void mirrorOwnCarWalkEnd();

// The car hook: whether GPL's draw of `record` is skipped in this walk.
bool mirrorOwnCarSkips(const void* record);

// The car hook: whether `record` is the viewed car drawn as an outside car in this walk, whose
// list leaves out its own mirror objects (CarDrawList::hideMirrorBody).
bool mirrorOwnCarHidesBody(const void* record);

// THE PLACEMENT CHECK, every per-glass mirrorMode. The glasses are placed in a car frame taken
// from GPL's mirror camera (Stereo.cpp PaneFrame: its eye less the 0.35 m mount); the cockpit is
// drawn wherever GPL's pose of the car puts it. aimPane hands over the frame (cleared with the
// pane); the car hook hands over the pose of the cockpit in a main pass, and of the viewed car's
// outside model in its own mirror passes (row-major 3x4, local to world). Every 5 s the log says
// how far each sat from the frame, in car axes: 0 is a glass where GPL draws it.
void mirrorOwnCarFrameSet(const float origin[3], const float R[9]);
void mirrorOwnCarPlaced(bool cockpit, const float pose[12]);

}  // namespace gplvr
