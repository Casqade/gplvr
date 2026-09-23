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
// palette row is built with roglv2's own arithmetic.
//
// What still goes to roglv2, counted and reported once a second:
//   - a destination that is not the GL framebuffer (a system-memory surface, or a GL texture
//     that roglv2 fills with glTexSubImage2D) -- we have no GL path into those;
//   - a stamp set with no palette of its own, whose glyph bitmap carries a fixed palette;
//   - a glyph whose bitmap is not an 8-bit colour-index image;
//   - anything unreadable, and anything at all once the atlas is full.
//
// Flat mode only. In VR the same globals belong to Hud.cpp, which re-issues every stamp once
// per eye; sharing them is the next step and is written up in the report.

namespace gplvr {

struct Binding;

// Resolve the seven 2D rasterizer globals and roglv2's own data, and say so in the log.
// From dllmain, after discovery.
void fastTextBind(const Binding& binding);

// Once a frame from the frame poll, on the render thread: re-asserts the thunks over whatever
// gpl_rasterResolveEntryPoints (0x4E21B0) put back at the last mode change, and polls the A/B
// key.
void fastTextPollFrame();

// From the present hook, before roglv2's screenDisplay: anything still batched is drawn. Every
// stamp of the frame has been issued by then.
void fastTextEndFrame();

// Draw anything still batched, now. From either side of gpl_sceneRender, so text issued before
// a world pass is on the framebuffer before that pass and text issued inside one is on it
// before whatever comes next.
void fastTextFlushNow();

// Once a second from the frame report.
void fastTextReport();

// True while the thunks are installed, and while the install is still retrying: PostProfile
// then leaves the globals alone and takes its per-entry times from here instead. It goes false
// again if the install gives up, and PostProfile's own poll picks the globals up next frame.
bool fastTextOwnsRasterEntries();

}  // namespace gplvr
