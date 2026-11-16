#pragma once

// THE AIMED GLASS SEEN THROUGH THE MODEL'S OWN GLASS (mirrorMode kMirrorPerGlass, Hooks.h mirrorExact).
//
// The exact camera shows the aimed glass's plane, which no longer sits in the cockpit's housing
// once the glass is aimed. So the model's own glass is drawn where it is, its outline the
// housing's whatever its shape, and each eye's texture coordinate at each of its
// points is the aimed glass's where that eye's ray through the point meets the aimed plane. That
// coordinate is a ratio of two linear functions of the point, so the car shader carries it as
// (uv q, q) and divides per fragment: exact for each eye, no clip, any outline.
//
// THE PANE'S FIELD IS FITTED PER EYE. The part of the aimed plane an eye sees through the model's
// glass -- its footprint, the outline (MirrorSurface::outline, or the derived rectangle) projected
// from the eye onto the plane -- is a box off the glass's centre and narrower than the glass when
// the aim turns it away (about 0.75-0.80 x 1.10 half sizes at a 21 degree aim). Each glass's pane
// in each eye's pass is aimed with its field on that box, plus a margin, along the exact camera's
// own left and up axes: half extents fovScale x the box's, the principal point on fovScale x the
// box's middle. The picture's texels go where the glass shows them instead of on a square that
// covers every corner from the head, which left about 35-40 % of the horizontal resolution unused.
//
// The texture coordinate stays affine on the aimed plane: with the aimed glass's own UVs
// a + B . (x - pivot) (the turned glass at scale 1), a fitted picture of middle m and half extents
// h along left L and up U puts the plane point x at image coordinate ((x - pivot).L - m_L) / h_L,
// so u(x) = a + (B.L) k_L ((x - pivot).L - m_L) + (B.U) k_U ((x - pivot).U - m_U), with
// k = the glass's half size / h; fovScale cancels. The shader takes the same four vectors per
// glass, composed for the picture the glass's region holds when the main pass draws it -- which is
// the one its pane last rendered there, or, after MirrorCache puts an eye's copy back, the one in
// that copy.

#include "MirrorShared.h"

namespace gplvr {

// Glasses the shader serves; a glass past these is drawn turned, as without the variant.
constexpr int kMaxWindowGlasses = 4;
constexpr int kWindowVectorsPerGlass = 4;

// aimPane, for every pane, glass `glass`'s in pass `pass` from `eye` (car axes): in the housing,
// `view`, made from `mirror` and `aim` (false `made` when it could not be), has its exact field
// refitted to that eye's footprint, and the field is recorded as the picture the glass's region
// will hold. Outside the housing, or for a view with no exact camera, the region's picture is
// recorded as unfitted, and the glass is not served until a fitted one renders.
void mirrorWindowFit(int glass, int pass, const MirrorGlass& mirror, const MirrorAim& aim,
                     const float eye[3], bool made, MirrorGlassView& view);

// MirrorCache: mirror.mip has been copied into eye `eye`'s copy, or that copy put back into it.
// The pictures recorded go with the texels.
void mirrorWindowPictureSaved(int eye);
void mirrorWindowPictureRestored(int eye);

// cockpitGlassTurn, for a housed glass: its centre in car axes, the same centre in its polygon's
// own frame (`pivot`), and the aimed glass's texture coordinate at scale 1 on the aimed plane as
// u = atPivot[0] + gradient[0] . (x - pivot), v likewise, with x in that frame.
void mirrorWindowGlassSet(int glass, const float centre[3], const float pivot[3],
                          const float gradient[2][3], const float atPivot[2]);

// The shader's u_window: kWindowVectorsPerGlass vectors per glass, glass-major -- pivot, the
// picture's plane normal, b0, b1, with u = b0.w + b0.xyz . (x - pivot) on that plane for the
// picture the glass's region holds now. Returns how many glasses have them; the rest are zero.
int mirrorWindowVectors(float out[kMaxWindowGlasses * kWindowVectorsPerGlass][4]);

}  // namespace gplvr
