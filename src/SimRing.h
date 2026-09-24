#pragma once

// The sim/render state ring (docs/audit-2026-09-18/72-work-vs-wait.md section 6), stage 2.
//
// GPL hands sim state to the renderer inside a lock bracket: the render thread polls
// getBGCount() with Sleep(1) until a new tick has arrived, takes gpl_taskFgBgLock, runs the
// latch callbacks against live sim state, and releases it. Two costs follow. The render thread
// spends the frame's whole slack in that poll -- 3.6 to 4.9 ms of a 6.94 ms frame at the 144 cap
// (report 72 section 1) -- and while it holds the lock the timer's tick grants are deferred, so
// the renderer stops the simulation clock for as long as it latches (report 72 section 4.3).
//
// The ring reverses the hand-off: the sim thread publishes a finished tick into one of four
// slots and never waits; the render thread takes the newest complete slot. A free slot always
// exists because the sim skips the newest and whatever the renderer has pinned. Stage 2 holds
// ONE pin, for the length of a single car's latch, so three slots would do; the fourth is
// provisioned for stage 3, where the blend's two copies become two pinned slots.
//
// THIS IS STAGE 2 OF THE STAGED PLAN, and it is deliberately not the decoupling yet:
//
//   simRing = 1  the sim thread publishes each tick's car sources, and the render thread
//                VERIFIES the ring against GPL's own latch -- it rebuilds each car's render
//                record from the slot and compares it with what gpl_carLatchRenderRecord just
//                wrote, dword by dword. Nothing drawn changes. This is what proves the payload
//                is complete and the mapping exact before anything depends on it.
//   simRing = 2  the render thread MATERIALISES the record from the slot instead of reading
//                live sim state. GPL's lock bracket, its tick wait and the rest of its latch
//                are still GPL's, so the wait is not reclaimed yet -- what changes is that the
//                per-car part of the latch no longer reads across the thread boundary.
//
// Stage 3 -- skipping the bracket, so the render thread stops waiting for a tick and the sim
// clock is never stopped -- needs the rest of slot 1 in the ring (the frame record, the view
// record, particles, debris, flagmen, the skidmark delta queue) and the lifetime hold flag that
// replaces byte_77638C. Section 6.5 of the report has the order.
//
// What a slot holds per car is the SOURCE side of gpl_carLatchRenderRecord 0x46D030: the car's
// own dwords [40] and [271..317], its driver index [196], and fifteen dwords out of the physics
// block [195] (position, distance along and across the track, the segment hint and the 3x3
// matrix). The two lookups that function does -- the driver data and the model handle -- stay
// on the render thread, because model lifetime is the render thread's (report 42).
//
// The publish runs on the sim thread, at the bgSleep call that ends gpl_bgTask's loop body
// (0x452B74): the tick is complete there, and states 1 and 5 and replay never reach
// gpl_simStep (report 45 section 6.1).

#include <cstddef>
#include <cstdint>

namespace gplvr {

// Redirects the sim thread's bgSleep call and GPL's three gpl_carLatchRenderRecord call sites,
// after checking every one of them against its stock bytes. All or nothing, and logged either
// way. Call before interpolationInstall, which chains onto the same three call sites.
void simRingInstall();

// True while the ring is installed and simRing is not 0.
bool simRingActive();

// The address Interpolation.cpp will find in the car latch call sites once this is installed,
// so that its own stock-byte check can accept it. 0 when nothing was installed.
uintptr_t simRingCarLatchHookAddress();

// The once-a-second `ring:` line: publishes, ticks skipped, the slot the renderer took, the
// verify result and the deferred grants GPL counted in the window. False when nothing was
// published.
bool simRingFormat(char* out, size_t size, unsigned frames);

// Totals and the worst case, before the log closes.
void simRingLogSummary();

}  // namespace gplvr
