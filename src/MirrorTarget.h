#pragma once

// The mirror pass, rendered into a framebuffer of its own.
//
// GPL renders the rear view through a camera on a bitmap instance at (0, 0) of the frame,
// then gpl_sceneRender copies that corner into mirror.mip before the main pass draws the
// car that samples it. In the frame's corner it leaves a square behind wherever the main
// pass does not paint over it -- in the left eye, when the right eye's pass renders a
// mirror of its own.
//
// Nothing in roglv2 depends on which framebuffer is bound, and nothing in the mirror path
// depends on its width: the ortho and the viewport span the display, the copy reads rows
// counted from the top of the display's height, and glCopyTexSubImage2D reads whatever is
// bound for reading. So a framebuffer as tall as the frame and as wide as the mirror,
// bound between the mirror camera's bind and the main camera's, receives the mirror pass
// unchanged and keeps it out of the frame.

namespace gplvr {

struct SceneParams;

// Called from the cameraBind hook AFTER the original bind, which has flushed the previous
// destination's polygons into whatever was bound before. Binds the mirror framebuffer when
// `camera` is the mirror camera, and puts the previous framebuffer back on the next bind of
// any other camera.
void mirrorTargetAfterBind(void* camera, const SceneParams* params);

// After gpl_sceneRender returns: puts the previous framebuffer back if a mirror pass was
// never followed by a main camera bind.
void mirrorTargetEndScene();

// THE RACE LOBBY'S TV FEED. gpl_simFrameRender makes the mirror instance at mirror.mip's size on
// the main camera's surface (report 109 section 1), and in the lobby that surface is the 373x280
// TV picture, so the instance is clipped to it and the copy-out fills only that block of
// mirror.mip; the rest keeps whatever the car or a replay last left there. In the front end, in
// VR, the block is stretched over the whole texture after the copy. The view needs no
// correction: GPL gives the mirror camera a 90 degree horizontal field and sets its aspect to
// width / (2 * height) of the instance (0x4C7D40), so the vertical field is the same at any
// instance size and the stretch shows GPL's view at a lower resolution.
//
// Whether a bitmapDraw reading `readSurface` (a handle) is that copy-out.
bool mirrorTargetLobbyCopy(int readSurface);

}  // namespace gplvr
