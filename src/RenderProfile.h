#pragma once

// Where a frame actually goes: the two measurements that open the boxes `profileFrame`
// only weighs.
//
// `profileFrame` reports frame = scene + present + gplvr + other. Both interesting numbers
// are inside terms it cannot see into. `scene` is GPL walking the track ring, transforming,
// clipping and emitting; `present` is roglv2 sorting, filling vertex arrays and issuing one
// glDrawArrays per polygon, plus whatever the GPU still owed. Nothing so far distinguishes
// CPU submission from GPU drain, and the whole renderer question turns on that split.
//
// Two stages, and they cost almost nothing:
//
//  GEOMETRY -- GPL already counts its own frame. A reset function zeroes five counters at
//  the top of sceneRender and a debug overlay displays them at the bottom; the counters run
//  whether or not the overlay is enabled. Reading them costs six loads per render at
//  exactly the boundary the sceneRender hook already wraps. They give polygons emitted,
//  vertex references, vertices ACTUALLY transformed (the memoisation hit rate), transform
//  nodes entered, and -- against roglv2's own counter -- how many polygons the rasterizer
//  threw away.
//
//  PRESENT -- roglv2 reaches the driver through its import table, so three IAT slots split
//  `present` without touching roglv2's code. `glDrawArrays` is CPU time inside the driver,
//  which is what draw-call merging would remove. `glFinish` is how far behind the GPU was:
//  large means the GPU is the bottleneck and no submission change helps, near zero means
//  every millisecond of `present` is CPU. roglv2 issues that glFinish itself, once per
//  present, so timing it is free and does not perturb the pipeline -- unlike
//  `profileGpuWait`, which inserts a stall of its own and changes what it measures.
//
//  PRE -- the largest span in the frame, and until now the one nobody could account for.
//  Five markers on call sites in GPL's own foreground loop split it into the two waits at
//  the top of the loop, the simulation-state latch, the input and event work, the window
//  redraw, and any earlier eye renders. The first of those waits is a `Sleep(1)` poll for
//  the next simulation tick, which is why the frame rate equals the task rate exactly with
//  vsync off -- so the answer to "why is `pre` 4.5 ms" is expected to be "because the game
//  is asleep", and the point of the split is to prove that rather than assume it.
//
// And one deliberately destructive switch, `nullDraw`, which drops every polygon at the
// rasterizer boundary by forcing polyDraw's vertex count to zero. The picture goes black
// and the frame time that remains is the floor no shim, no merge and no GPU rasterizer can
// ever go below. One lap, one number, and it bounds every renderer optimisation at once.

#include <cstddef>

namespace gplvr {

struct Binding;

// Locate GPL's render counters and roglv2's polygon counter. Logs what it found. Failing
// is not fatal: the geometry line simply does not appear.
void renderProfileDiscover(const Binding& binding);

// Take over roglv2's glDrawArrays / glFinish / SwapBuffers imports. Safe to call more than
// once; it notices slots that are already ours.
void renderProfileInstallDriverThunks();

// Force polyDraw's vertex count to zero. Refuses when the measurement shim already owns
// polyDraw, because both want the same function pointer. Installed whenever `nullDraw` or
// `nullDrawKey` is set, so the key can turn it on later.
void renderProfileInstallNullDraw(const Binding& binding);

// Toggle it. The worst frame on a track has to be driven to, and it cannot be driven to
// blind, so this is the only way to measure one.
void renderProfileSetNullDraw(bool active);

// Latch GPL's counters. Called from the sceneRender hook once the original has returned,
// which is where they hold the render that just finished.
void renderProfileAfterRender();

// Bracket the engine's own screenDisplay, so a glFinish can be charged to the half of the
// frame it happened in. roglv2 issues them from more than one place.
void renderProfileSetInPresent(bool inPresent);

// roglv2's glDrawArrays time so far in this window, in rdtsc cycles, for the frame budget's
// per-span attribution. Zero unless profileStages installed the thunk; reset by
// renderProfileEndWindow.
unsigned long long renderProfileDrawCycles();

// The frame, split into four spans that TILE it, from hooks that already existed:
//
//   pre      present returns -> sceneRender entry   the game's own loop: sim, input, wait
//   scene    sceneRender entry -> return            the world, both passes, both eyes
//   post     sceneRender returns -> present entry   the HUD and 2D overlays
//   present  present entry -> return                roglv2's flush and SwapBuffers
//
// This replaced `other`, which was a SUBTRACTION -- the largest term in the frame was the
// one nobody could account for, and both times it got guessed at, the guess was wrong.
// Because the spans tile, they sum to the total and the largest one names the culprit.
//
// Both note functions take raw QueryPerformanceCounter values and write nothing.
void renderProfileNoteScene(long long begin, long long end);
void renderProfileNoteFrame(long long presentBegin, long long presentEnd);

struct FrameSpans {
    double pre = 0.0;
    double scene = 0.0;
    double post = 0.0;
    double present = 0.0;
    double total = 0.0;
};

// The window's mean spans, and the single WORST frame in it broken down the same way.
//
// The worst frame gets its own breakdown because an average over 130 healthy frames and
// one sick one describes neither. The frame line already reported `worst` as a bare
// number; this is what it was made of. `mean` carries pre and post only -- scene and
// present are already timed by the caller.
bool renderProfileTakeSpans(FrameSpans& mean, FrameSpans& worst);

// Drain every window accumulator. Call it once per report, UNCONDITIONALLY.
//
// The resets used to sit at the end of each format function, so they only ran on the paths
// that printed. A window that bailed early -- no frames yet, the rasterizer not resolved,
// a zero cycle delta -- carried its counts into the next one and silently doubled its
// per-frame figures. A menu-to-race transition produces exactly that.
void renderProfileEndWindow();

// One line each for the per-second report. All three return false when nothing was
// measured.
bool renderProfileFormatGeometry(char* out, size_t size, unsigned long long frames);
bool renderProfileFormatPresent(char* out, size_t size, double seconds,
                                unsigned long long frames);

// `pre`, broken into sub-spans that tile it -- and, above all, into the part that is WORK
// and the part that is the game asleep.
//
// GPL's foreground loop opens every iteration by polling `getBGCount()` with `Sleep(1)`
// until the simulation advances, and then spinning on the BG lock with another `Sleep(1)`.
// That is why the frame rate equals the task rate exactly with vsync off, and it means a
// large `pre` is the expected shape of a HEALTHY frame rather than a cost to attack. The
// two waits therefore get spans of their own, labelled WAIT on the line; whatever is left
// over is real work and is where a fix would have to go.
//
// Takes no window arguments: it averages over its own count of frames whose markers came
// out in the guaranteed order, and prints that count against the total so a window where
// the ordering broke down cannot be read as a measurement.
bool renderProfileFormatPre(char* out, size_t size);

}  // namespace gplvr
