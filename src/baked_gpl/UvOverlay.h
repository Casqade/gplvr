#pragma once

// The UV checker overlay (docs/audit-2026-09-18/105-baked-renderer-rewrite.md section 4.7): with
// bakedDebugKey's view 8 every texture GPL draws through roglv2 is replaced by the baked
// renderer's checker texture (Renderer::uvCheckerTexture), as the baked renderer's own shaders
// replace theirs, so the two halves of a capture show the same grid on both renderers and a
// difference in how a polygon's UVs are laid out or triangulated shows as a different grid.
//
// roglv2 draws every polygon with glDrawArrays on texture unit 0 (rogl_binDraw4_texturedOpaque
// 0x20011780 and its siblings). Its import of glDrawArrays is redirected: while the overlay is on
// and texturing is enabled, the checker is bound around the draw and roglv2's own texture put
// back after it, so roglv2's cache of the bound texture stays true and no upload is touched.

#include <cstdint>

namespace gplvr {

// Redirects roglv2's glDrawArrays import once. Logs what it did or why not.
void uvOverlayInstall();

// The checker texture (0 while the baked renderer is not up) and whether the overlay is on.
void uvOverlaySet(unsigned checkerTexture, bool on);

// One line for the once-a-second report; false when the overlay was off all second.
bool uvOverlayFormat(char* out, unsigned size);

}  // namespace gplvr
