#pragma once

// Reads GPL's task scheduler through its own EXPORTED API, and reports whether the
// simulation is actually keeping up.
//
// GPL'S LOOP ARCHITECTURE, established 2026-08-31 (derivation in STATUS.md)
//
// It is genuinely multithreaded -- five Win32 threads, not time-sliced coroutines --
// but they are serialised by a hand-off gate, so at most one game task runs at a time:
//
//   main    priority HIGHEST. Drains a semaphore the timer releases and runs the
//           "interrupt": DirectInput polling, a queue service, and the Windows message
//           pump. NOT physics -- I got that wrong once and it cost three answers.
//   timer   priority HIGHEST, pinned to one core. Sleeps against the CPU clock and
//           releases one tick at 2x the sim rate (72 Hz stock). Nothing else.
//   fg      priority BELOW_NORMAL. Runs a state-object loop; fgSleep() = Sleep(2).
//   bg      priority HIGHEST. THE SIMULATION. One iteration per gate slot; each
//           iteration steps every car and then updates force feedback.
//   sv      priority HIGHEST. Session/race state.
//
// The gate (a three-slot rotating critical section) hands BG one slot per tick. If BG is
// still busy the slot is COUNTED AND DROPPED -- no accumulator, no catch-up. The physics
// dt is implicit: every rate-dependent term reads taskGetRate() and trusts that BG really
// ran that many times.
//
// So the number that matters is BG iterations per second against taskGetRate(). Below it
// the world runs in slow motion and does not know -- force feedback included, since its
// derivative term is literally `taskGetRate() * (steer - lastSteer)`.
//
// EVERYTHING HERE COMES FROM GetProcAddress. gplc67.exe exports 294 symbols including the
// whole Papyrus task API, so there is no byte pattern to get wrong and nothing to be
// fragile against across builds. `inBackground`/`inForeground`/`inServer` also let a hook
// ask which task it is running on, which is how the architecture above was confirmed
// rather than guessed.

#include "Pe.h"

#include <cstddef>

namespace gplvr {

// Resolves the exported task API, and -- if `image` is given -- redirects bgSleep's call
// sites so BG's DUTY CYCLE can be measured.
//
// Why duty cycle. "Can this machine run the sim at 90 Hz?" cannot be answered by watching
// bg/s: at 36 Hz it reads 36 whether BG is using 1 ms of its 27.8 or 27. What settles it
// is the fraction of wall time BG spends OUTSIDE bgSleep, because that is its real work.
// 15% busy at 36 Hz means roughly 37% at 90 and the answer is yes; 60% busy means no, on
// any hardware within a generation. The worst single iteration matters too -- one 20 ms
// step blows an 11.1 ms budget however good the average is.
//
// bgSleep is EXPORTED, so its address comes from GetProcAddress and only the call sites
// are found by scanning. Redirecting them needs no prologue patch and is reversible,
// which is the same approach already used for sceneRender and cameraBind.
void taskRateBind(const ModuleImage* image = nullptr);

// What taskGetRate() returns -- 36 stock, 60 under a frame-rate patch, 0 if unresolved.
int taskRateSimHz();

// Names the task the CALLER is running on: "bg", "fg", "sv", "main/other", or nullptr if
// the exports are unavailable. Cheap -- three predicate calls.
const char* taskRateCurrentTask();

// Logs the calling task once per distinct label. Used to establish, from inside the
// running game, which thread renders and which one simulates.
void taskRateReportThread(const char* what);

// Formats the reporting fragment and resets the window. False if nothing to say.
//
// `seconds` is the ACTUAL elapsed window, not an assumed 1.0. The reporter only fires
// when a frame arrives, so during a load stall the window can be many seconds long, and
// the first version of this divided by nothing and printed "bg 1514/s". That is 42
// seconds of ticks labelled as one -- wrong in exactly the case the counter exists for.
bool taskRateFormat(char* buffer, size_t size, double seconds);

}  // namespace gplvr
