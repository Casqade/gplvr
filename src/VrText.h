#pragma once

// Text in the headset, without a text renderer -- and, since the first build, text that
// can be ROTATED.
//
// WHY THE CALLER DRAWS INTO A PANEL TEXTURE. wglUseFontBitmapsA builds display lists of
// glBitmap calls, and glBitmap is screen-axis-aligned by construction: a raster position
// places the bitmap and nothing transforms it. Drawing the menu's strings straight into
// the eye buffers therefore left every glyph level with the display while the panel quad
// under them rolled with the head -- "the text rotates with my head" -- and no care at
// the call site can fix it, because the rotation the text needs is not a thing glBitmap
// has.
//
// The menu is now an OpenXR quad composition layer: the strings are drawn flat into that
// layer's own texture and the runtime rotates the whole quad. Nothing here has to turn,
// and the layout being in the texture's pixels also means how big the text looks depends
// on the panel's size in metres and not on the headset's resolution -- the first build
// sized the font from the projected pixel height and a tweak strip came out three times
// too wide for itself.
//
// WHY STILL wglUseFontBitmapsA. The alternatives were weighed in the first build and
// nothing has changed: GPL's own drawDebugText costs ~103 us a glyph (report 71); the
// fast-text atlas accelerates roglv2's OWN stamped glyphs and has no source for glyphs
// GPL never stamps; pribluda's lists belong to a DLL that may not be installed. One WGL
// call, GDI does the rasterising, and no glyph code is ours.

#include "GlInterop.h"

namespace gplvr {

// Two sizes live at once, because a menu wants a body size and a smaller line of help and
// rebuilding the lists between them every frame would cost two GDI font builds a frame.
enum VrTextSlot {
    kVrTextBody = 0,
    kVrTextSmall,
    kVrTextSlots,
};

// Build the lists for `pixelHeight` in that slot, or keep the ones already built for it.
// False means there is no text, and the reason has been logged once.
bool vrTextEnsure(const GlFunctions& gl, int slot, int pixelHeight);

// Drop the lists, the canvas texture and its framebuffer. `contextAlive` false when the
// context they belong to has gone: they are then forgotten rather than deleted, because
// deleting a name in a dead or replaced context either does nothing or destroys somebody
// else's object.
void vrTextRelease(const GlFunctions& gl, bool contextAlive);

// The built font's metrics for that slot, in pixels. Zero until vrTextEnsure succeeded.
int vrTextHeight(int slot);
int vrTextAscent(int slot);
int vrTextWidth(int slot, const char* text);

// Draw at a baseline-left position, in the current colour and whatever pixel-space ortho
// is in force -- which, between vrTextCanvasBegin and vrTextCanvasEnd, is the canvas's.
void vrTextDraw(const GlFunctions& gl, int slot, float x, float baselineY,
                const char* text);

}  // namespace gplvr
