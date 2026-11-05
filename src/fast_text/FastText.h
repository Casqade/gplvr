#pragma once

// The fast text path, `fastText = 1` (docs/audit-2026-09-18/71-fast-text.md).
//
// GPL draws its HUD, its dash readouts and its messages one glyph at a time through the
// rasterizer's `stampDraw` entry point (roglv2 0x20004900). For a destination that is the GL
// framebuffer roglv2 blits each glyph with `glRasterPos2i` + `glDrawPixels` of an 8-bit
// colour-index image, after rebuilding the four 16-entry GL_PIXEL_MAP_I_TO_* tables from the
// stamp set's colour. On this driver that costs about 103 us a glyph: at 2560x1440 the 56
// stamps of a flat frame are 5.78 ms of an 8.83 ms frame, more than the scene, roglv2's flush
// and the present together (report 70d.5).
//
// This intercepts the same global GPL calls, reads the glyph out of roglv2's own font data,
// keeps it in a GPU atlas, and draws a run of glyphs as one batched quad list with our own
// program, inside roglv2's context and under roglv2's own 2D transform. The pixels are the
// ones glDrawPixels would have written: the atlas holds roglv2's colour indices, and the
// palette row is built with roglv2's own arithmetic. A stamp set with no palette of its own is
// drawn in the glyph bitmap's own colormap, which is what roglv2 does with it.
//
// What still goes to roglv2, counted and reported once a second:
//   - a destination that is not the GL framebuffer (a system-memory surface, or a GL texture
//     that roglv2 fills with glTexSubImage2D) -- we have no GL path into those;
//   - a glyph whose bitmap is not an 8-bit colour-index image;
//   - anything unreadable, and anything at all once the atlas is full.
//
// IN VR THIS IS ALSO WHERE GPL'S TEXT IS PLACED.
//
// GPL anchors its notices to the corners of its 2560x1440 layout -- "PRESS F1 FOR HELP" at
// (5, 143), "PRESS PAUSE TO RESUME" at (2, 1310) -- and in VR those coordinates land in the
// off-screen eye target unchanged, which is the left eye's far periphery at zero disparity. It
// cannot be moved by re-issuing the call: a destination handle says where IN a surface a glyph
// goes, not which framebuffer receives it, so an eye surface as the destination puts the pixels
// in a buffer nobody reads (measured 2026-10-07; Hud.cpp's destinationCanSplit is why that path
// declines). A quad has a position and a size of its own, so this path can put the same glyph
// anywhere in the target -- including once in each eye, at a convergent disparity.
//
// The unit that moves is a RUN, not the surface and not a glyph. Scaling the surface would drive
// a corner-anchored notice further out; scaling a run about its own centre and then moving that
// centre keeps the spacing GPL authored inside the line. vrGplTextScale and vrGplTextSpread are
// the two knobs.
//
// NEITHER KNOB AT ITS DEFAULT MAKES THIS A NO-OP. Placement alone is already the whole fix:
// the same glyph is drawn into BOTH eye rects at vrGplTextDepth's disparity, where roglv2 left one
// copy in the left eye's periphery at infinity. Scale 1 keeps the glyph the size GPL authored
// and inset 1.0 keeps its distance from the middle at the size GPL authored, so the defaults
// change neither -- but the eye it lands in, and the depth, do change.
//
// A RUN NEVER LANDS ON ANOTHER. The inset pulls each line's position toward the middle while
// the scale grows its glyphs, so two notices GPL drew a line apart -- "car must be stopped" over
// the pause prompt -- came out on top of each other. Once a frame the placed runs are stacked in
// GPL's own top-to-bottom order and any that would overlap is moved down until it clears.
//
// IN A REPLAY, the glyphs inside GPL's replay bands are not placed at all: they are drawn where
// GPL put them, into the replay panel's layer (ReplayPanel.h), which the headset shows on a quad
// of its own.
//
// IT TAKES ONLY stampDraw IN VR. The other six globals exist here to flush the pending quads
// before anything else draws, and in VR Hud.cpp holds all of them -- bitmapDraw carries the
// mirror panes' copy offset. Both modules re-assert over whatever a mode change left in a slot
// and adopt what they find as the original, so two owners of one slot make each the other's
// original and the chain calls itself. Hud.cpp calls fastTextFlushNow() from its own thunks
// instead, which is the same contract with one owner per slot.

namespace gplvr {

struct Binding;

// Resolve the seven 2D rasterizer globals and roglv2's own data, and say so in the log.
// From dllmain, after discovery.
void fastTextBind(const Binding& binding);

// Once a frame from the frame poll, on the render thread: re-asserts the thunks over whatever
// gpl_rasterResolveEntryPoints (0x4E21B0) put back at the last mode change, and polls the A/B
// key.
void fastTextPollFrame();

// From the present hook, before roglv2's screenDisplay: anything still batched is drawn, the
// frame is counted, and each run's extent becomes the placement the next frame's first glyph is
// put by. Every stamp of the frame has been issued by then.
//
// IN VR THE DRAW HAS ALREADY HAPPENED by the time this runs, and it has to: this call sits
// after the headset's copy of the frame and after the window blit, so a quad drawn here would
// reach neither. The present hook flushes through fastTextFlushNow() while the off-screen
// target is still bound, and this one then finds nothing left and does its bookkeeping.
void fastTextEndFrame();

// Draw anything still batched, now. From either side of gpl_sceneRender, so text issued before
// a world pass is on the framebuffer before that pass and text issued inside one is on it
// before whatever comes next; from the present hook before the off-screen target is unbound,
// which is what puts VR's text in the image the headset is handed; and from Hud.cpp's own 2D
// thunks in VR, where they stand in for the six globals this path gives up.
void fastTextFlushNow();

// Once a second from the frame report.
void fastTextReport();

// A rasterizer surface handle's place in its root -- the instance's absolute origin and size --
// and whether roglv2 draws into it through the GL framebuffer. Read out of roglv2's handle table
// under an exception guard, so a freed or bogus handle answers false rather than faulting.
bool fastTextSurfaceInfo(int handle, int& originX, int& originY, int& width, int& height,
                         bool& framebuffer);

// Open roglv2's 2D batch the way its own 2D entry points do: anything batched here is drawn,
// an open polygon batch is flushed, and the batch flag is set. A 2D call made right after this
// then flushes nothing of its own, so a framebuffer bound around that call receives that call's
// pixels and no one else's.
void fastTextOpenRasterBatch();

// True while the thunks are installed, and while the install is still retrying: PostProfile
// then leaves the globals alone and takes its per-entry times from here instead. It goes false
// again if the install gives up, and PostProfile's own poll picks the globals up next frame.
bool fastTextOwnsRasterEntries();

}  // namespace gplvr
