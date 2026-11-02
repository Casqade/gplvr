#pragma once

// What GPL still draws in a baked main pass -- cars, cockpit, marshals, the pit-board man,
// debris, particles, lit polygons -- tested against the baked depth
// (docs/audit-2026-09-18/67-baked-objects.md section 67c; milestone M1b of report 57).
//
// roglv2 keeps GPL's painter's order in its own depth buffer: each polygon gets a synthesised
// z from its submission number, the buffer is cleared at the start of every flush, and the
// polygon is drawn from client arrays with glDrawArrays (report 60, render-shim-scope.md). Its
// vertex arrays still carry the real distance: the vertex converters write w (the view-space
// forward distance, GPL's raster vertex +12 inverted) into the fog-coordinate array at drawer
// +220 for every polygon mode. So the flush that paints GPL's objects over the composited baked
// image runs under a small program of ours: the same fixed-function result (texture MODULATE
// colour, colour sum, alpha > 0), roglv2's own synthesised depth untouched, and one extra
// discard -- a fragment whose w lies behind the baked surface at that pixel. The baked depth is
// `near / w` (reversed-Z) and GPL's w is the same forward distance from the same camera and near
// plane, so the comparison needs no other mapping.
//
// Blob shadows are the exception (section 4.8 of report 105): roglv2's depth for a polygon is its
// submission number, so the flush can tell the polygons of each shadow call apart and test them
// by GPL's walk order against the baked solid order instead.
//
// Hooks, all in roglv2 and all runtime: its imports of glClear (a depth clear opens a flush),
// glDisable (GL_DEPTH_TEST closes it), glEnable/glDisable (texturing and colour sum state for
// the program), and slot 17 of the batch drawer's vtable (0x20011470, which sets the vertex
// arrays at the start of every flush) to bind the program with the drawer's fog array.

#include <windows.h>

namespace gplvr {

// One import of a module to redirect: its name, the hook, and where the slot's previous value
// goes (another hook of gplvr's may sit there; the hook forwards to it).
struct ImportRedirect {
    const char* name;
    void*       hook;
    void**      original;
};

// Redirects `module`'s imports by name; the number of slots that hold their hook afterwards.
int redirectImports(HMODULE module, const ImportRedirect* hooks, int count);

// Installs the hooks once. Logs what it did; refuses when any hooked byte is not stock.
void residualInstall();

// The composited pass just drawn: its depth texture, its rect in the bound framebuffer's
// window pixels, the near plane and the depth convention. The next roglv2 flush -- the one that
// paints what GPL submitted during this pass -- is tested against it. `orderTexture` is the
// pass's solid order (Renderer::orderTexture): GPL's blob shadows are tested by GPL's walk order
// at their car's place against it (report 105 section 4.8), and by depth with the margin, as
// everything else, without it.
struct ResidualPass {
    unsigned depthTexture = 0;
    unsigned orderTexture = 0;
    int      rect[4] = {0, 0, 0, 0};
    float    nearPlane = 0.125f;
    bool     reversedZ = true;
};
void residualArm(const ResidualPass& pass);

// Drops an arm no flush has taken -- after a pass whose walk left roglv2 nothing to paint, a
// glClear-bracketed flush may not come -- so a later flush of another pass is not tested against
// this pass's depth and rectangle. True when there was one.
bool residualDisarm();

// Brackets the baked main walk, so a flush inside it is counted: whatever roglv2 paints then
// lands under the baked image, which the composite draws over it.
void residualWalkBegin();
void residualWalkEnd();

// Around one call of GPL's that draws a car's blob shadow, inside the baked walk: the polygons it
// submits to roglv2 are recorded by their submission numbers, with `slot` -- where GPL's walk was
// at the call (Renderer::visible's size) -- and the flush tests them by that place instead of by
// depth. Outside a baked walk both do nothing.
void residualShadowBegin();
void residualShadowEnd(unsigned slot);

}  // namespace gplvr
