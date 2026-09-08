#pragma once

// Stage 1 of the rasterizer shim: measure whether there is anything to batch.
//
// The frame profile put roglv2's submission at 5.0 ms of an 8.84 ms frame, ~630 ns for
// each of 8,615 polygons, because it issues one glDrawArrays per polygon. A shim that
// merged polygons into one draw would remove most of that -- but only if GPL submits them
// in long runs that share rasterizer state. If it interleaves textures polygon by polygon
// there is nothing to merge, and the whole idea dies here for the price of a few thunks.
//
// So this stage changes no behaviour at all. It thunks polyDraw and the state setters,
// forwards every call untouched, and reports how many polygons fall between consecutive
// state changes -- as a histogram, as a mean, and as the share of polygons living in runs
// long enough to be worth a batched draw. It also breaks the count down by WHICH setter
// ended each run, because "texture changes every polygon" and "the destination surface
// changes every polygon" would need different answers.
//
// The state setters are the four that can change what a draw means: mipSetCurrent (the
// texture), polySetDest (the destination surface -- the mirror pass switches it mid
// frame), and polyStartShadow/polyEndShadow. mipSetColor is thunked too, since a flat
// colour change would also break a merge.

#include <cstddef>

namespace gplvr {

struct Binding;

// Install the forwarding thunks. Verifies each global is populated first, logs what it
// took over, and does nothing unless `shimMeasure` is set.
void shimInstall(const Binding& binding);

// Close the frame's distinct-texture set. Called from the present hook.
void shimEndFrame();

// One line for the per-second report. Returns false when nothing was measured.
bool shimFormat(char* out, size_t size, double seconds);

}  // namespace gplvr
