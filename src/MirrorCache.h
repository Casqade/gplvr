#pragma once

// A copy of mirror.mip per eye, so each eye keeps its own reflection on the frames where the
// mirror is not rendered at all (`mirrorUpdateRate`).
//
// THERE IS EXACTLY ONE mirror.mip -- one texture for the whole carset (report 37 section 1.4)
// -- and both glasses live in it as its two halves. With `perEyeMirror` on, each eye's pass
// renders both glasses from its own viewpoint into that one texture and then draws the car
// that samples it, so the image the car gets is always the pass's own. What survives to the
// end of the frame is whichever pass ran last, which is the right eye's.
//
// That is why skipping the render flickered. A frame that renders nothing leaves both eyes
// sampling the right eye's reflection, and the left eye then alternates between its own and
// its neighbour's at the skip cadence -- a stereo mismatch, which the eye finds far more
// readily than a stale image. Saving each pass's image and putting it back on the frames that
// do not render removes the alternation; what is left is a reflection up to N-1 frames old in
// BOTH eyes at once, which reads as mirror lag.
//
// The copies are GL's own. roglv2's copy-out is framebuffer-to-texture -- glCopyTexSubImage2D,
// report 37 section 1.3 -- and it has no texture-to-texture form to borrow, so this blits
// between framebuffers of its own with the mirror texture and the cache attached to them. The
// texture name comes out of the bitmap roglv2 hands `bitmapDraw`, and the size GL reports for
// it is checked against the bitmap's before it is written through.

namespace gplvr {

// mirror.mip's level-0 bitmap, as seen going past as the destination of the copy-out, with the
// size the rasterizer reports for it.
//
// A rasterizer HANDLE -- a small integer, not a pointer. Everything roglv2's C API passes
// around is one, and it is resolved through the handle table by `rasterBitmapGlTexture`. This
// call stores it and nothing else; the GL work happens in the calls below.
void mirrorCacheNoteTexture(int bitmapHandle, int width, int height);

// True when both eyes have an image saved, which is the condition for skipping a render. False
// until the first frame that renders the mirror has gone through, and after anything that
// invalidates the textures -- so a caller that consults this before deciding to skip cannot
// skip the frame that would have filled the cache.
bool mirrorCacheReady();

// Save mirror.mip into this eye's copy. Call AFTER the eye's own render, which is where the
// second glass is copied in -- before it, the texture holds only one of the two halves.
void mirrorCacheSave(int eye);

// Put this eye's copy back into mirror.mip. Call BEFORE the eye's render, and only on a frame
// that is not rendering the mirror. False when there is nothing saved for that eye yet.
bool mirrorCacheRestore(int eye);

// Fill mirror.mip with black, and forget both saved copies.
//
// For `mirrorUpdateRate = 0`. Nothing renders the mirror then, but the CAR STILL SAMPLES THE
// TEXTURE, so the glasses keep showing whatever reflection was last written -- a frozen image
// that looks like a fault rather than an off switch. This is called when the setting is turned
// off, from wherever holds the GL context.
bool mirrorCacheBlackout();

// How many restores happened since the last call, for the once-a-second line.
unsigned mirrorCacheTakeRestores();

// Forget the texture and the saved images, without touching GL. Called on a mode change, where
// the bitmap this was told about may already be freed and the context may not be ours -- the
// two cache textures are kept and reused, since their size follows mirror.mip and that is one
// file per carset.
void mirrorCacheRelease();

}  // namespace gplvr
