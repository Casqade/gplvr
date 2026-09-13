#pragma once

// Make GPL's billboards face the eye instead of the view plane.
//
// GPL's billboards are scene-graph node type 3 (table at 0x570330, slot 3), and the
// rotation is not a matrix -- which is why two sweeps for one found nothing. The
// vertical-axis variant, sub_4D6490, builds its horizontal axis as
//
//     normalize(cross((0,0,1), modelviewRow0))
//
// where row 0 of the modelview is the CAMERA FORWARD direction in the object's local
// frame. That makes the quad view-plane-aligned: its normal is the view direction, the
// same for every sprite on screen. The (0,0,1) is hardcoded, so the quad never tilts; it
// only swings about world up.
//
// On a monitor at 40-50 degrees of field, "face the view direction" and "face the
// viewer" differ by a few degrees and nobody notices. At 100+ degrees per eye they do
// not: a billboard at the edge of vision is mis-yawed by up to about 50 degrees, so it
// foreshortens and turns its face away, and rotating your head re-aims every billboard
// in the scene at once. Confirmed in game -- a tree seen out of the corner of the eye is
// visibly narrower and turned away, and swings back as you look at it, with car, tree and
// head-position all stationary.
//
// The fix substitutes the horizontal vector from the sprite to the eye for that camera
// row. Downstream takes cross((0,0,1), v), which reads only v.x and v.y, so nothing else
// in the quad construction changes -- and the sign is (anchor - eye) because the stock
// value points from the eye toward the sprite, which is what keeps the winding and the
// UVs the right way round.
//
// The fully screen-aligned variant, sub_4D69F0, spans its quad along modelview rows 1 and
// 2 (camera left and up), so in the headset it turns and ROLLS with the head. That is the
// pit board: pitboard.3do is a single type-3 node on pitboard.srb, whose SRH2 chunk sets
// the sprite's +32 byte that selects this variant; the board's position is car-fixed
// (gpl_pitBoardTrackCar_perTick), only its orientation followed the head. Both row reads
// are redirected to a basis facing the eye position with its up in the vertical plane.
// The particle billboards in gpl_particleRender read the same accessors through their own
// calls and are left alone: rotationally symmetric puffs, where screen-aligned is not wrong.
//
// Unconditional. "Face the viewer" is the more correct construction at every field of
// view, and the only place the difference is large enough to see is the one gplvr
// exists for; the `billboardFaceEye` key that used to gate it was an A/B against stock
// that has been run.

namespace gplvr {

struct Binding;

// Redirect the camera-row read inside sub_4D6490. Verifies the stock call before writing
// and logs what it did; safe to call once, after the image is bound.
void billboardInstall(const Binding& binding);

}  // namespace gplvr
