#!/usr/bin/env python3
"""Generate a GEM+ <BinaryPatch> XML that moves GPL's simulation rate to an arbitrary Hz.

WHAT THIS IS

GPL's sim rate is 36 Hz, baked into ~58 places in gplc67.exe. The published 60 fps and
144 fps patches are hand-built XMLs that rewrite those places. This derives the same thing
for any target rate, from the reference patch plus the rules behind it.

It emits ONE XML. It does not build an exe -- tools/gplexegen.ps1 does that, by handing the
XML list to the real GPLPatchDLL.dll. That split is deliberate: the DLL owns <ExeSection>,
<Relocation>, the resource strings and the hidden +8 address rebase, so nothing here has to
model any of them.

    python tools/gplrategen.py --rate 90 --out 78_90fps.xml
    # then append 78_90fps.xml to the XML list passed to gplexegen.ps1

THE RULES, all derived in STATUS.md and confirmed against two independent patches

1. DIMENSIONAL SCALING. 33 of the 58 sites are constants carrying units of time, and they
   scale by (new/36)^k with k in {2, 1, -1, -2}. k is taken from the REFERENCE patch, by
   dividing its value by the stock value and matching against (60/36)^k -- so the exponent
   is read off a known-good patch rather than guessed here.

2. SUBSTEP RE-QUANTISATION. GPL's physics runs at 288 Hz internally: gpl_simInitPhysicsRate
   computes substeps = round(physicsHz / taskGetRate()) and gpl_simStepAllCars runs that
   many predictor/corrector pairs per tick. substeps * dt must equal exactly one tick, so
   physicsHz must be an integer multiple of the rate:

       N = round(288 / rate);  physicsHz = rate * N

   36 -> N=8 -> 288 (8/288 = 1/36 exactly).  60 -> N=5 -> 300.  144 -> N=2 -> 288, i.e.
   unchanged, which is exactly what Bowden's 144 patch does -- independent confirmation.

3. CODE SITES are copied from the reference verbatim. They are jumps and inserted
   instructions, not rate-dependent values.

THE 63 Hz CEILING, AND THE CAVE THAT LIFTS IT

Three `push imm8` sites carry the rate -- two the timer (2x the sim rate) and one the sim
rate itself -- and imm8 is SIGN-EXTENDED, so a pure byte patch cannot go past a timer of
127 and therefore a sim rate of 63. All three are widened here to `push imm32`, which does
not fit in place, so each site becomes a jump into a stub in the cave at
STANDALONE_CAVE_VA. See STANDALONE_TASK_SITES.

THIS PATCH IS SELF-CONTAINED. It emits the rate, timer and sound-callback registrations,
the substep constants as exact reciprocals, the replay version stamp, the multiplayer
frame-rate handshake and the GUI event throttle. No third-party patch is applied
alongside it, and the 77G patch in particular MUST NOT be -- its caves live in the same
region and GPLPatchDLL has no collision detection whatsoever.

    python tools/gplrategen.py --rate 144 --out "78 144fps standalone v29.xml"

--companion takes another XML and compares every address against it, refusing when both
write the same bytes with different values. Nothing needs it for the normal build; it is
there for checking a patch someone else wants to stack.

WHAT THIS STILL DOES NOT FIX: the AI's other half lives in gpla67.ini, in TICKS, and no
exe patch of any kind touches it. That is tools/gplaiscale.py. See STATUS.md.
"""

import argparse
import io
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gplpatch  # noqa: E402  (PE mapping + the +8 rebase)

STOCK_RATE = 36.0
REF_RATE = 60.0
PHYSICS_HZ_TARGET = 288.0  # stock internal physics rate; N is chosen to sit nearest this

# The substep constants. These do NOT follow rate^k -- see rule 2.
SUBSTEP_DT_SITES = [0x4A4CD5, 0x4A4CF5, 0x4A4D15, 0x4A4D35, 0x559530]
SUBSTEP_HZ_SITE = 0x559534
SUBSTEP_HALF_DT_SITE = 0x559538

# `push imm8` sites carrying the timer rate. imm8 is sign-extended: value must stay <= 127,
# which is why STANDALONE_TASK_SITES widens all three to `push imm32` in a cave.
TIMER_PUSH_IMM8_SITES = [0x4E71B7, 0x4E842D]
SIM_PUSH_IMM8_SITES = [0x40BCA9]

# The sites that carry THE RATE ITSELF, as opposed to a constant derived from it. They
# classify as `scaled` like everything else -- 36 -> 60 is rate^1 -- so they have to be
# excluded by address: at 144 the value does not fit the instruction's immediate, and
# STANDALONE_TASK_SITES replaces the whole instruction instead.
# 0x4E8078 is the odd one out and is dropped for a DIFFERENT reason: it is the imm32 of
# `mov eax, 72` at taskGetRate+8, the value returned when task_force72Hz is set. That flag
# is written only by taskStart, whose sole caller (gpl_main, 0x40BCB9) passes a hard zero,
# so the jnz at 0x4E807C is never taken and eax is always reloaded from task_simRate_Hz.
# The immediate is four bytes and 288 would fit it perfectly well -- it is dead, not
# narrow. 60FPSv2 writes 120 here and 77G writes 144; both are inert, and so is our 72.
RATE_CARRYING_SITES = set(TIMER_PUSH_IMM8_SITES + SIM_PUSH_IMM8_SITES + [0x4E8078])

EXPONENTS = [2, 1, -1, -2]

# THE AI RATE-CONVERSION ACCESSORS -- the one thing in 60FPSv2 that is neither a constant
# nor inert code, and the piece the 144 fps patch does not have at all.
#
# Two tiny functions return AI parameters that are loaded at RUN TIME and therefore cannot
# be scaled by patching a constant:
#
#     00449C70   fld dword ptr [0055812C]   ; a per-tick speed
#     00449C76   ret
#     00449C80   fld dword ptr [00558128]   ; the other of the pair
#     00449C86   ret
#
# flt_558128 / flt_55812C are copied out of the AI parameter block by the mod layer's
# gpla67.ini loader (sub_981C44) from 00555640 / 00555658, whose built-in defaults are 1.0
# and 3.102, and sub_44A640 can override them per track from the pair table at 005F8AA0.
# Because the value only exists after the ini and the track are read, 60FPSv2 converts it
# ON READ instead: it plants a float in the nop padding at 00449C6C and rewrites each
# `ret` as `fmul dword ptr [00449C6C]; ret`.
#
# The float it plants is 0.6 = 36/60, so the exponent is rate^-1 -- a per-tick quantity.
# WE MUST EMIT THIS. Without it the AI reads a speed four times too large at 144 Hz, and
# these are exactly the parameters that decide when a car brakes for what is ahead.
#
# They are NOT in gpla67.ini, so gplaiscale.py does not touch them and there is no risk of
# scaling the same quantity twice.
AI_ACCESSOR_CONST_SITE = 0x449C6C
AI_ACCESSOR_CODE_SITES = [0x449C76, 0x449C86]

# HARDCODED TICK COUNTS IN THE AI CODE. Not constants in .data, not in any ini, and not in
# 60FPSv2 except for one site out of nine -- so nothing scaled them and at 144 Hz every one
# fires four times too early.
#
# gpl_aiUpdateAllDrivers (0x44B300) is the ground truth for which struct fields are
# countdowns: it decrements +590, +1115, +1131, +1139, +1147, +1159 once per driver per
# tick (and +2268 in its first loop). Every site below was found by scanning ALL of .text
# for `mov dword ptr [reg+<one of those offsets>], imm32`, so the list is complete rather
# than sampled. Offsets are relative to the AI driver struct, which the code reaches as
# car+1141.
#
# 60FPSv2 patches 0x0049744A and misses the other eight, INCLUDING 0x004978E8, which is the
# identical instruction 1182 bytes further down the very same function. That is what a
# hand-built patch looks like: the author found one, fixed it, and never swept for siblings.
#
# The last entry is not a countdown reload but the gate that reads one: gpl_aiUpdateAllDrivers
# counts ticks into car+2877 and runs the AI's sound and telemetry update when it reaches 9,
# then zeroes it. Nine ticks is 0.25 s at 36 Hz and 0.0625 s at 144 -- the engine-note update
# for every AI car, sixteen times a second instead of four. This is audible as the cars
# blipping the throttle far too rapidly on the grid, and no ini value affects it.
#
# (value, byte width). The gate is a `cmp r/m32, imm8`, so its scaled value must stay under
# 128 -- fine to 504 Hz.
# THE AI's ENGINE-REV AND THROTTLE-BLIP MACHINE, sub_44C220.
#
# This is what the cars do on the grid while they wait for the green: a three-state machine
# on flags 2/4/8 of car+4 that ramps the throttle up, holds, drops it, holds, repeats. Both
# holds are RANDOMISED TICK COUNTS, and both ramps are PER-TICK rates:
#
#     state 2: count down car+1199; when it expires, throttle += 0.045/tick to a 0.85 cap,
#              then count down car+1195; when THAT expires, go to state 4
#     state 4: throttle -= 0.5/tick; at zero, reload car+1199 with rand%36 + 12 ticks and
#              car+1195 with rand%12 + 11 ticks, and go back to state 2
#
# 12..47 ticks is 0.33..1.31 s at 36 Hz and 0.08..0.33 s at 144. That is the blipping the
# user heard as "at least twice as fast as vanilla", and no ini value or track.ini
# coefficient touches it because every number here is an instruction immediate or a private
# .rdata float.
#
# It is reached through the mod layer: sub_987019 sets flt_179CBEC (its rev target scale)
# and tail-calls sub_44C220, and sub_987019 is called from gpl_aiComputeDlongSpeedGoal.
#
# CRUCIALLY, car+1199 and car+1195 are decremented INSIDE sub_44C220, not in
# gpl_aiUpdateAllDrivers -- which is why the earlier countdown sweep, keyed off the fields
# that gpl_aiUpdateAllDrivers decrements, did not find them. That sweep was complete for
# ITS function, not for the AI.
#
# Every address here was checked for exclusivity before being listed: each is referenced
# only from inside sub_44C220, so scaling it cannot disturb anything else. One that is NOT
# exclusive is deliberately absent -- the RPM decay of 125/tick at 0x005547B8 is also read
# from 0x004A37A2, and is left alone until that use is understood.
#
# (address, format, stock value, exponent, note). Exponent +1 scales by rate/36 (a duration
# in ticks), -1 by 36/rate (a per-tick rate of change). Verified rate-invariant: 0.045/tick
# is 1.62/s at both 36 and 144 Hz, 3.3680556 is 121.25/36 and becomes 121.25/144, and the
# holds stay 0.33..1.31 s.
AI_BLIP_SITES = [
    (0x0044C3DE, "i4", 36,          +1, "rand %N for the car+1199 blip hold"),
    (0x00539268, "f4", -11.99988,   +1, "car+1199 blip hold base"),
    (0x0044C404, "i4", 12,          +1, "rand %N for the car+1195 blip hold"),
    (0x0053926C, "f4", -10.8,       +1, "car+1195 blip hold base"),
    (0x00539258, "f4", 0.045,       -1, "throttle ramp per tick"),
    (0x00539274, "f4", 0.2,         -1, "throttle decay per tick"),
    (0x00539278, "f4", -3.3680556,  -1, "RPM delta per tick, = -121.25/36"),
]

# SITES WHERE THE 60FPSv2 REFERENCE IS WRONG AND MUST NOT BE FOLLOWED. Keyed by real VA;
# the site is dropped entirely, leaving the stock bytes.
#
# 0x44B4BC is the `push 0.25` argument to the AI's gpl_engineWearAndFailure call. That
# argument is the REAL SECONDS since the last call (the function integrates wear as
# value*dt and accumulates dt against a 0.2 s threshold), and the call sits inside the
# every-9-ticks block whose gate at 0x44B458 this patch ALSO scales (9 -> 36 ticks). The
# gate scaling keeps the real interval at 0.25 s, so the argument must STAY 0.25:
# scaling both, as v1 of this table did by inheriting the reference's rate^-1, makes AI
# engine wear run 4x slow. (60FPSv2 scaled the argument and NOT the gate -- also
# consistent, by the opposite choice.) One of the pair, never both.
#
# 0x53919C is a .lp-WORLD constant the reference wrongly treated as current-rate. Its only
# two readers are the `fcomp`s at 0x44A478 and 0x44A4B1 in sub_44A400, which runs once at
# track load and clamps the minrace and maxrace lines' raw dlong-speed field IN PLACE --
# record field +0, m/tick at 36 Hz forever. A .lp-world floor must stay in .lp-world units.
# The store immediate at 0x44A434 (0.186141) is not patchable by the reference's mechanism
# and was left stock, so scaling only the compare made the site internally inconsistent
# even at 60 Hz. At 144 the floor fell from 6.70 m/s to 1.67 m/s, letting any minrace or
# maxrace node between those speeds keep a crawl value.
#
# Reader-exclusive to .lp arithmetic, so dropping the site is the whole fix -- no cave
# redirect is needed. Note the family splits here: 0.62040, 0.37224 and 0.062040 are read
# against car+482, which IS current-rate, and legitimately want rate^-1.
REFERENCE_MISTAKES = {
    0x44B4BC: "engine-wear dt: its 9-tick gate is scaled instead, which keeps the real "
              "0.25 s interval; scaling both would double-convert",
    0x53919C: "the minrace/maxrace .lp dlong-speed floor in sub_44A400: a .lp-world "
              "constant, 36 Hz forever, that the reference scaled as if current-rate",
}

# TICK-COUNT IMMEDIATES FOUND BY THE 2026-09-04 CONSUMER AUDIT. Same class as
# AI_COUNTDOWN_SITES (hardcoded tick counts nothing scaled) but they live inside
# arithmetic -- rand %N holds and an imm8 add -- so neither the countdown sweep (keyed on
# `mov [reg+off], imm32`) nor the reference diff could see them.
# (address of the immediate, stock value, width, note). All scale rate^+1.
AI_EXTRA_TICK_SITES = [
    # The two ambient-sound cycle periods (v19). AUDIO, not AI -- they share this table
    # only because the shape is identical: a tick count in an instruction immediate.
    # sub_4A7B40 picks a new random ambient level every 108 or 252 ticks, which are 3.0 s
    # and 7.0 s at 36 Hz (3x36 and 7x36) and 0.75 s / 1.75 s unscaled at 144.
    # gpl_netCarExtrapolateRecord's confidence blend ramp (v22). The weight over packet age
    # |n| is: |n| <= 5 -> 0; 5 < |n| <= 9 -> (|n| - 5) * 0.2; |n| > 9 -> 1.0. All three
    # numbers are tick counts and the slope 0x53A83C scales with them, so the ramp reaches
    # the same weight at the same wall-clock age: at 144 Hz (36 - 20) * 0.05 = 0.8, exactly
    # what (9 - 5) * 0.2 gives at 36 Hz.
    #
    # These sit on the !(flags & 0x20) branch, so offline they are reached only during
    # REPLAY PLAYBACK, and online for remote cars. Correct either way.
    (0x0049982D,  9, 1, "blend ramp upper bound, cmp |n|, 9"),
    (0x0049986B,  5, 1, "blend ramp lower bound, cmp |n|, 5"),
    (0x00499870, -5, 1, "blend ramp offset, add eax, -5 (a SIGNED imm8)"),
    (0x004A7B46, 108, 4, "ambient sound cycle period 1, ticks (mov ecx, 6Ch)"),
    (0x004A7B5D, 252, 4, "ambient sound cycle period 2, ticks (mov ecx, 0FCh)"),
    (0x004978B9,  27, 4, "rand %N for the car+1155 post-lift brake-derate hold, "
                         "gpl_aiComputeDlongSpeedGoal"),
    (0x004978C8,   9, 1, "base of the car+1155 hold (add edx, imm8), same site"),
    (0x00442CD6, 180, 4, "rand %N added to the car+1199 blip hold when the START OF RACE "
                         "state is entered -- the initial stagger of the grid blipping"),
    (0x00447ACF,  36, 4, "rand %N jitter on the car+1115 decision hold, "
                         "gpl_aiSetDecisionHoldTicks"),
]

# FOUR MORE `push 72.0f` INTO gpl_aiSetLineHoldTicks (0x44C9D0), which does nothing but
# `car+1139 = (int)arg` -- a countdown gpl_aiUpdateAllDrivers decrements. There are five
# such sites in the image; the reference scaled ONE (0x44B66D), so these four have been
# holding a 2-second line decision for half a second at 144 Hz.
#
# All four are on the post-contact path, which is why they belong with the traffic-crash
# work: a car that has just been hit resumes steering back to the line while still
# unsettled, and a nudge becomes a spin.
AI_LINE_HOLD_SITES = [
    (0x004433D1, 72.0, +1, "line hold on entering the CRASHING state (sub_443360, the "
                           "same function whose car+1159 = 144 reload is already scaled)"),
    (0x00443AC9, 72.0, +1, "line hold, gpl_aiState_returnToPitting_handler"),
    (0x00444A8C, 72.0, +1, "line hold, off-track recovery (sub_444A20)"),
    (0x0044D571, 72.0, +1, "line hold, mechanical failure (sub_44D3B0)"),
]

# THE LATERAL-VELOCITY TERM IN THE DLAT ACCELERATION CEILING -- the traffic-crash fix.
#
# gpl_aiDlatSpringAccel (0x498080) clamps its spring output to
#
#     ceiling = car+236 * car+376 * gpl_ai_latCeilOnLine/OffLine     [m/tick^2]
#
# and then adds car+1046 / car+1050 to it. Those two are a lateral VELOCITY in m/tick:
# sub_447CC0 writes them from the blocker map's relative dlat velocity (car+522 ours minus
# car+522 theirs, and its negation), and the only other write is car+1046 = -car+522 at
# the track edge. Adding m/tick to m/tick^2 is stock's per-tick shorthand -- "the
# acceleration that cancels this velocity in one tick" -- and the tick it means is the
# 36 Hz one.
#
# So the term is 4x too strong at 144. The ceiling is about 0.011 m/tick^2 (the traction
# circle, ~1.47 g); a 0.5 m/s lateral closing rate is worth 18 m/s^2 at 36 Hz, leaving a
# firm 0.38 g nudge, and 72 m/s^2 at 144 Hz, which drives the clamp to -5.9 g. The lateral
# command flips sign and saturates.
#
# It matches every part of the report it was hunted for: dormant in clear air, armed only
# when another car is alongside with a closing rate, severity linear in the tick rate, and
# PRESENT IN STOCK 36 Hz AT A QUARTER OF THE STRENGTH -- which is the user's "vanilla did
# this occasionally too, but less violently".
#
# Exclusivity: a byte search on displacements 0x416/0x41A finds writes only in sub_447CC0
# (five each) and the constructor's zero-init, and reads only these two fadds plus sign
# tests, which a positive scale preserves.
#
# The edit: each site is a 6-byte `fadd dword ptr [esi+disp32]`, replaced by a 5-byte call
# to a stub plus a nop. esi is the AI driver at both sites. The stub reproduces the fadd
# with the scale folded in, using the 36/rate constant v6 already planted in the cave tail.
DLAT_VEL_SITES = [
    (0x00498244, 0x41A, "car+1050, the +dlat side"),
    (0x004982BF, 0x416, "car+1046, the -dlat side"),
]
DLAT_VEL_CAVE_VA = 0x00988950   # verified all-zero from 0x988950 to 0x988A00 in the stock
DLAT_VEL_STUB_STRIDE = 16       # exe; Lee's 77G caves do not start until 0x988B70

# DIMENSIONAL CONSTANTS FOUND BY THE SAME AUDIT, each verified EXCLUSIVE to its consumer
# by cross-reference before being listed. (address, format, stock value, exponent, note).
AI_EXTRA_FLOAT_SITES = [
    # mod_aiGetMaxAccel computes mod_ai_maxAccelScale = THIS / mod_aiCarClassScale(class),
    # and gpl_aiGetMaxAccel multiplies (engine force / mass) by it -- i.e. it converts a
    # real-world m/s^2 into the AI's m/tick^2. 0.0022530865 = 2.92/36^2, where 2.92 is the
    # baseline class figure and 36^2 the seconds^2 -> ticks^2 conversion. That settles the
    # long-open "units of mod_aiCarClassScale" question: the class scale is rate-free and
    # the 36^2 lives HERE, so this constant is rate^-2. Without it car+1022 (the accel
    # budget and the throttle formula's denominator) stays in 36 Hz units at any rate.
    # THREE CLAMP ASSIGNMENTS v5 BROKE. In gpl_aiFakePhysicsTick (0x496AB0) each clamp
    # compares against an .rdata float and assigns a bit-identical INLINE IMMEDIATE. v5
    # scaled the .rdata halves and left the immediates, so the clamps now amplify instead
    # of clamping: `if (x > 0.09306) x = 0.37224` sets a dlat velocity of 13.4 m/s to
    # 53.6 m/s sideways. Each takes the same exponent as the partner it must match.
    (0x00496B35, "f4", 0.6204,    -1,
     "clamp assignment paired with flt_53A700 (already rate^-1); v5 scaled the compare "
     "and left the assignment, so it amplifies"),
    (0x00496C99, "f4", 0.37224,   -1,
     "clamp assignment paired with flt_53A714 (already rate^-1), same v5 regression"),
    (0x00496CB2, "f4", -0.37224,  -1,
     "clamp assignment paired with flt_53A710 (already rate^-1), same v5 regression"),
    # The other end of a clamp whose partner flt_53A704 IS scaled rate^-2, leaving the
    # pair 12x asymmetric. STATUS's v5 note called these parameter-table reads; they are
    # literal .rdata constants, read at 0x496B72/B81/CDC/CEB.
    (0x0053A708, "f4", 0.011343,  -2,
     "gpl_aiFakePhysicsTick accel clamp, positive end; partner flt_53A704 already rate^-2"),
    (0x0053A718, "f4", -0.011343, -2,
     "gpl_aiFakePhysicsTick accel clamp, negative end; same pair"),
    # THE ABRUPT-DODGE THRESHOLD (v15). Read at 0x445FB6 and 0x446117 in
    # gpl_aiState_avoidLateralBlocker, and nowhere else -- a byte search on the
    # displacement over the whole image, mod range included, returns exactly those two.
    #
    # The unit comes from the WRITER, not the reader. The fcomp operands are the blocker
    # record's +32/+36, which sub_447F50 fills with `ourCar+522 - otherCar+522` and its
    # negation; sub_447CC0 then copies those same values into car+1046/1050, the pair v13
    # established as a lateral velocity in m/tick. So this threshold is m/tick too.
    #
    # It chooses abrupt_line_transition (off_55782C) over basic_line_transition
    # (off_557828). Unscaled, -0.05 m/tick at 144 Hz demands 7.2 m/s of lateral closing,
    # which two racing cars never reach -- so the abrupt set is dead and every emergency
    # dodge uses the basic one, whose spring is 35.6% weaker over a 33% longer transition.
    #
    # Corroboration: switch_to_cornering_dlat_velocity, the same physical quantity, is
    # already rate^-1 in the ini and carries the comment "meters/tick".
    (0x00538870, "f4", -0.05, -1,
     "the lateral closing rate above which the AI dodges with the ABRUPT line-transition "
     "set instead of the basic one"),

    # THE LOSS-OF-CONTROL SPEED NORMALISER (v15). One reader, 0x49850B in sub_498500:
    # `car+510 * 1.0745755`, clamped at 2.0, feeding sub_44CA20's "am I still in control"
    # decision -- false enters the CRASHING state.
    #
    # THE INI NAMES THE UNIT. The sibling constant in the same expression is the
    # descriptor slot for `loss_of_control_speed_adj_scaling`, whose gpla67.ini comment
    # reads "scaling factor for loss of control adjustment based on speed relative to
    # 75 MPH" -- and 1/1.0745755 = 0.9306 m/tick, which at 36 Hz is 33.5 m/s, exactly
    # 75 mph. So this is 1/(m/tick) and the product is meant to be dimensionless.
    #
    # Unscaled at 144 Hz a 150 mph car normalises to 0.5 instead of 2.0, leaving the AI
    # roughly 37% more forgiving at racing speed with the clamp unreachable below about
    # 300 mph: a bot that has genuinely lost it keeps steering normally while sideways.
    (0x0053A7A4, "f4", 1.0745755, +1,
     "1/(m/tick) normalising speed against 75 mph for the loss-of-control decision"),

    # THE BLOCKER-COMMITMENT RAMP (v16). Single reader, 0x446C74 in
    # gpl_aiState_evaluateBlocker: a per-tick counter n scales this constant and the
    # product is compared against a speed ratio, so the car ahead is treated as a blocker
    # only after the ramp has climbed far enough.
    #
    # BOTH SIDES OF THE COMPARISON CARRY sigma = 36/rate, which is why the exponent is
    # rate^-2 and not the rate^-1 an audit proposed. The right-hand side is a raw .lp
    # speed (gpl_aiLpDlongSpeed, a direct record field with no car+376 conversion) divided
    # by car+510; the left-hand side comes from gpl_aiGetDlongSpeedScale. n itself ticks
    # rate/36 times faster. c = c0 * (36/r)^2.
    #
    # MEASURED, not inferred. gpl_aiGetDlongSpeedScale returns car+276 for AI drivers (the
    # car+769 flag is set for all of them), and no instruction in the image writes that
    # field -- a bulk copy fills it, so static analysis could not settle its units. The
    # aiProbe detour read it in both builds: 0.947407543 at 36 Hz against 0.236851885 at
    # 144, a ratio of exactly 4. It is sigma-carrying.
    (0x00538880, "f4", -0.01, -2,
     "gpl_aiState_evaluateBlocker's per-tick blocker-commitment ramp; both sides of its "
     "comparison carry sigma, so the ramp is 16x too fast unscaled"),

    # THE 10 MPH "AM I MOVING" GATE (v17). One xref, `fcomp` at 0x44CADC in sub_44CA20 --
    # the same loss-of-control decision the v15 normaliser feeds.
    #
    # THE UNIT COMES FROM THE WRITER. car+482 is filled at 0x44B678-0x44B69D in
    # gpl_aiUpdateAllDrivers with sqrt(car+510^2 + car+522^2) -- the magnitude of the
    # (dlong, dlat) velocity pair, m/tick at the CURRENT rate. 0x44B461 converts it with
    # exactly ONE factor of gpl_ticksPerSecond; an acceleration would need two.
    #
    # The family is an mph ladder in m/tick@36 with 1 mile = 1608 m: 0.6204 is 50 mph,
    # this is 10, and 0.06204 is 5. Unscaled the gate demands 40 mph at 144 Hz, so its arm
    # is dead between 10 and 40 mph real.
    #
    # Not a descriptor slot: 0x5392A4 is in the compiler's contiguous float pool, not on a
    # 24-byte stride. The same VALUE does sit in two real descriptor records at 0x5549E0
    # and 0x5549F8, which is a coincidence in different storage.
    (0x005392A4, "f4", 0.12408, -1,
     "sub_44CA20's 10 mph 'am I moving' gate on car+482, m/tick"),

    # THE AI'S EMA FILTER COEFFICIENTS (v17). sub_44AC30 builds twelve-byte records
    # {int N; float 1/N; float acc}, updated inline at each use as
    # `acc += (x - acc) * (1/N)` -- a per-tick exponential average with a time constant of
    # N ticks. At 144 Hz the wall-clock window is a quarter of stock.
    #
    # PATCH THE RECIPROCAL, NOT THE COUNT. Sweeps on every record's int field (0x2AA,
    # 0x2B6, 0x2C2, 0x2CE, 0x2DA, 0x2E6, 0x2F2, 0x2FE, 0x442, 0x4FB) find ctor and reset
    # writes and NO reader: the integer is dead storage and the 4/6/3 immediates a patch
    # would naturally aim at change nothing.
    #
    # Both addresses are the INLINE IMMEDIATE itself, verified by reading the bytes back:
    # 0x44AD8B holds AB AA 2A 3E and 0x44ADD2 holds AB AA AA 3E.
    #
    # rate^-1 is this file's stock*scale convention rather than the exact
    # equal-time-constant value 1-(1-a)^(36/rate) = 0.0446 -- a 7% difference on a feel
    # filter. The change is DC-neutral: it restores response time and moves no steady
    # state, so it cannot interact with the ini's pitch/roll_accel_coeff.
    #
    # The 0.25 sibling at 0x44AF66 is deliberately NOT here: it seeds both a per-tick EMA
    # and the per-segment filter array, which sub_448CD0 steps once per segment-END
    # CROSSING -- an event count, rate-free. Exclusivity fails, so it stays stock.
    (0x0044AD8B, "f4", 0.16666667, -1,
     "1/6 EMA coefficient shared by four per-tick AI filters: applied dlong accel "
     "(0x496C04, 0x497884), car+526 lateral accel (0x496D6E), a traction-usage ratio "
     "(0x497920) and car+538 (0x44C0E4)"),
    (0x0044ADD2, "f4", 0.33333334, -1,
     "1/3 EMA coefficient for the rev/load fraction read through sub_44C0D0; its second "
     "target car+770 is dead, so the live readers are unit-uniform"),

    # THE OTHER HALF OF A LATCH v13 ONLY HALF-SCALED (v18). In sub_4A4EA0, the post-substep
    # per-contact hook:
    #
    #   0x4A4F3C  fild  dword ptr [ecx+47Bh]      ; car+1147, a per-tick countdown
    #   0x4A4F42  fcomp ds:flt_53AC74             ; 9.0   <- THIS, was never scaled
    #   0x4A4F4F  mov   dword ptr [ecx+47Bh], 9   ; imm at 0x4A4F55, already scaled to 36
    #
    # Same field on both sides, so both must carry the same exponent. With only the store
    # scaled the latch cycles 36 -> 8 instead of being topped up every tick: sustained
    # impacts behave identically and only the tail differs, expiring up to 0.19 s early.
    #
    # car+1147 is decremented once per tick by gpl_aiUpdateAllDrivers (0x44B5E5-0x44B5F0),
    # zero-initialised at 0x44AFC4 and cleared on entering SERVICING at 0x44467B. Its only
    # consumer is `if (this+1147 > 0) return 0;` at 0x44CA42, the head of sub_44CA20 --
    # the same "am I still in control" test the 10 mph gate above guards.
    #
    # Exclusivity proved: one reader. Not a descriptor slot -- 0x53AC64-0x53AC78 is a plain
    # float pool (1296.0, 9.8, 6.0025, 21609.0, 9.0, 0.75) with no 0x42 record tag.
    (0x0053AC74, "f4", 9.0, +1,
     "the compare half of car+1147's crash latch in sub_4A4EA0; v13 scaled only the store "
     "at 0x4A4F55, leaving the pair inconsistent"),

    # ------------------------------------------------------------------------------
    # THE SIX UNREACHED LINE STATES (v18). The largest AI finding of the campaign, from
    # the 2026-09-07 enumeration of the parameter-descriptor table.
    #
    # GPL registers 19 fuzzy-line parameters for each of FIFTEEN line states
    # (gpl_aiRegisterFuzzyLineParams, 0x444D30), reading each state's values from an ini
    # section named after the state. gpla67.ini names only NINE. The six with no section --
    # try_to_draft_line_transition, panic_follow_line, follow_the_leader, "do nothing"
    # (with a space, so it could never match), crashing, in_pit_stall -- fall back to the
    # DESCRIPTOR DEFAULTS below, which no ini reaches and gplaiscale therefore cannot
    # touch. Every per-tick default among them has been running unscaled at 144 Hz.
    #
    # panic_follow_line and crashing are precisely the states the #5, #6, v13 and v15 work
    # has been chasing. That is why this is worth its own build.
    #
    # THE DEFAULTS ARE LIVE ONLY BECAUSE THE KEYS ARE ABSENT FROM gpla67.ini -- checked
    # per key, section-qualified. The nine named states keep overriding these from their
    # own ini sections, already scaled, so there is no double conversion.
    #
    # The seed set is dword_557860 = descriptor indices 64,65,66,67,68,70,74,6,7,112 (plus
    # the same nine again for the `cornering_` twins). Of those, 68 is centrifugal_lookahead
    # (inert downstream, refuted as #9), 70 is desired_dlong_sep in METRES, and 74 is
    # avoid_time_coeff which #4 settled at rate^0 -- so the rate-carrying members are the
    # seven below, plus two more descriptor defaults that are read directly.
    (0x005546F8, "f4", 0.007, -2,
     "physics|dlat_accel_k1, 1/tick^2, seeds the six unreached states and the pit-stall "
     "line classes"),
    (0x00554710, "f4", 0.09, -1,
     "physics|dlat_accel_k2, 1/tick, same seed set"),
    (0x00554C68, "f4", 108.0, +1,
     "behavior|long_term_lookahead, ticks; seeds six states AND is read directly into the "
     "track struct via sub_445B80+0x10"),
    (0x00554C80, "f4", 36.0, +1,
     "behavior|short_term_lookahead, ticks, same two consumers"),
    (0x00554C98, "f4", 18.0, +1,
     "behavior|dlat_lookahead, ticks"),
    (0x00554CB0, "f4", 20.0, +1,
     "behavior|dlat_trans_time, ticks; seeds the six states plus follow_line, "
     "leaving_pits, line_improvement and every cornering_dlat_trans_time except "
     "basic and abrupt"),
    (0x005550E8, "f4", 0.24816, -1,
     "behavior|switch_to_cornering_dlat_velocity, m/tick -- the ini's own comment on the "
     "same quantity reads 'meters/tick'"),
    (0x00554E00, "f4", 36.0, +1,
     "behavior|stay_on_track_trans_time, ticks, read at 0x4465D7 in "
     "gpl_aiState_stayOnTrack"),
    (0x00554FC8, "f4", 4.0, +1,
     "behavior|goal_dlat_accel_averaging, ticks -- sub_44AC30 stores it as car+742 and "
     "1/it as car+746. This is one of the ctor 'hardcoded' filter defaults: it is not "
     "hardcoded at all, it is a descriptor slot absent from the ini"),

    # ------------------------------------------------------------------------------
    # PLAYER CONTROLS AND AUDIO (v19). Everything above this line is the AI; these are the
    # human's car and the sound. Found by the 2026-09-07 audit of the per-tick tier.
    #
    # Several of these only matter to players using digital steering or the driving aids,
    # so a wheel with the aids off never reaches them. They are patched anyway: the sites
    # are correct for everyone, and the patch is a standalone artefact other people run.
    #
    # DIGITAL-STEERING SELF-CENTRING. sub_4A2FC0, entered only when steering is bound to a
    # digital control; a wheel takes the early return at 0x4A2FDF. Both terms are applied
    # once per call, i.e. per tick:
    #     v7  = (speed < 8.9337) ? speed*[53AB98] : [53ABA0]/(speed + 50.964)   ; re-centre
    #     v11 = v7 + 2.6738951/(speed + 32.203)                                 ; steer in
    # sub_4A4720 returns |velocity| in SI m/s, so the speed offsets and the 8.9337 knee are
    # rate-free and only the numerators carry ticks.
    #
    # THE CORROBORATION IS DECISIVE: 77G already writes 0x53ABA8 as -0.66847 =
    # -2.6738951/4, exactly rate^-1. It scaled the steer-IN term and left the re-CENTRING
    # term, so at 144 Hz a held key steers correctly but releasing it snaps back 4x fast --
    # full lock to centre in 0.063 s instead of 0.25 s.
    (0x0053AB98, "f4", 0.014551544, -1,
     "digital-steering re-centring rate, low-speed branch (sub_4A2FC0)"),
    (0x0053ABA0, "f4", 7.7867613, -1,
     "digital-steering re-centring rate, high-speed branch; partner of the 0x53ABA8 that "
     "77G already scaled"),

    # DIGITAL THROTTLE AND BRAKE RAMPS. Two readers each (0x4A314F/0x4A316F in sub_4A3110,
    # 0x4A332D/0x4A334E in sub_4A32A0), both the same per-tick-step unit.
    (0x0053ABAC, "f4", -0.055555556, -1,
     "digital pedal PRESS step, -1/18 per tick = 0.5 s full travel at 36 Hz"),
    (0x0053ABB0, "f4", 0.16666667, -1,
     "digital pedal RELEASE step, 1/6 per tick"),

    # THE DRIVING AIDS. Both are per-tick integrators whose error term is already shaped
    # into [-1, 1] by dimensionless constants (1.6, 0.625, -0.26315787) that must NOT be
    # scaled -- only the slew is a rate.
    (0x0053ABC0, "f4", -0.0625, -1,
     "throttle-help slew, per tick: full travel in 16 ticks = 0.44 s at 36 Hz, 0.11 s "
     "at 144 unscaled"),
    (0x0053ABCC, "f4", -0.125, -1,
     "brake-help slew, per tick, same shape"),

    # THE TACHOMETER TELLTALE drag-down, a DOUBLE at 0x4A2E1E. car+0x464 is the RPM
    # high-water mark and this is how fast it is dragged back toward the live RPM.
    (0x0053AB80, "f8", 1050.0, -1,
     "tacho telltale drag-down, rpm per tick (cosmetic)"),

    # THE AMBIENT SOUND RAMPS, sub_4A7B40. Audio only -- both feed sub_4E6E00, the same
    # volume setter sub_4A1240 uses. The two tick PERIODS are in AI_EXTRA_TICK_SITES.
    (0x0053AD48, "f4", -0.01, -1,
     "ambient level RISE step per tick, both ramps (two xrefs, same units)"),
    (0x0053AD4C, "f4", 0.005, -1,
     "ambient level FALL step per tick, ramp 1"),
    (0x0053AD54, "f4", 0.002, -1,
     "ambient level FALL step per tick, ramp 2"),

    # THE DRIVER'S HEAD ATTITUDE (v20). gpl_viewAttitudeSmooth_perTick runs two one-pole
    # IIRs once per tick with a coefficient that depends on attitude but NOT on rate:
    #
    #     alpha = min((pitch^2 + roll^2) * 0.4052847, 0.8) + 0.1
    #     smoothed += alpha * (raw - smoothed)
    #
    # so the time constant is in samples and the head settles 4x faster at 144 Hz. The
    # output reaches car+1176/1180 through gpl_ffbFillControlState and is read by the view
    # code (0x454894) and by gpl_mirrorViewCompute (0x454EE7) -- it is the driver's
    # head/eye lean, and the mirror's copy of it.
    #
    # SCALING BOTH THE GAIN AND THE BASE IS AN APPROXIMATION, and worth stating plainly.
    # The exact equal-time-constant map is alpha' = 1-(1-alpha)^(36/rate), which is
    # non-linear and cannot be produced by scaling an affine expression. But over the real
    # operating range -- pitch and roll are the SMALL Euler angles, so pitch^2+roll^2 is
    # about 0.3 rad^2 at most and alpha runs 0.10 to 0.22 -- rate^-1 tracks the exact value
    # to within 0.006 in alpha and always errs toward MORE smoothing.
    #
    # The 0.8 cap at 0x53AB44 is deliberately NOT here: it is shared with
    # gpl_controlsSubstepAdvance (0x4A3F71), where it means something else. Leaving it stock
    # is safe because with the reduced gain it only binds past ~80 degrees of combined
    # pitch and roll.
    (0x0053AB40, "f4", 0.4052847, -1,
     "head-attitude IIR gain on (pitch^2+roll^2), per tick"),
    (0x0053AB48, "f4", -0.1, -1,
     "head-attitude IIR alpha base, per tick (an fsub, so the base is +0.1)"),

    # PARTICLES (v20). gpl_particleUpdate runs once per tick from gpl_simStep, so every
    # per-tick step in it is 4x fast at 144 Hz. Purely visual. Only the EXCLUSIVE slots are
    # here -- four more particle constants sit in slots shared with spawn-position jitter
    # and with the renderer's billboard corner, and those need operand relocation into the
    # cave rather than an in-place rewrite. See STATUS.
    (0x00539DC8, "f4", -0.012, -1,
     "type-16 dust rise per tick"),
    (0x00539DD0, "f4", -0.02, -1,
     "smoke puff radius growth per tick"),
    (0x00539DD8, "f4", 0.002, -2,
     "spark gravity: a per-tick decrement to particle+0x3C, which is itself a per-tick "
     "position step, so units/tick^2"),
    (0x00539DA8, "f8", 3.0, -1,
     "smoke turbulence coefficient p0 = rnd*3.0 (a DOUBLE). Scaling THIS rather than the "
     "radius growth is what keeps particle+8 consistent: p8 is both the rendered puff "
     "radius and the alpha-decay coefficient, so p0*p8 needs the factor and the radius "
     "does not"),
    (0x00539D78, "f4", 20.0, -1,
     "type-32 sparks emitted per call, and the emitter runs once per tick"),
    (0x005393E0, "f4", -0.015, -1,
     "dust/skid intensity ramp per tick in sub_44FAC0, clamped at 1.0"),

    # THE JITTER GATE (v23). `r = rnd(); fcom 0.35; if (r < 0.35) v = r*0.1 - 0.0175`.
    # Note `fcom`, not `fcomp` -- the draw stays on the stack and IS the magnitude, so the
    # gate and the magnitude are the same number and the jitter is zero-mean only because
    # 0.35 * 0.1 = 2 * 0.0175.
    #
    # THIS MUST SHIP WITH THE 0.1 RELOCATION IN PARTICLE_REDIRECTS AND NEVER ALONE.
    # Quartering the gate by itself confines r to [0, 0.0875], so v lands in
    # [-0.0175, -0.00875] and every sample is negative: a zero-mean wobble becomes a
    # one-sided drift. With 0.1 -> 0.4 the mean stays exactly zero
    # (0.4 * 0.0875/2 = 0.0175), which is why 0x539DC4 = 0.0175 is deliberately NOT scaled.
    (0x00539DC0, "f4", 0.35, -1,
     "per-tick probability of a smoke-jitter event; EXCLUSIVE, one reader at 0x47CA38"),

    # THE NET-EXTRAPOLATION AGE WEIGHTS (v22). Both EXCLUSIVE, one xref each.
    #
    # The Gaussian is the one that matters: it sits on the COMMON TAIL of
    # gpl_netCarExtrapolateRecord, which every path with flags&1 falls through to, so it is
    # LIVE OFFLINE for the player and every AI car. rec+0x1F0 = exp(-(n*c)^2) where n is the
    # packet age in ticks -- a confidence that decays over 9 ticks, i.e. 0.25 s at 36 Hz and
    # 0.0625 s unscaled at 144.
    #
    # The stored value is the literal 0.1111111f (0x3DE38E37), which is 2 ulps off 1/9 --
    # that is why an earlier search for the exact float 1/9 (0x3DE38E39) found nothing.
    (0x0053A854, "f4", 0.1111111, -1,
     "gpl_netCarExtrapolateRecord's Gaussian confidence width, 1/ticks"),
    (0x0053A83C, "f4", 0.2, -1,
     "the 5..10 tick blend ramp's slope, 1/ticks; its three bounds are in "
     "AI_EXTRA_TICK_SITES and move with it"),

    # DELIBERATELY NOT HERE: 0x0053A830 = -0.75, the per-tick age slope in sub_498CE0's
    # `rec+68 = rec+68 * 0.5 + 0.75 * n`. It is exclusive and it IS a per-tick rate, but the
    # 0.5 beside it is the shared slot and stays 0.5, so the expression's steady state is
    # 0.75n/(1-0.5) = 1.5n. Scaling only the slope would move that to 0.375n and change the
    # DC GAIN fourfold -- the same trap that made the first FFB smoothing patch wrong. Fix
    # the pole and the slope together in a cave, or leave both stock.

    # THE "CRASHED CAR HAS STOPPED" TIMER (v14). In the CRASHING state's per-tick handler
    # sub_443430, car+1143 counts consecutive ticks where the wreck's |v|^2 + |w|^2 is
    # under flt_5387E0 = 0.01, and the state transition fires once the count passes this
    # threshold -- so on the 10th at-rest tick, 0.278 s at 36 Hz.
    #
    # A FLOAT compared through fild/fcomp, which is why every previous sweep missed it:
    # the countdown sweep wanted an imm32 reload, an integer up-counter sweep wants an
    # integer immediate, and the reference diff never touched it. flt_5387E4 has EXACTLY
    # ONE xref. Its neighbour flt_5387F0 = 18.0 IS scaled by the reference, but for a
    # different consumer (gpl_aiPredictOneTickBallistic) -- good corroboration that half a
    # second recurs in this state family and that the reference caught one form of it.
    #
    # 9 -> 36 fires on the 37th tick, 0.257 s, against an exact 39 for 0.278 s. The
    # one-tick residual is the price of following this file's stock*scale convention, and
    # 20 ms in a wreck-has-settled test is immaterial.
    #
    # flt_5387E0 = 0.01 itself stays: the velocity terms come off the physics object in
    # SI, so 0.01 is a 0.1 m/s threshold in real units and is rate-free.
    (0x005387E4, "f4", 9.0, +1,
     "the at-rest tick count that ends the CRASHING state (car+1143)"),
    (0x0053A740, "f4", 0.0022530865, -2,
     "mod_aiGetMaxAccel's m/s^2 -> m/tick^2 conversion, = 2.92/36^2"),
    # The blip machine's state-4 throttle release, a DOUBLE: throttle -= 0.5 per tick.
    # The one per-tick rate in gpl_aiUpdateThrottleAndRevs the v1 table missed, because it
    # is stored as f8 next to the f4 blip constants.
    (0x00539260, "f8", 0.5, -1,
     "blip machine state-4 throttle release per tick (double)"),
    # sub_4982F0, the per-tick yaw integrator: below THIS speed (m/tick) the yaw
    # acceleration is derated by speed * (1/THIS). 0.49632 m/tick is 64 km/h at 36 Hz; left
    # unscaled at 144 it covers everything below 257 km/h and quarters the AI's steering
    # authority through normal corners.
    (0x0053A79C, "f4", 0.49632, -1,
     "yaw-authority low-speed threshold in sub_4982F0, m/tick"),
    (0x0053A7A0, "f4", 2.0148292, +1,
     "reciprocal of the yaw-authority threshold, 1/(m/tick)"),
    # sub_44C920: below THIS speed the AI's lateral goal is dead-reckoned (pos + vel*t)
    # instead of asked of the racing line. 0.37224 m/tick = 48 km/h at 36 Hz; unscaled at
    # 144 the dead-reckoning branch covers everything below 193 km/h.
    (0x0053929C, "f4", 0.37224, -1,
     "dead-reckoning speed threshold in sub_44C920, m/tick"),

    # THE CORNERING-PATH SWEEP (v5, after v4 got the bots to pace but crashing in
    # corners). These live in the AI's per-tick lateral/longitudinal control and the
    # fake-physics integrator; every address was xref-checked and every consumer's
    # arithmetic verified current-rate before listing. The +-0.011343 clamps seen in
    # the same code are reads of the PARAM TABLE slot (ini nominal_traction_circle,
    # already scaled by gplaiscale) and need nothing here.
    (0x0053A700, "f4", 0.62040001, -1,
     "line-speed cap in the fake-physics tick + the mod's limp-mode speed floor "
     "(sub_496AB0, sub_9839BA), m/tick"),
    (0x0053A704, "f4", -0.015124, -2,
     "fake-physics per-tick decel cap (sub_496AB0), m/tick^2"),
    (0x0053A70C, "f4", -0.0083181998, -2,
     "fake-physics per-tick decel cap 2 (sub_496AB0), m/tick^2"),
    (0x0053A710, "f4", -0.37224001, -1,
     "fake-physics dlat velocity clamp, negative side (sub_496AB0), m/tick"),
    (0x0053A714, "f4", 0.37224001, -1,
     "fake-physics dlat velocity clamp, positive side (sub_496AB0), m/tick"),
    (0x0053A724, "f4", 0.00062040001, -1,
     "zero-motion/park threshold in the position integrator (sub_496E40), m/tick"),
    (0x0053A768, "f4", 0.012408, -1,
     "'am I moving' gate in the lateral-accel builder (sub_497E00), m/tick"),
    (0x0053889C, "f4", 0.012408, -1,
     "'stopped' gate in the queue check sub_4478D0, m/tick"),
    (0x00538808, "f4", 0.062040001, -1,
     "'blocker is moving' threshold (sub_444410/sub_444A20/sub_4478D0), m/tick"),
    (0x0053A7A8, "f4", 2.8538401, -1,
     "following-horizon speed knee in the separation solver (sub_4985E0), m/tick"),
    (0x0053A7AC, "f4", -25.229164, +2,
     "following-horizon slope, ticks per (m/tick), same solver"),
    (0x005388B4, "f4", 0.62040001, -1,
     "blocker-width speed knee (sub_4481D0), m/tick"),
    (0x005388B8, "f4", 1.6118633, +1,
     "reciprocal of the blocker-width speed knee, 1/(m/tick)"),
    # THE LATERAL CEILING PAIR -- deliberately rate^-1, NOT the rate^-2 their units say.
    # sub_498080 computes the lateral-accel ceiling as car+236 * car+376 * THIS, where
    # car+376 in stock is the DIMENSIONLESS driver-quality product v12 -- but our
    # accessor conversion makes car+376 carry an extra 36/rate (right for its line-speed
    # role, poison here, same dual-use as the v13 fix). The ceiling needs a net /16 at
    # 144; car+376 already contributes /4, so the constant takes the remaining /4 =
    # rate^-1. If the accessor conversion is ever moved out of AdjCoeff, these two must
    # become rate^-2.
    (0x0053A794, "f4", 0.01100271, -1,
     "lateral-accel ceiling, on-line (sub_498080); see the rate^-1 note above"),
    (0x0053A790, "f4", 0.0079400996, -1,
     "lateral-accel ceiling, off-line (sub_498080); see the rate^-1 note above"),

    # THE AI's SLIPSTREAM RANGE (v27). sub_4987E0 runs once per qualifying car pair per
    # tick, from gpl_aiUpdateCarGaps, which is called at 0x44B7E0 -- after the entrant
    # loop closes and OUTSIDE the every-9-ticks gate, so there is no tick gate to trade
    # against. It decides how far back a tow reaches:
    #
    #     LIM = car+482^2 * 4.887682 + 4.3      compared against a longitudinal gap
    #
    # car+482 is the car's speed in m/tick AT THE CURRENT RATE, and the gap is in METRES
    # -- it is accumulated from car+982, whose producer sub_497DC0 builds it from the
    # dlong POSITION pair (car+502, car+518) that partners the m/tick velocity pair.
    # Three independent checks put the whole constant block in metres: the +-3.7224
    # clamp is 300 mph in m/tick@36; the AI node spacing is trackLength/3 = 3 m; and the
    # block contains 4.3 (car length), 2.15 and its exact reciprocal 0.465116, 1.9035
    # (car width) with its exact square 3.62331 and reciprocal 0.275991.
    #
    # metres = (m/tick)^2 * C forces C to carry tick^2/m, so rate^+2. x16 is an
    # exponent-field-only change, hence bit-exact with no rounding introduced.
    #
    # EXCLUSIVE: exactly one reference image-wide, at 0x00498877.
    #
    # AND IT IS PROVED BY MEASUREMENT, not only by dimensional argument. Papyrus shipped
    # this model TWICE. The AI copy reads this block; a PHYSICS/PLAYER copy at 0x0053AE5C
    # is read by sub_4AAF60 / sub_4AB090 with car+1272, which gpl_carUpdateDerivedState
    # writes in METRES PER SECOND (capped against 729000 = 90^3, a 90 m/s ceiling).
    # THIRTEEN constants are bit-identical across the two blocks -- 1.2, 0.15, -4.3, 2.15,
    # 1.9035, 4.3, 0.465116262, 3.62331223, 0.275990576, -0.15625, 0.125, -0.078125, 64.5.
    # The ONLY one that differs is this coefficient:
    #
    #     4.88768196 / 0.00371604948 = 1315.29 = 36.27^2
    #
    # The game measures its own unit ratio, and the answer is the tick rate. Both copies
    # give the same physical answer: 30.94 m against 30.55 m at 188 mph. That also settles
    # the neighbours -- a constant identical in an m/tick copy and an m/s copy cannot carry
    # rate, so every one of them is correctly left alone.
    #
    # THE COMMUNITY PATCH. GPL_Slipstream v3.0/v3.1 retunes eleven constants of this block
    # (this one 4.887682 -> 7.86351776, the gains -0.15625 -> -0.1161 and -0.078125 ->
    # -0.0387, the search radius 64.5 -> 70.8). Its PHYSICS change is a pure constant
    # retune of stock code -- GPL always had a slipstream, merely a weak one. (The patch
    # does insert code, three caves and eight trampolines, but none of it is physics: it is
    # a network version handshake, a UI gate, a GPLRank best-lap block, and for OTHER mods
    # a restoration of the stock falloff curve that SRMZ replaced.) It ships the SAME
    # slipstream constants at 36 and 60 fps, which is an omission
    # rather than a judgement: its 60 fps variant is simply brr's 60FPSv2 concatenated (its
    # readme credits him), and 60FPSv2 independently misses about 35 of 58 rate sites.
    # Its own package is self-inconsistent at 60 Hz for exactly this reason -- it scales
    # BOTH copies by the same factor, so the AI gets a 19.7 m tow at 188 mph while the
    # player gets 46.1 m.
    #
    # The two patches write the same address and GPLPatchDLL has no collision detection, so
    # whichever is applied later wins silently. This patch is 144-only and takes precedence.
    # ANYONE COMBINING THE TWO at 144 Hz wants the community value times the same 16:
    # 7.86351776 * 16 = 125.816284. The player-side twin at 0x0053AE68 must NOT be scaled
    # by the rate in either case.
    #
    # Unpatched at 144 the v^2 term collapses 16x and the tow reaches only ~5 m at any
    # speed instead of 10 m at 90 mph and 18 m at 134 mph. The products are subtracted
    # from car+482 alongside a wind term in sub_497BC0 (0x497C74/0x497C7A), i.e. they
    # reduce AIRSPEED, which then drives a drag model going as airspeed^2 -- so a lost
    # 14% of airspeed is a lost 26% of drag reduction in the AI's own performance model.
    # That is an AI that never sets up a slipstream pass.
    #
    # The +-3.7224 clamps at 0x53A7DC/E0 are formally rate^-1 and are LEFT ALONE: they
    # fire at 300 mph in stock units and are four times further from binding at 144 Hz,
    # the negative half can never fire at all because car+482 is an fsqrt result, and
    # they have 4 and 2 readers against this site's 1.
    (0x0053A7E4, "f4", 4.88768196105957, +2,
     "the AI's slipstream range coefficient, tick^2/m (sub_4987E0); LIM = v^2*C + 4.3 m"),

    # THE THIRD DIGITAL-STEERING RE-CENTRING RATE (v28). This one was missing from the
    # table for a reason that only standalone mode exposes: 77G scales it, so in the
    # companion build it was already correct and the omission was invisible. Its two
    # partners at 0x53AB98 and 0x53ABA0 have been in the table since v13, and the IDB note
    # on 0x53ABA0 even called this one "the partner of the 0x53ABA8 that 77G already
    # scaled" -- which is exactly the kind of dependency on a third party that going
    # standalone removes.
    #
    # EXCLUSIVE: one reference image-wide, `fdivr dword ptr [0053ABA8]` at 0x004A3033 in
    # gpl_controlsDigitalSteerUpdate. 77G writes -0.6685, which is stock x 0.250010 -- the
    # same rate^-1 its partners take.
    (0x0053ABA8, "f4", -2.67389512, -1,
     "digital-steering re-centring rate, the third branch (fdivr at 0x004A3033); 77G "
     "scaled this one and we inherited it, so standalone must emit it itself"),

    # THE AI'S POST-SHIFT HOLD-OFF AND THE RPM DECAY IT PAIRS WITH (v29). These two must
    # move TOGETHER or not at all; scaling either alone breaks the other's expression.
    #
    # 0x554DD0 is a TICK COUNT. gpl_aiComputeDlongSpeedGoal at 0x00497403 computes
    #
    #     holdTicks = (3 - 2*skill) * 18        -> car+0x24E, via __ftol
    #
    # and gpl_aiUpdateAllDrivers decrements car+0x24E once per tick at +0x1F7, after which
    # gpl_aiComputeDlongSpeedGoal reads it at +0x61 as a shift hold-off. 18..54 ticks is
    # 0.500..1.500 s at 36 Hz and 0.125..0.375 s at 144 -- the AI re-decides its shift four
    # times too eagerly. This is STOCK code that no XML touches, so the bug is in the
    # companion build too, not only in standalone.
    #
    # 0x5547B8 is the per-tick RPM decay, and the two multiply into a shared margin:
    # both the mod's new shift decision (sub_9873EB+0xC0) and its rewritten
    # gpl_autoShiftDownshiftTest (+0x126) evaluate `revLimit - 1.1 * decay * 18`, a 2475
    # RPM margin. rate^-1 on the decay and rate^+1 on the count leaves the product exactly
    # invariant: 125 * 18 = 2250 = 31.25 * 72.
    #
    # WHY 0x5547B8 WAS BLOCKED UNTIL NOW, and why the block was illusory. The IDB note at
    # 0x009874A5 said 125.0 must not be scaled because it has a reader at 0x004A37A2
    # "outside the AI", to be resolved first. Checking VANILLA gpl.exe settles it: there,
    # 0x5547B8 has exactly TWO readers, both `fsub` in gpl_aiUpdateThrottleAndRevs
    # (0x0044C241, 0x0044C255), a genuine per-tick RPM decay. The third reader exists only
    # because 60.xml REWROTE gpl_autoShiftDownshiftTest, and it uses the constant in the
    # same margin form as the AI's own new shift code. The "second use" is the mod's copy
    # of the first, so there is no conflict to resolve.
    (0x005547B8, "f4", 125.0, -1,
     "AI per-tick RPM decay; PAIRED with 0x00554DD0, the product must stay 2250"),
    (0x00554DD0, "f4", 18.0, +1,
     "AI post-shift hold-off base in TICKS, (3-2*skill)*18 -> car+0x24E; PAIRED with "
     "0x005547B8, the product must stay 2250"),
]

# THE ADJ-COEFF DUAL-USE FIX. gpl_aiComputeDriverSpeedParams builds
#
#     v12 = driver_mods x difficulty x class[614] x hype x AdjCoeff
#
# and uses it TWICE: this[92] = v12 is the multiplier every .lp line speed goes through
# (sub_445030 / sub_449650 return car+376 * car+1066 * raw), and v13 = 1 - v12 is a
# DIMENSIONLESS driver-quality deficit that sets the traction/accel scalings this[83..85]
# via (1 - K*v13). The on-read accessor conversion multiplies AdjCoeff by 36/rate, which
# is exactly right for the first use (the .lp files stay in m/tick@36 forever and this is
# the one conversion on that path) and poisons the second: at 144 Hz v12 ~ 0.25, v13 ~
# 0.75, and this[84] collapses to ~25% -- the AI loses three quarters of its traction
# budget. 60FPSv2 shipped the same defect at 0.6 strength.
#
# The fix un-poisons v13 ONLY: the 10 bytes at 0x455636 (`fsub [esp+4]` computing
# 1 - v12, plus the this[92] store) detour to a cave that stores this[92] = v12 as
# before, then computes 1 - v12*(rate/36) -- i.e. 1 minus the UNCONVERTED product.
# Everything downstream of either use is untouched: [esp+4] is rewritten with v13 at
# 0x455648 before its later reads.
V13_DETOUR_VA = 0x455636
V13_DETOUR_STOCK = bytes.fromhex("d8642404898670010000")
V13_RESUME_VA = 0x455640
# 16-aligned, inside modtext's RAW data (0x988913..0x988BFF is a verified zero tail in
# the stock exe), clear of every gpl67 XML write and of the abandoned 0x988F00 cave.
V13_CAVE_VA = 0x988920

# THE SHARED ONE-G CONSTANT, 0x53A758 = 0.0075619998 = 9.8/36^2, THE ONE REFERENCE SITE
# WITH READERS IN TWO UNIT WORLDS. Found 2026-09-04 after the v2 build still left the
# bots slow: the site's rate^-2 scaling comes from 60FPSv2, which targeted VANILLA, where
# the only readers are the slope/gravity terms of sub_497BC0 -- current-rate world,
# scaling correct. The 67 mod adds five more readers. Four of them also want the scaled
# value (three operate on current-rate accel fields; sub_986CAC multiplies by
# ticksPerSecond^2 to reach real m/s^2, and both factors being patched keeps 9.8). The
# fifth is mod_aiAeroCornerSpeedScale (0x9874E0), whose whole formula runs in m/tick@36
# .lp units and NEEDS the stock value: with G/16 its corner-speed ratio collapses ~4x,
# which re-quarters every line speed downstream of everything else the patch fixes.
#
# Fix: keep 0x53A758 scaled, plant the STOCK value at G36_CONST_VA (the tail of the v13
# cave) and repoint that single fld's operand at it. Every scaled site inherited from
# the reference was re-checked for mod-range readers after this: 0x53A758 is the only
# one shared with the mod.
G36_FLD_VA = 0x9874E0
G36_FLD_STOCK = bytes.fromhex("d90558a75300")   # fld dword ptr [0053A758]
G36_CONST_OFFSET = 0x1C                          # inside the v13 cave blob
ONE_G_36 = 0.0075619998

# THE REST OF THE MOD'S .LP-WORLD READS OF RATE-CARRYING GLOBALS (v4, from the minidump
# of the v3 in-race freeze). The v3 hang was NOT load or a threading race: restoring
# stock G into mod_aiAeroCornerSpeedScale made v10 = a9 - mass*G*0.000313 go NEGATIVE,
# because a9 is seeded AT RUNTIME from the ini's nominal_traction_circle -- which
# gplaiscale scales rate^-2. sqrt(negative) -> NaN -> the NaN reaches a .lp index via
# ftol -> wild pointer -> access violation in the fg thread -> the process deadlocks on
# the comm gates it was holding. Proven numerically from the dump: flt_5F8B14 =
# 0.000697125 (the scaled ini value), threshold mass*G36*0.000313 = 0.001449.
#
# The general disease: the mod's .lp-unit session-init and corner code read THREE
# rate-carrying globals seeded/patched to current-rate values -- gpl_ticksPerSecond
# (0x5391E4, 36 -> 144), flt_5F8B14 (the runtime copy of the scaled traction circle),
# and gpl_ai_ONE_G_perTickSq (handled above). Each read below sits in arithmetic whose
# other operands are m/tick@36 .lp data, so each must see the STOCK value. Reads of the
# same globals verified to sit in CURRENT-RATE arithmetic (sub_986E93's G*tps^2 = 9.8
# m/s^2, sub_9872F9, the stock exe's readers) are deliberately absent.
#
# Each entry redirects one 6-byte FPU memory operand to a stock-value copy planted in
# the cave tail. (va, stock bytes, const key, note).
TPS36_CONST_OFFSET = 0x20
TC36_CONST_OFFSET = 0x24
ONE_TC_36 = 0.011154   # [GP] nominal_traction_circle in the 36 Hz gpla67.ini. If a
                       # future carset changes it, regenerate with the new stock value.
MOD_LP_WORLD_REDIRECTS = [
    (0x986DA9, "d80d148b5f00", TC36_CONST_OFFSET,
     "a9 class figure = cardata*tc: seeding of flt_179CD78 in the class setup"),
    (0x987F6C, "d905148b5f00", TC36_CONST_OFFSET,
     "flt_A0FF00 = tc: seeding in the mod session init (sub_987DB2)"),
    (0x986E20, "d80de4915300", TPS36_CONST_OFFSET,
     "gearing top speed: dlongSpeedMaximum(m/tick@36) * tps -> m/s"),
    (0x986F65, "d80de4915300", TPS36_CONST_OFFSET,
     "gear ladder: min lp speed(m/tick@36) * tps -> m/s"),
    (0x987E3E, "d80de4915300", TPS36_CONST_OFFSET,
     "average lap speed from .lp records (fed flt_A0FF28; was 4x too big)"),
    (0x987BD8, "d80de4915300", TPS36_CONST_OFFSET,
     ".lp accel-feasibility pass: lp speed -> m/s (sub_987959)"),
    (0x987C33, "d905e4915300", TPS36_CONST_OFFSET,
     ".lp accel-feasibility pass: m/s^2 -> m/tick^2@36 divisor (sub_987959)"),
]

# THE .LP TARGET YAW RATE -- the last unconverted 36 Hz quantity on the AI's control
# path (v6, found by auditing the line object's vtable+0x30 slot, the one .lp accessor
# with no car+376 wrapper).
#
# A .lp record is 20 bytes: [dlong speed, dlat speed, dlat offset(m), YAW RATE, flags],
# the three motion fields per tick at 36 Hz, nodes 3 m apart. That field+12 is a RATE and
# not a curvature is measured, not inferred: integrating it over a lap as a yaw rate
# (dtheta = w * 3m / speed) closes to exactly one turn on all fifteen stock tracks
# (-0.976..-1.047, Targa +1.016 because it runs the other way), while reading it as a
# curvature (dtheta = k * 3m) scatters from -0.64 to -1.65.
#
# Every other per-tick .lp field reaches the AI through a wrapper that multiplies by
# car+376 (sub_445030 speed, sub_449650 dlat speed, sub_449690 dlat gradient), and
# car+376 carries the 36/rate accessor conversion -- which is why those paths are already
# right. The yaw rate is the exception: vtable slot 12 (sub_4496D0) hands sub_44A920's
# raw value straight to gpl_aiIntegrateYawPerTick, which feeds it to the spring as the
# TARGET YAW RATE against car+534, a rate in the CURRENT tick. At 144 Hz the AI is told
# to rotate four times faster than the corner needs and the damping term drives it there:
# the car yaws far more than its path curves, which is "bots slide through corners and
# crash" exactly.
#
# Fix: one fmul in the accessor's tail, written into its 8 bytes of alignment padding.
# Stock leaves this rate unscaled by driver quality, so the multiplier is a plain 36/rate
# constant and NOT car+376 -- using car+376 would also apply the difficulty product and
# change stock behaviour. The accessor's two other callers are unharmed: sub_445B60
# discards the result, and gpl_aiState_tryToOutbrake only tests its sign, which a
# positive scale preserves.
LP_YAWRATE_VA = 0x44A943
LP_YAWRATE_STOCK = bytes.fromhex("d944811cc3" + "90" * 6)
SPT36_CONST_OFFSET = 0x28    # 36/rate, in the cave tail

# THREE SITES THAT MUST MULTIPLY A VALUE BY 36/rate RATHER THAN REPLACE IT (v21).
#
# All three carry a speed in m/tick@36 into current-rate arithmetic, and in two of them the
# value comes from a track's own `tracks/<name>/track.ini`, which NO tool in this campaign
# reads. That is what forces a code patch: scaling the .rdata default would fix the tracks
# that do NOT override and silently discard the value from the ones that do -- monaco for
# pit_stall_approach_speed and zandvort for track_start_speed_limit are exactly those.
# A stub that MULTIPLIES whatever the ini loaded is correct for every track, present and
# future. The multiplier already exists at V13_CAVE_VA + SPT36_CONST_OFFSET.
#
#   0x445650  gpl_aiApproachPitStall_getSpeedGoal -- 0.18612 m/tick is 15 mph at 36 Hz and
#             60 mph unscaled at 144. `fld [desc]` becomes `call stub` + nop.
#   0x445672  gpl_aiLeavingPits_getSpeedGoal -- returns a RAW .lp pit-line speed with no
#             car+376 conversion, so a bot leaves the pits targeting 4x the line speed. The
#             patch goes AFTER the call, not on it: gpl_aiLpDlongSpeedAtDist is cdecl with
#             stack args, so a call-trampoline would shift them by one return address and
#             hand it the wrong values. Jumping in after the call leaves esp untouched.
#   0x442E56  gpl_aiState_startOfRace_tick -- NOT an FPU site: it is `mov eax, ds:[desc]`
#             then `mov [esi+426h], eax`, a raw dword copy of track_start_speed_limit into
#             the car. The stub does the whole copy through the FPU instead. The 999999.9
#             no-limit default stays effectively infinite either way; zandvort's 1.231
#             m/tick (99 mph) is the value this rescues, and unscaled it means 397 mph,
#             i.e. the start limit is simply switched off.
LP_MUL_CAVE_VA = 0x00988A20
LP_MUL_CAVE_SPAN = 0x50

# ======================================================================================
# THE FORCE-FEEDBACK SMOOTHING FILTER (v32). The only rate coupling on the FFB path, and
# the reason it lives here rather than in gplffb.py with the two FFB PREFERENCES: it is a
# rate fix, it belongs to every build at every rate, and a patch that ships without it
# leaves the wheel silently wrong for the rate it runs at.
#
#     0x4A4389  d8 05 2CAE7800   fadd  ds:gpl_ffb_smoothedForce_IIR_STATE
#     0x4A438F  d8 0d 2CAB5300   fmul  ds:gpl_half_SHARED_3_READERS      <- alpha
#     0x4A4395  d9 15 2CAE7800   fst   ds:gpl_ffb_smoothedForce_IIR_STATE
#
# A one-pole IIR applied once per CALL, so its time constant is in SAMPLES rather than
# seconds: about 56 ms of smoothing at 36 Hz and 14 ms at 144. gpl_ffbUpdate runs once per
# TICK while its input, gpl_ffbAverageAndEmit, runs inside the substep loop at a fixed
# 288 Hz -- so this filter is the only part of the chain that moves with the tick rate.
#
# THE CONSTANT CANNOT BE EDITED IN PLACE. 0x53AB2C has three readers in three unit worlds:
# this filter pole, an arithmetic mean of two axes in gpl_ffbFillControlState (0x4A2209),
# and `(int)(taskGetRate() * 0.5)` -- half a SECOND -- in gpl_controlsReadAxes (0x4A2F3F).
#
# THE DC-GAIN TRAP, and why a single-constant fix is WRONG. The stock form is
# `x = (a + x) * 0.5` -- ONE constant serving BOTH taps -- whose steady-state gain is
# 0.5/(1-0.5) = 1, i.e. the filter tracks its input exactly. Substituting the
# equal-time-constant alpha into that shared constant gives a steady state of
# 0.1591/(1-0.1591) = 0.189: the wheel would collapse to 19% of the physics' output. A
# first attempt at this patch did exactly that and was never installed. A one-pole with a
# preserved time constant needs TWO constants, so this is a cave.
#
# The replacement is NOT a rate^k. Preserving the wall-clock time constant needs
# alpha' = 1 - (1 - alpha)^(36/rate) = 0.1591 at 144 Hz, where a naive alpha/4 would give
# 0.125 -- a 27% error, because the relationship is exponential, not linear.
#
# "Correct" here means "matches 36 Hz stock", which is MORE smoothing than an unpatched
# 144 Hz build gives. A sharper wheel is a legitimate preference; it is now a matter of
# editing this table rather than of omitting a switch, because the default has to be the
# rate-correct one.
FFB_IIR_BLOCK_VA = 0x004A4389
FFB_IIR_BLOCK_STOCK = bytes.fromhex("d8052cae7800" "d80d2cab5300" "d9152cae7800")
FFB_IIR_STATE_VA = 0x0078AE2C
FFB_IIR_ALPHA_STOCK = 0.5
# In modtext's verified zero tail, between the .lp-leak stubs (ending 0x9889C0) and the
# pit-lane cave at 0x988A20.
FFB_IIR_ALPHA_VA = 0x00988A00
FFB_IIR_ONE_MINUS_VA = 0x00988A04
FFB_IIR_STUB_VA = 0x00988A80


# ======================================================================================
# THE GPLRANK BEST-LAP BLOCK (v31). NOT A RATE FIX -- a deliberate etiquette choice, and
# the one part of this patch that changes what the game RECORDS rather than how it runs.
# It is emitted as one clearly-labelled group so it is easy to find and easy to drop.
#
# GPLRank ranks drivers by comparing saved best laps. A 144 Hz build is not the game those
# rankings were set on, so uploading its times mixes two populations. Lee Bowden blocked
# the writes in 77G for that reason, and his FAQ says so outright: "Saving of Best Laps is
# purposely disabled". This restores that decision.
#
# gpl_statsRecordBestLap(trackName, carIndex, sessionType, lapSeconds) keeps a table of
# best times at [esi + 0x136C5D8] and a parallel table of unix timestamps. Blocking both
# stores leaves the table holding whatever it was loaded with, so nothing new reaches the
# stats file. Everything else still works: the in-race timing display, the session lap
# list, and NORMALISED PLAYER TIME, which is learned from session lap times directly and
# never reads this table.
#
# ONE CORRECTION TO 77G, and it matters. 77G nops out all six bytes of the fstp. But
# `fld [ebp+arg_C]` at 0x411A78 has already PUSHED the lap time, and the fstp at 0x411A7D
# is what pops it -- so plain nops LEAK ONE x87 REGISTER PER NEW BEST LAP, and the x87
# stack is eight deep. This emits `fstp st(0)` (DD D8) instead, which pops and discards,
# then pads. Same effect, no leak. It is the same defect class as the surviving fdivr that
# D1 left behind.
#
# The second store is an integer timestamp and needs no such care.
#
# (address, stock bytes, replacement, note)
BEST_LAP_BLOCK_SITES = [
    (0x00411A7D, bytes.fromhex("d99ed8c53601"),
     bytes.fromhex("ddd8") + b"\x90" * 4,
     "do not store the new best lap time; fstp st(0) pops the value fld pushed at "
     "0x00411A78, which plain nops would leak"),
    (0x00411A89, bytes.fromhex("898678051f01"), b"\x90" * 6,
     "do not store the matching unix timestamp"),
]

# THE LAST THREE .lp LEAKS (v30), D, E and F. All three mix a .lp quantity -- m/tick at
# 36 Hz forever -- with a live-sim one at the current rate, and all three are fixed at the
# single consumer that needs the conversion rather than at the source, because in every
# case the source has other readers that need it left alone.
#
# The multiplier each stub needs already exists in the v13 cave tail: rate/36 at +0x18 and
# 36/rate at SPT36_CONST_OFFSET. No new constant.
LP_LEAK_CAVE_VA = 0x00988970    # 0x988970..0x9889FF, zero in the donor and in every build;
LP_LEAK_CAVE_SPAN = 0x90        # the dlat stubs end at 0x98896F and the next cave is 0x988A00
LP_LEAK_STUB_STRIDE = 0x20

# THE PARTICLE OPERAND RELOCATIONS (v23). Four constants that are genuinely per-tick in
# gpl_particleUpdate_perTick and its type-8 spawner, but whose .rdata slots are SHARED with
# consumers in other units -- spawn positions in metres, a dimensionless divisor, and in one
# case the particle renderer's billboard-quad corner. Patching a slot would deform sprites
# or move spawn points, so each reader's disp32 operand is repointed at a private slot.
#
# (slot offset, kind, stock value, note). "geom" is k^(36/rate); the rest are stock*scale^k.
# THE NET/REPLAY EXTRAPOLATION FIXES (v24). gpl_netCarExtrapolateRecord runs 20 times per
# tick from gpl_simStep, but the block these sites live in is entered only for records
# stamped by gpl_netCarRecordStampFromNet or gpl_netCarRecordStampFromReplay -- remote cars
# ONLINE, and every car during REPLAY PLAYBACK. In live single-player driving
# gpl_netCarRecordStampLocalSim re-stamps every record each tick and this code is skipped.
# It is fixed anyway because the 144 patch is a standalone artefact that other people run
# in multiplayer and use to record replays.
#
# THE +-36-TICK WINDOWS. `cmp esi, -36` is `83 FE imm8`, and imm8 is SIGN-EXTENDED to
# [-128, 127], so 144 cannot be written into it at any cost. But each compare is followed
# by a SIX-byte `0F 8x rel32`, giving 9 contiguous bytes -- room for a 5-byte jump into a
# cave that does the widened `81 FE imm32`, re-issues the branch to its original target,
# and jumps back to the fall-through.
NET_CAVE_VA = 0x00988AC0
NET_CAVE_SPAN = 0xB0          # to 0x988B70, where Lee Bowden's 77G caves begin
NET_WINDOW_SITES = [
    (0x0049940B, "the local player's own record: age vs -36 ticks"),
    (0x00499414, "the local player's own record: age vs +36 ticks"),
    (0x004995CF, "the remote/replay record: age vs -36 ticks"),
    (0x004995D8, "the remote/replay record: age vs +36 ticks"),
]

# THE rec+72 GROUND-CLEARANCE FILTER. `rec[72] = (a + rec[72]) * 0.5` with ONE constant on
# both taps, so its DC gain is 0.5/(1-0.5) = 1. Substituting alpha alone would drop that to
# 0.189; the fix needs alpha and 1-alpha, exactly like the FFB filter at 0x4A438F.
#
# The 20 stock bytes interleave `mov eax,[ebx+290h]; test eax,eax` between the fadd and the
# fmul, and the flags feed a `jz` at 0x4999F4 -- so the replacement must end with that test.
NET_IIR_VA = 0x004999E0
NET_IIR_STOCK = bytes.fromhex("d84348" "8b8390020000" "85c0" "d80d2ca85300" "d95b48")
NET_IIR_ALPHA_STOCK = 0.5

# A SHARED SLOT WHOSE MAJORITY OF READERS WANT THE SCALED VALUE AND WHOSE MINORITY WANTS
# THE STOCK ONE (v25). Same shape as MOD_LP_WORLD_REDIRECTS, but those all redirect to the
# v13 cave tail, which is full at exactly 48 bytes. This is its own small slab.
#
# 0x0053AD4C is the ambient sound level's per-tick FALL step, and it has FIVE readers, not
# the one the v19 audit recorded:
#
#   0x4A7BC1  gpl_ambientSoundLevelUpdate   per-tick audio ramp          rate^-1
#   0x983A06  mod_aiLimpSpeedFloor          car+482 - 0.005, m/tick      rate^-1
#   0x983A7C  sub_983A40                    same speed margin            rate^-1
#   0x983AAB  sub_983A40                    "am I moving" gate, m/tick   rate^-1
#   0x988044  mod_aiSessionLpInit           DIMENSIONLESS                rate^0
#
# The odd one out is doing:
#
#   0x988041  fild  [ebp-14h]        ; an unsigned byte out of the .lp node record
#   0x988044  fmul  [0053AD4C]       ; * 0.005
#   0x98804A  fadd  [00539790]       ; + 1.0
#   0x988050  fmulp st(1), st        ; |v| *= (1 + n * 0.005)
#   0x988052  fcom  [00A0FF2C]       ; running minimum, mod_ai_figMinLpSpeed36
#
# A constant that is multiplied by an integer and ADDED TO 1.0 is dimensionless, whatever
# the integer turns out to be -- that argument settles the units without identifying n.
# Scaling it rate^-1 shrinks the bonus fourfold. Measured over the fifteen stock race.lp
# files the flag byte is 54..62 or 181..191, so the boost is x1.27..1.95 stock and only
# x1.07..1.24 as patched, leaving mod_ai_figMinLpSpeed36 roughly 15..35% low. Its consumer
# is mod_aiClassGearingSetup at 0x986F5F, whose very next instruction is one of this
# patch's own .lp-world redirects -- so it sets the AI's lowest gear ratio on every track.
#
# The four per-tick readers genuinely want the scaled slot, so the slot stays scaled and
# this one reader is moved to a private stock copy.
#
# (instruction VA, opcode bytes, the slot it currently reads, stock value, slot offset,
#  note). The displacement patched is always insn_va + len(opcode).
STOCK_COPY_CAVE_VA = 0x00988A70   # zero in the stock exe; 0x988A20+0x50 ends here and the
STOCK_COPY_CAVE_SPAN = 0x10       # FFB cave does not start until 0x988A80
STOCK_COPY_REDIRECTS = [
    (0x00988044, b"\xd8\x0d", 0x0053AD4C, 0.005, 0x00,
     "mod_aiSessionLpInit's .lp-world reader of the ambient fall-step slot: the constant "
     "is multiplied by a node byte and added to 1.0, so it is dimensionless and must NOT "
     "carry the rate; the slot's four other readers are per-tick and keep rate^-1"),
]

# ---------------------------------------------------------------------------------------
# STANDALONE MODE (v28, in progress). Emit the sites the 77G companion currently supplies,
# so the result is one self-contained XML that needs no third-party patch.
#
# WHY. Every defect found on 2026-09-08 was in 77G rather than in our own work: it writes
# gpl_tickPeriodSeconds's replacement to an address eight bytes low and kills the global
# for the whole run; it reads dword_94D25C instead of dword_94D264 in its taskBegin cave;
# it freezes the sound interpolator's steps-per-tick at 4.0 when its own build makes it 2;
# and it rounds the substep constants to four decimals. We cannot test what we do not
# generate, and a user cannot be expected to find and install a patch that may vanish.
#
# All four cease to exist here by construction. This mode derives the tick period from the
# rate the stock code already computes (so the fstp at 0x452BE8 is never touched), leaves
# the sound interpolator accessor alone (so R = sndRate/simRate is computed, not frozen),
# and emits NOTHING for the substep constants -- at 144 Hz with N = 2 the physics rate is
# 288, exactly stock, so the stock bytes are already the exact reciprocals.
#
# THE THREE TASK REGISTRATIONS. Each pushes a rate as `6A ib`, and every rate above 63
# overflows a sign-extended imm8, so each needs `68 imm32` in a cave. The push is not the
# first instruction of its own block in any of the three, so the detour starts earlier and
# the stub replays what it displaced.
#
#   0x0040BCA8  push 36  ; push gpl_svTask   -> the SIM rate
#   0x004E842C  push 72  ; taskBegin         -> the TIMER rate, 2x sim
#   0x004E71B6  push 72  ; gpl_soundTaskRegister -> the SOUND callback rate, 2x sim
#
# The sound rate matters more than it looks. gpl_soundSchedulerUpdate_perTick divides by
# sndRate/simRate to get a per-callback delta, so getting it wrong is not a cosmetic
# mistiming: leaving 72 against a 144 Hz sim gives a ratio of 0.5, a per-step delta twice
# the whole remaining error, and a recurrence x[n+1] = 4T - 3x[n] whose gain is -3. That
# diverges, and every moving sound slams between its clamp rails. 77G dodged it by
# freezing the ratio; scaling the rate properly removes the need.
STANDALONE_CAVE_VA = 0x00988B70   # free ONLY here: this is where 77G's own caves start,
STANDALONE_CAVE_SPAN = 0x60       # and in standalone mode 77G is not applied

# (detour VA, stock bytes it replaces, resume VA, stub-builder). Each builder takes the
# integer to push and returns the stub body without its trailing jmp.
STANDALONE_TASK_SITES = [
    (0x0040BCA8, bytes.fromhex("6a24" "6840ab4600"), 0x0040BCAF, "sim", 1,
     lambda v: b"\x68" + struct.pack("<I", v) + bytes.fromhex("6840ab4600"),
     "gpl_svTask registration: the SIM rate"),
    (0x004E8426, bytes.fromhex("8b1564d29400" "6a48"), 0x004E842E, "timer", 2,
     lambda v: bytes.fromhex("8b1564d29400") + b"\x68" + struct.pack("<I", v),
     "taskBegin: the TIMER rate, 2x sim. 77G's cave reads dword_94D25C here, eight low"),
    (0x004E71B1, bytes.fromhex("68d0714e00" "6a48"), 0x004E71B8, "sound", 2,
     lambda v: bytes.fromhex("68d0714e00") + b"\x68" + struct.pack("<I", v),
     "gpl_soundTaskRegister: the SOUND callback rate, 2x sim"),
]

# STANDALONE, part 2: the two remaining things the companion supplies.
#
# (a) THE REPLAY CHUNK VERSION STAMP. gpl_replaySave writes a `RPLY` chunk whose version
# field is `2 | (rate << 8)` -- the frame rate lives in the second byte. Nothing in the
# game validates it (gpl_replayLoad stores whatever it finds and only 0x004930F1 reads it
# back, to preserve the original stamp on re-save), so this is a cooperative tag for
# external tools: a replay is frame-indexed and gpl_replayPeekHeader divides its length by
# taskGetRate(), so a tool must know the recording rate to interpret it. 60FPSv2 writes
# three bytes here and 77G writes one; they produce the same imm32.
STANDALONE_REPLAY_VERSION_VA = 0x004930F9
STANDALONE_REPLAY_VERSION_STOCK = bytes.fromhex("02000000")

# (b) THE MULTIPLAYER FRAME-RATE HANDSHAKE. Not a timer fix, which is what it looks like:
# it is a client/server patch-level check. gpl_driverRecordInit stamps a 32-bit signature
# into the join record, and gpl_netJoinQualifyDriver -- the function that emits "you are
# not qualified to drive in this race" -- compares it against the server's own before the
# password, laptime and rating checks. Mixing frame rates online desynchronises, so a
# 144 Hz build must refuse 36 and 60 Hz clients and be refused by them.
#
# 60FPSv2 derives its signature by READING ITS OWN CODE at 0x004E842C (`push 2*rate`), so
# the fingerprint is a hash of how taskBegin was patched. We cannot reuse that: standalone
# mode replaces those very bytes with a jmp into our own cave, which would make the
# signature a property of our cave's displacement. We stamp an explicit constant instead,
# derived from the rate so that two builds at different rates still reject each other.
#
# Both halves must ship together. Writer without checker leaves the server accepting 36 Hz
# clients; checker without writer makes the server reject every client including copies of
# itself.
#
# The five-hop jump chain is not obfuscation: the free inter-function alignment gaps are
# 6/15/10/4/4 bytes and two of them hold nothing larger than a `jmp rel8`.
STANDALONE_MP_SIG_BASE = 0x47500000        # 'GP' + the rate in the low 16 bits
STANDALONE_MP_WRITER_VA = 0x004104BD       # the record ctor's `pop edi; pop esi; retn`
STANDALONE_MP_WRITER_STOCK = bytes.fromhex("5f5ec3" + "90" * 14)
STANDALONE_MP_CHECK_VA = 0x0045DBD2        # inside the 15-byte gap before sub_45DBE0
STANDALONE_MP_ENTRY_VA = 0x0045D51A        # the 6-byte gap before gpl_netJoinQualifyDriver
STANDALONE_MP_ACCEPT_VA = 0x0045D54D       # resume the stock password/laptime/rating checks
STANDALONE_MP_REJECT_VA = 0x0045D5B2       # the "not qualified" path, message id 20014
# (VA, stock bytes, note) for the three sites that are pure jumps through the gaps.
STANDALONE_MP_HOPS = [
    (0x0045DB7C, bytes.fromhex("90909090"), 0x0045DB3C, "hop 1"),
    (0x0045DB3C, bytes.fromhex("90909090"), 0x0045DAF6, "hop 2"),
]

# STANDALONE, part 3: the GUI event throttle, as our own cave rather than a rewritten copy
# of somebody else's.
#
# GPL's whole GUI runs on the task rate, so at 144 Hz a HELD control -- the setup screen's
# arrows, spinners, list scrolling -- repeats four times too fast. 77G fixes that by
# counting callbacks in a global and delivering only every fourth, which fixes held
# controls and breaks single clicks: the counter is free-running, so a discrete click lands
# on a dropped phase three times out of four and the button has to be pressed again.
#
# Throttle on (id, time) instead of on a bare count. A held control is a dense run of the
# SAME widget id, one callback per frame; a discrete click is one callback, and consecutive
# callbacks in a notification burst carry DIFFERENT ids:
#
#     deliver if  id != lastId          (a different widget: always)
#             or  now - lastTick >= GATE (same widget, enough time has passed)
#
# `now` is gpl's own scheduler tick counter, which runs in menus too, so GATE = rate/36 is
# exactly the 36 Hz cadence: held arrows feel stock, every click lands, and repeatedly
# CLICKING one arrow works because human clicks are ~14 ticks apart.
#
# Two things this gets right that the 77G-copy route could not. Every address is written
# LITERALLY into the cave body: GPLPatchDLL's +8 rebase applies to <VirtualAddress> and to
# <Relocation> targets but never to bytes inside <Data>, so a literal operand is exact
# while a relocation to an original section resolves eight bytes high -- the same trap that
# destroyed gpl_tickPeriodSeconds. And because we own the whole file, this is an ordinary
# patch site rather than a surgical edit of a third party's cave.
#
# State lives in two moddata dwords, both zero and unreferenced in the stock image, and
# both inside stock moddata's raw data (0x00A0D250 - 0x00A08000 = 0x5250 < 0x8A00), so NO
# section growth is required.
STANDALONE_GUI_CAVE_VA = 0x00988BA0     # clear of the task stubs, which end by 0x00988B9E
STANDALONE_GUI_CAVE_SPAN = 0x60
STANDALONE_GUI_DETOUR_VA = 0x004EEF55
STANDALONE_GUI_DETOUR_STOCK = bytes.fromhex("8b44241050")   # mov eax,[esp+10h]; push eax
STANDALONE_GUI_RESUME_VA = 0x004EEF5A
STANDALONE_GUI_TICKS_VA = 0x00581EC0    # getIntCount()'s global; task_serviceInterrupt bumps it
STANDALONE_GUI_LASTTICK_VA = 0x00A0D250
STANDALONE_GUI_LASTID_VA = 0x00A0D254


def _standalone_gui_cave(cave_va, gate):
    """The same-id throttle. Returns the cave body; entry is a jmp from the detour.

    esp still holds the dispatcher's entry esp (E) on entry, so the widget id is at E+0x10;
    after the two pushes it is at esp+0x18.
    """
    if not 1 <= gate <= 127:
        raise SystemExit("REFUSED: the GUI gate must be 1..127 ticks (got %r)" % gate)
    tick = b"\xa1" + struct.pack("<I", STANDALONE_GUI_TICKS_VA)      # mov eax, [tickCount]
    head = (b"\x50\x51"                                              # push eax; push ecx
            + bytes.fromhex("8b4c2418")                              # mov ecx,[esp+18h]
            + bytes.fromhex("83f90a"))                               # cmp ecx, 10
    same = (b"\xa1" + struct.pack("<I", STANDALONE_GUI_LASTID_VA)    # mov eax,[lastId]
            + b"\x3b\xc1")                                           # cmp eax, ecx
    aged = (tick
            + b"\x2b\x05" + struct.pack("<I", STANDALONE_GUI_LASTTICK_VA)   # sub eax,[lastTick]
            + b"\x83\xf8" + bytes([gate]))                           # cmp eax, GATE
    accept = (b"\x89\x0d" + struct.pack("<I", STANDALONE_GUI_LASTID_VA)     # mov [lastId],ecx
              + tick
              + b"\xa3" + struct.pack("<I", STANDALONE_GUI_LASTTICK_VA))    # mov [lastTick],eax
    tail = (b"\x59\x58"                                              # pop ecx; pop eax
            + bytes.fromhex("8b442410")                              # mov eax,[esp+10h]
            + b"\x50")                                               # push eax
    drop = b"\x59\x58" + bytes.fromhex("c21400")                     # pop; pop; retn 14h

    # Lay it out, then back-fill the three short branches now that every offset is known.
    off_jbe = len(head)                       # jbe -> pass
    off_jne = off_jbe + 2 + len(same)         # jne -> accept
    off_jl = off_jne + 2 + len(aged)          # jl  -> drop
    off_accept = off_jl + 2
    off_pass = off_accept + len(accept)
    off_jmp = off_pass + len(tail)
    off_drop = off_jmp + 5
    body = (head
            + b"\x76" + struct.pack("<b", off_pass - (off_jbe + 2))
            + same
            + b"\x75" + struct.pack("<b", off_accept - (off_jne + 2))
            + aged
            + b"\x7c" + struct.pack("<b", off_drop - (off_jl + 2))
            + accept + tail
            + b"\xe9" + struct.pack("<i", STANDALONE_GUI_RESUME_VA
                                    - (cave_va + off_jmp + 5))
            + drop)
    assert len(body) == off_drop + len(drop)
    return body


PARTICLE_CAVE_VA = 0x00988AA0
PARTICLE_CAVE_SPAN = 0x20
PARTICLE_SLOTS = [
    (0x00, "geom", 0.9,  "streak/colour decay per tick; 0.9^(36/rate), NOT 0.9*36/rate, "
                         "which would be 0.225 -- near-total stopping, not a slow fade"),
    (0x04, -1,     0.05, "type-8 spawn VELOCITY in units/tick: the integrator at 0x47CADC "
                         "does pos += vel with no dt"),
    (0x08, -1,    -1.0,  "the constant term of the per-tick alpha decrement"),
    (0x0C, +1,     0.1,  "the jitter MAGNITUDE, scaled UP so that quartering the event "
                         "rate leaves the per-event amplitude distribution unchanged"),
]
# (disp32 VA, instruction VA, opcode bytes, old target, slot offset)
PARTICLE_REDIRECTS = [
    (0x0047CB81, 0x0047CB7F, b"\xd8\x0d", 0x00539D5C, 0x00),
    (0x0047CB8D, 0x0047CB8B, b"\xd8\x0d", 0x00539D5C, 0x00),
    (0x0047CB99, 0x0047CB97, b"\xd8\x0d", 0x00539D5C, 0x00),
    (0x0047C630, 0x0047C62E, b"\xd8\x0d", 0x00539D94, 0x04),
    (0x0047C645, 0x0047C643, b"\xd8\x0d", 0x00539D94, 0x04),
    (0x0047CA83, 0x0047CA81, b"\xd8\x25", 0x00539D9C, 0x08),
    (0x0047CA47, 0x0047CA45, b"\xd8\x0d", 0x00539D90, 0x0C),
]

# THE TYPE-4 SCRUB-PUFF LIFETIME (v23). `((counter mod 15) >> 1) + 1` ticks, range 1..8, a
# wall-clock duration that wants 4..32 at 144 Hz. Every in-place lever was ruled out: the
# 15 also produces the texture variant, and `shr edx,1` is only TWO bytes (D1 EA), so there
# is no room for a multiply and not even room for a jmp.
#
# The fix needs no cave at all. The lifetime is stored as a FLOAT (`fild var_C` then
# `fstp [particle+4]`), and the 79-byte store block reloads `[ebp+var_4]` SIX times while
# ebx sits unused -- loaded at 0x47C543, last read at 0x47C553, popped at 0x47C5DB.
# Caching the particle pointer in ebx frees 11 bytes, and an `fmul m32` costs 6. The scale
# factor lives in the 9 bytes of 0x90 alignment padding after the function's retn.
#
# particle+4 is a UNION -- alpha for types 1/2/16 (written at 0x47C8C2), lifetime for
# type 4 (written here) -- so this write site is type-exclusive and the multiply cannot
# leak into the alpha path.
TYPE4_BLOCK_VA = 0x0047C58C
TYPE4_SCALE_VA = 0x0047C5E7
TYPE4_STOCK = bytes.fromhex(
    "8b4dfc" "d94510" "8d411c" "8b4d08" "8b11" "8910" "8b5104" "895004" "8b4908" "894808"
    "8b55fc" "c7421804000000" "8b45fc" "c7401400000000" "8b4dfc" "d9590c" "8b55fc"
    "a174c77400" "db45f4" "894210" "8b4dfc" "b801000000" "d95904")


def _type4_block():
    return (bytes.fromhex(
        "8b5dfc"            # mov  ebx, [ebp+var_4]      -- cache the particle pointer
        "d94510"            # fld  [ebp+arg_8]
        "8d431c"            # lea  eax, [ebx+1Ch]
        "8b4d08" "8b11" "8910" "8b5104" "895004" "8b4908" "894808"
        "c7431804000000"    # mov  [ebx+18h], 4
        "c7431400000000"    # mov  [ebx+14h], 0
        "d95b0c"            # fstp [ebx+0Ch]
        "a174c77400"        # mov  eax, dword_74C774
        "894310"            # mov  [ebx+10h], eax       -- texture variant, untouched
        "db45f4")           # fild [ebp+var_C]          -- the 1..8 tick count
        + b"\xd8\x0d" + struct.pack("<I", TYPE4_SCALE_VA)   # fmul dword [rate/36]
        + bytes.fromhex(
        "d95b04"            # fstp [ebx+4]              -- 4..32 ticks at 144 Hz
        "b801000000")       # mov  eax, 1               -- the return value
        + b"\x90" * 9)


def _lp_leak_stubs(rate_over_36, spt36):
    """(site VA, stock bytes, resume VA, stub body, note) for .lp leaks D, E and F.

    `rate_over_36` and `spt36` are the addresses of the two conversion floats in the v13
    cave tail. Each stub ends with a jump the caller appends, so the body here is only the
    arithmetic plus whatever stock instruction it replays.
    """
    mul_up = b"\xd8\x0d" + struct.pack("<I", rate_over_36)   # fmul dword ptr [rate/36]
    mul_dn = b"\xd8\x0d" + struct.pack("<I", spt36)          # fmul dword ptr [36/rate]
    return [
        # D. gpl_aiEvaluateBlocker's straightaway-pass separation. arg_4 is
        # car+510 / gpl_aiLpDlongSpeed(...) -- a live m/tick over a .lp m/tick@36, so it
        # carries 36/rate. The 2.0 is a correct dimensionless factor and the descriptor
        # float it multiplies really does mean 10 metres, so neither moves: the stub
        # converts arg_4 back to dimensionless and re-reads 2.0 in place with `fsubr`.
        # Stock (2 - r)*10 spans 8..20 m; at 144 unpatched it is 17.0..20 m, a gate 1.75x
        # too wide with the blocker-speed modulation squashed 4x.
        (0x00446B1F, bytes.fromhex("d905d4875300d8650c"), 0x00446B28,
         bytes.fromhex("d9450c") + mul_up + bytes.fromhex("d82dd4875300"),
         "leak D: straightaway_pass_dlong_sep gate, arg_4 reconverted before 2.0 - r"),
        # E. The blocker-classification decel test. The dividend is a .lp speed delta in
        # m/tick@36 and the divisor is a lookahead in CURRENT-rate ticks, while the right
        # side of the compare is an acceleration that divides by the same lookahead
        # SQUARED. So the left side was short exactly one factor of the rate and the test
        # fired 4x too readily -- bots declaring a blocker whenever the line ahead demands
        # more than a quarter of the deceleration it should.
        (0x00446BEC, bytes.fromhex("d982a40b0000"), 0x00446BF2,
         bytes.fromhex("d982a40b0000") + mul_up,
         "leak E: blocker decel test, the lookahead divisor scaled to match the RHS"),
        # F. The mod's per-segment observed-speed table is SEEDED from a raw .lp speed and
        # then filtered towards the driver's own current-rate segment speed, so the seed is
        # 4x high at 144. It decays per LAP with alpha 0.25 -- half-life 2.41 laps -- and
        # is re-seeded every session, so a short race never leaves the biased window.
        (0x0098138A, bytes.fromhex("d901c1e20b"), 0x0098138F,
         bytes.fromhex("d901") + mul_dn + bytes.fromhex("c1e20b"),
         "leak F: mod per-segment .lp speed seed, converted to the current rate"),
    ]


def _lp_mul_stubs(spt36):
    """(call-site VA, stock bytes, replacement, stub bytes, note) for each site.

    `spt36` is the address of the 36/rate float in the v13 cave.
    """
    mul = b"\xd8\x0d" + struct.pack("<I", spt36)          # fmul dword ptr [36/rate]
    return [
        (0x00445650, bytes.fromhex("d905084f5500"), "call",
         bytes.fromhex("d905084f5500") + mul + b"\xc3",
         "approaching_pit_stall's dlong speed goal, [ai_track] pit_stall_approach_speed"),
        (0x00445672, bytes.fromhex("8b54241483"), "jmp",
         bytes.fromhex("8b54241483c408") + mul + bytes.fromhex("d91ac20c00"),
         "leaving_pits' dlong speed goal, a raw .lp pit-line speed"),
        (0x00442E56, bytes.fromhex("a16057550089862604 0000".replace(" ", "")), "call",
         bytes.fromhex("d905605755 00".replace(" ", "")) + mul
         + bytes.fromhex("d99e26040000") + b"\xc3",
         "start-of-race speed limit, [ai_track] track_start_speed_limit"),
    ]

# THE MARSHALS' WHITE FLAG -- the .lp-world leak outside the AI (v7). Same disease as
# MOD_LP_WORLD_REDIRECTS, but in STOCK code, which is why it survived every AI audit.
#
# sub_458EB0 decides each entrant's flag word (car+0x2C: bit0 chequered, bit1 WHITE,
# bit2 green, bit3 yellow, bit4 red, bit5 blue, bit8 full-course yellow). The white
# flag -- "slow vehicle on course" -- is this test at 0x459130:
#
#     gpl_aiLpDlongSpeedAtDist(0, carTrackPos) * dword_5FD1B4 * 0.5  >  car+0x44
#
# i.e. "is the car slower than half the racing line's speed here". The left side is a
# .lp dlong speed, m/tick at 36 Hz FOREVER; dword_5FD1B4 is the integer tick rate
# (sub_452BC0: dword_5FD1B4 = taskGetRate()), so the fimul is the m/tick -> m/s
# conversion and only 36 is ever correct for it. The right side is genuine m/s (the
# same field the 4.47 m/s = 10 mph thresholds in this function compare against).
#
# At 144 Hz the threshold becomes TWICE the racing line's speed instead of half, so
# every car on the track is "slow": after the five-second debounce the marshals show
# the white flag permanently, everywhere. The other two readers of dword_5FD1B4
# (sub_47D430, sub_47D730) convert debris lifetimes from seconds to ticks and are
# correct with the current rate -- so only this one operand is redirected, at a stock
# int32 36 planted in the cave tail.
WHITE_FLAG_VA = 0x459135
WHITE_FLAG_STOCK = bytes.fromhex("da0db4d15f00")   # fimul dword ptr [005FD1B4]
INT36_CONST_OFFSET = 0x2C    # STOCK_RATE as an int32, for the fimul above

# THE FLAGMEN WAVE FOUR TIMES TOO FAST -- 60FPSv1 README line 94 ("Moving 3do objects
# (flags etc..) move too fast") and the 144 readme's limitation 3, unfixed by every patch
# so far because nobody found the stepper.
#
# gpl_flagmanStepAnims (0x481710) is called once per SIM TICK from gpl_simStep. It walks
# the flagman objects as a raw block -- eax starts at the FRAME byte of object 0
# (gpl_flagmanObjBlock + 21) and strides 26 -- so the frame is [eax] and the state is
# [eax-1]. That folded offset is why an image-wide search for byte accesses at disp8 0x15
# found only the resets in gpl_flagmanSetAnim / gpl_flagmanSetFlagColour.
#
# Each state advances only when a mask of the call counter gpl_flagmanAnimTick (0x74F530)
# comes up zero, so the animation cadence is a fixed division of the sim rate: one step in
# 4 (9 Hz at 36) for the waving states, one in 8 (4.5 Hz) for the two one-shots. Widening
# each mask by rate/36 restores the wall-clock cadence, and because these are `test dl,
# imm8` the whole fix is seven one-byte edits with no cave.
#
# The frame CEILINGS in the same code (3, 9, 4, 5, 0Bh) are model-table sizes, not time,
# and are deliberately left alone. The first site is not animation: it gates
# gpl_flagmanBroadcastState, which sends the flagman array as network message 9 at 9 Hz;
# scaling it keeps the wire rate stock instead of quadrupling it when hosting.
#
# Only exact powers of two work as masks, so this is emitted only when rate/36 is one.
FLAGMAN_MASK_SITES = [
    (0x0048171B, 3, "gpl_flagmanBroadcastState gate -- the 9 Hz network update"),
    (0x0048175C, 3, "flagman state 1, idle: frame counts down to 0"),
    (0x00481771, 3, "flagman state 3, wave: ping-pong 0..3, commits the pending colour"),
    (0x0048179F, 3, "flagman state 2: frame 0..8, wraps"),
    (0x004817B1, 7, "flagman state 4: one-shot 0..4"),
    (0x004817C3, 7, "flagman state 5: one-shot 0..5"),
    (0x004817D5, 3, "flagman state 6: frame 0..10, wraps"),
]

# GEOMETRIC PER-TICK COEFFICIENTS. These do NOT follow this file's stock*scale^k rule, and
# scaling one linearly is a silent bug -- 0.995 at rate^-1 would become 0.2488, which is not
# a slow decay but an almost total one.
#
# Two forms, and they need different arithmetic:
#
#   retained   x *= k              per tick, keeping a fraction   ->  k ** (36/rate)
#   alpha      x += (target-x)*k   per tick, blending toward it   ->  1 - (1-k) ** (36/rate)
#
# Both preserve the WALL-CLOCK time constant, which is what a linear scale cannot do. The
# distinction is the whole reason this table exists: 0.995 is a retained fraction and 0.3
# is a blend fraction, and swapping their formulas gives nonsense in both directions.
#
# (address, kind, stock value, note). Every entry must be EXCLUSIVE to one consumer.
GEOMETRIC_DECAY_SITES = [
    # Debris velocity drag, three xrefs at 0x47D4D5/E0/ED, all in gpl_debrisUpdate, one per
    # velocity component. Everything else in that function is already rate-aware
    # (gpl_tickRateInt * 20.0 lifetime, gpl_tickPeriodSeconds integration).
    (0x00539E38, "retained", 0.995,
     "debris velocity drag per tick -> 0.998749 at 144"),
    # The pit board's two approach rates, in gpl_pitBoardTrackCar_perTick. Three xrefs each
    # (x, y, z), all in that function, same units. The 16.0 and 1.0 gates beside them are
    # distances SQUARED in m^2 and are rate-free.
    (0x00539F28, "alpha", 0.3,
     "pit board approach blend per tick -> 0.085309 at 144"),
    (0x00539F2C, "alpha", 0.1,
     "pit board return-to-rest blend per tick -> 0.025996 at 144"),
]

# PERIODS STORED AS "PERIOD MINUS ONE". A mask `test cl, 3` fires every 4th tick and a
# bound `cmp [esi+14h], 0Fh` with `jle` lives 16 ticks -- in both the stored number is one
# less than the duration, so the conversion is (stock + 1) * scale - 1, NOT stock * scale.
#
# (address of the imm8, stock value, expected preceding bytes, note). The prefix is checked
# so a same-offset coincidence cannot be patched by accident, and the result must stay
# within a sign-extended imm8.
PERIOD_MASK_SITES = [
    (0x0047CB22, 3, b"\xF6\xC1",
     "type 8/9 particle: frames 0..7 advance every 4th tick (test cl, 3)"),
    (0x0047CB29, 1, b"\xF6\xC1",
     "type 8/9 particle: frames 8..15 advance every 2nd tick (test cl, 1)"),
    (0x0047CBA3, 0x0F, b"\x83\x7E\x14",
     "type 32 particle: sparks die after 16 ticks (cmp [esi+14h], 0Fh; jle)"),
    # The two replay keyframe gates (v30). Both are 32-tick periods = 0.8889 s of wall
    # clock, and 0.8889 s at 144 Hz is 128 ticks, so both go 1F -> 7F. getBGCount()
    # (dword_581EB8) is the SIM TICK counter: measured against real .rpy tapes, type 13
    # appears once per taskGetRate() frames and types 2 and 10 once per 32, so the counter
    # advances exactly once per replay frame. Masking BL/AL alone is equivalent to the
    # full counter mod 128, because 128 divides 256.
    #
    # Neither reader assumes a spacing. Type 10's handler copies 16 bytes verbatim into
    # car+1084, the damage state, which is a LATCHED value with a periodic keyframe and
    # additional event writes; the worst case on a seek is 0.889 s of stale damage, which
    # is exactly what 36 Hz gives.
    (0x00452495, 0x1F, b"\x8B\xD8\x80\xE3",
     "gpl_simStep: type-10 car-damage keyframe, one record per car in 32 ticks "
     "(getBGCount() & 1Fh)"),
    (0x00451B8D, 0x1F, b"\xA1\x04\xD5\x5F\x00\x40\xA8",
     "gpl_replayWritePlayerRecords: type-2 packed telemetry keyframe, one record in "
     "32 ticks (test al, 1Fh)"),
]

AI_COUNTDOWN_SITES = [
    (0x0044337F, 144, 4, "+1159 reload, AI state machine"),
    (0x004434D7,  72, 4, "+1139 reload, AI state machine"),
    (0x00443AED, 144, 4, "+1159 reload, AI state machine"),
    (0x004449CA, 144, 4, "+1159 reload, AI state machine"),
    (0x00448EDA, 144, 4, "+1115 reload"),
    # The MOD-RANGE twin of the site above, in sub_983DF2 (the LAPPED / let-the-leader-by
    # logic). A byte search for `C7 8x 5B 04 00 00 90 00 00 00` finds exactly two instances
    # in the image: 0x448ED6, covered since v1, and this one, never scaled. 4 s of
    # decision hold becomes 1 s at 144 Hz.
    (0x00983FAB, 144, 4, "+1115 reload in mod code, the twin of 0x448EDA"),
    # THE GEAR-SHIFT REV-DECAY SUPPRESSION WINDOW (v20). car+1167, tested at 0x44C239 and
    # decremented once per tick at 0x44C6CB, suppresses the per-tick rev decay for the
    # first ticks of a shift. A wall-clock duration. Every earlier sweep missed it because
    # it is a MOD-range store and the countdown sweep never covered mod code. Unscaled the
    # engine sheds 48.75*(4H-6) RPM per shift instead of the stock-equivalent
    # 48.75*(4H-24) -- 877.5 RPM too much on every AI gear change, about 10% of the band.
    # The stock twin's immediate at 0x4973DB is dead: gpl_aiComputeDlongSpeedGoal jumps
    # over that block at 0x497343 and calls mod_aiGearShiftTest instead.
    (0x00987461, 6, 4, "+1167 rev-decay suppression window, mod code"),
    (0x0044D595, 144, 4, "+1159 reload"),
    (0x004978E8, 144, 4, "+1159 reload, the twin 60FPSv2 missed in the same function"),
    (0x004A4F55,   9, 4, "+1147 reload"),
    (0x0044B45A,   9, 1, "the every-9-ticks gate on the AI sound and telemetry update"),

    # THE OFF-TRACK GIVE-UP COUNTER (v14). `cmp dword ptr [esi+493h], 12h` at 0x444B52,
    # immediate here. car+1171 is incremented at most once per tick at 0x444B3D while the
    # car is below the moving-speed threshold OR spinning, and reset when it drives
    # normally (0x444B31), on state entry (0x4449D8) and in the ctor. At 18 the handler
    # jumps to the mod's state re-dispatch and gives up on rejoining the line.
    #
    # 18 ticks is half a second at 36 Hz and an eighth at 144. Exclusivity: a byte search
    # on the displacement gives 9 hits, of which 5 are the sites above and 4 are
    # `push 0x493` coincidences outside AI code; one reader.
    #
    # Found by an up-counter sweep, which is a class BOTH earlier sweeps were blind to --
    # the countdown sweep keys on `mov [reg+off], imm32` reloads, and this is an `inc`
    # plus a `cmp`. The class has nine members across the AI ranges and is now closed.
    (0x00444B58,  18, 1, "the off-track give-up counter's threshold (car+1171), on the "
                         "same post-contact path as the four line holds"),
]

PAT = re.compile(
    r"<VirtualAddress>\s*(?:0[xX])?([0-9A-Fa-f]+)\s*</VirtualAddress>\s*"
    r"<Data>\s*([0-9A-Fa-f]+)\s*</Data>",
    re.S,
)


def _rebase_xml_addresses(image, sites):
    """Make every site's XML address one that GEM+ will resolve to its real address.

    GPLPatchDLL adds 8 to any address it reads from a patch XML that falls in
    [0x0054C5B8, 0x00971FFF] (gplpatch.Image.fixup). Sites parsed from the reference XML
    already carry an XML address and a fixed-up real address, so they are consistent by
    construction. The hand-written tables in this file give the REAL address for both,
    which is only correct below that window -- and every site in them was below it until
    the parameter-descriptor table (0x554658..0x55595F) arrived.

    Writing 8 high there is silent and destructive: a descriptor record is 24 bytes with
    its float at base+16, so value+8 is the NEXT record's 0x42 table tag, and clobbering
    that unregisters the following key.
    """
    for site in sites:
        if image.fixup(site.xml_va) == site.real_va:
            continue
        candidate = site.real_va - image.va_shift
        if image.fixup(candidate) != site.real_va:
            raise SystemExit(
                "REFUSED: %08X cannot be expressed as an XML address -- fixup(%08X) is "
                "%08X, not %08X. It sits within %d bytes of the rebase window's edge."
                % (site.real_va, candidate, image.fixup(candidate), site.real_va,
                   image.va_shift))
        site.xml_va = candidate


def _dedupe_sites(sites):
    """Collapse exact duplicates; refuse on any other overlap.

    Two tables can legitimately reach the same address. 0x443AC9 is one: it is a v13 line
    hold AND a rate^+1 constant the 60 fps reference already scaled, so it arrives twice
    with identical bytes. The build is correct either way -- GEM+ simply writes it twice --
    but a duplicate is noise in the XML and hides the case that matters.

    That case is two sites that DISAGREE, where load order would silently decide which
    value survives. The same goes for two sites whose spans overlap without being the same
    span: a 4-byte float write that starts one byte inside another site's instruction
    corrupts both, and neither table's own check can see it. Both are refused.
    """
    seen = {}
    kept = []
    collapsed = []
    for site in sites:
        if site.out is None:
            kept.append(site)
            continue
        first = seen.get(site.real_va)
        if first is None:
            seen[site.real_va] = site
            kept.append(site)
            continue
        if first.out == site.out:
            collapsed.append(site)
            continue
        raise SystemExit(
            "REFUSED: %08X is written by two tables with DIFFERENT values -- %s writes "
            "%s, %s writes %s. Load order would decide which one survives.\n"
            "    first : %s\n    second: %s"
            % (site.real_va, first.kind, first.out.hex().upper(),
               site.kind, site.out.hex().upper(), first.note, site.note))

    spans = sorted((s.real_va, s.real_va + len(s.out), s)
                   for s in kept if s.out is not None)
    for (a0, a1, sa), (b0, b1, sb) in zip(spans, spans[1:]):
        if a1 > b0:
            raise SystemExit(
                "REFUSED: %08X..%08X and %08X..%08X overlap without being the same span. "
                "One write would land inside the other.\n    %s\n    %s"
                % (a0, a1, b0, b1, sa.note, sb.note))

    if collapsed:
        sys.stderr.write("deduped   : %d site(s) emitted twice with identical bytes\n"
                         % len(collapsed))
        for s in collapsed:
            sys.stderr.write("      %08X  %s\n" % (s.real_va, s.note))
    return kept


class Site:
    """One patch site, with how its value was derived."""

    def __init__(self, xml_va, real_va, stock, ref, kind, exponent=None, note=""):
        self.xml_va = xml_va
        self.real_va = real_va
        self.stock = stock      # bytes in the unpatched exe
        self.ref = ref          # bytes the reference patch writes
        self.kind = kind        # 'scaled' | 'substep' | 'code' | 'unclassified'
        self.exponent = exponent
        self.note = note
        self.out = None         # bytes we emit


def approx(a, b, tol=3e-3):
    if b == 0:
        return abs(a) < 1e-12
    return abs(a - b) / abs(b) <= tol


def classify(stock, ref):
    """Return (kind, exponent, decoder, encoder) for one site.

    A site is 'scaled' only if the reference value equals the stock value times
    (60/36)^k for some k. Everything else is copied verbatim -- including anything
    whose stock bytes are a NOP sled, which is inserted code, not a rescaled constant.
    """
    ratio = REF_RATE / STOCK_RATE

    if set(stock) in ({0x90}, {0x00}):
        return "code", None, None, None

    if len(stock) == 4:
        try:
            sf, = struct.unpack("<f", stock)
            rf, = struct.unpack("<f", ref)
        except struct.error:
            sf = rf = None
        if sf is not None and 1e-30 < abs(sf) < 1e30 and 1e-30 < abs(rf) < 1e30:
            for k in EXPONENTS:
                if approx(rf, sf * ratio ** k):
                    return ("scaled", k,
                            lambda b: struct.unpack("<f", b)[0],
                            lambda v: struct.pack("<f", float(v)))

    if len(stock) in (1, 2, 4):
        si = int.from_bytes(stock, "little")
        ri = int.from_bytes(ref, "little")
        if si:
            for k in EXPONENTS:
                if abs(ri - si * ratio ** k) < 0.5:
                    n = len(stock)
                    return ("scaled", k,
                            lambda b: int.from_bytes(b, "little"),
                            lambda v, n=n: int(round(v)).to_bytes(n, "little"))

    return "unclassified", None, None, None


# Sites parsed out of the reference and then deliberately dropped: the rate carriers this
# patch replaces wholesale, and the reference's own inserted code. Refilled by every call
# to build(), so tools/gplpatchtable.py can document what is NOT emitted without having to
# re-derive the rules.
DROPPED_SITES = []


def build(exe_path, ref_xml, rate):
    del DROPPED_SITES[:]
    data = io.open(exe_path, "rb").read()
    image = gplpatch.Image(data)
    text = io.open(ref_xml, encoding="utf-8-sig", errors="replace").read()

    scale = rate / STOCK_RATE
    substeps = int(round(PHYSICS_HZ_TARGET / rate))
    if substeps < 1:
        substeps = 1
    physics_hz = rate * substeps

    sites = []
    for addr, hexdata in PAT.findall(text):
        xml_va = int(addr, 16)
        real_va = image.fixup(xml_va)
        ref = bytes.fromhex(hexdata)
        off = image.offset(real_va)
        if off is None:
            raise SystemExit("%08X is not inside any section's raw data" % real_va)
        stock = data[off:off + len(ref)]

        if real_va in REFERENCE_MISTAKES:
            sys.stderr.write("dropping reference site %08X: %s\n"
                             % (real_va, REFERENCE_MISTAKES[real_va]))
            continue
        if real_va == AI_ACCESSOR_CONST_SITE:
            site = Site(xml_va, real_va, stock, ref, "rate-conversion",
                        note="36/%g, the factor the AI accessors multiply by" % rate)
            site.out = struct.pack("<f", STOCK_RATE / rate)
        elif real_va in AI_ACCESSOR_CODE_SITES:
            # Verbatim: `fmul dword ptr [00449C6C]; ret`. The fmul names the constant by
            # absolute address, so the instruction itself carries no rate.
            site = Site(xml_va, real_va, stock, ref, "rate-conversion",
                        note="fmul dword ptr [%08X]; ret" % AI_ACCESSOR_CONST_SITE)
            site.out = ref
        elif real_va in SUBSTEP_DT_SITES:
            site = Site(xml_va, real_va, stock, ref, "substep",
                        note="substep dt = 1/%g" % physics_hz)
            site.out = struct.pack("<f", 1.0 / physics_hz)
        elif real_va == SUBSTEP_HZ_SITE:
            site = Site(xml_va, real_va, stock, ref, "substep",
                        note="physics Hz = %g (= %g x %d substeps)" % (physics_hz, rate, substeps))
            site.out = struct.pack("<f", physics_hz)
        elif real_va == SUBSTEP_HALF_DT_SITE:
            site = Site(xml_va, real_va, stock, ref, "substep",
                        note="half substep dt = 1/%g" % (2 * physics_hz))
            site.out = struct.pack("<f", 1.0 / (2.0 * physics_hz))
        else:
            kind, k, decode, encode = classify(stock, ref)
            site = Site(xml_va, real_va, stock, ref, kind, k)
            if kind == "scaled":
                value = decode(stock) * scale ** k
                try:
                    site.out = encode(value)
                except (OverflowError, ValueError):
                    # The scaled value does not fit the instruction's immediate. That is a
                    # real property of the exe, not a bug here -- report it as one.
                    site.kind = "overflow"
                    site.out = None
                    if real_va == 0x4E8078:
                        # The reference writes one byte here, but the field is a four-byte
                        # imm32 that would hold 288 comfortably. See the comment on
                        # RATE_CARRYING_SITES: this site is dead, not narrow.
                        site.note = ("taskGetRate's fallback for a mode the game never "
                                     "selects; nothing reads it")
                    else:
                        site.note = ("%g does not fit the %d-byte field at this site"
                                     % (value, len(ref)))
                else:
                    site.note = "rate^%+d" % k
            else:
                site.out = ref
                site.note = "copied verbatim"
        sites.append(site)

    # Append the hardcoded tick counts. These are not in the reference patch at all (bar
    # one), so they cannot come out of the loop above -- they are synthesised here and
    # checked against the exe before being emitted.
    for va, stock_value, width, note in AI_COUNTDOWN_SITES:
        off = image.offset(va)
        if off is None:
            raise SystemExit("%08X is not inside any section's raw data" % va)
        have = int.from_bytes(data[off:off + width], "little")
        if have != stock_value:
            raise SystemExit(
                "REFUSED: %08X should hold the stock tick count %d but holds %d. This "
                "table is built from one exact revision of gplc67.exe; a different one "
                "would need the sweep re-run rather than the addresses trusted."
                % (va, stock_value, have))
        scaled = int(round(stock_value * scale))
        if scaled >> (8 * width):
            raise SystemExit(
                "REFUSED: %08X scales to %d, which does not fit its %d-byte field."
                % (va, scaled, width))
        site = Site(va, va, data[off:off + width], data[off:off + width], "tick-count",
                    note="%s: %d -> %d ticks, %.3f s either way"
                         % (note, stock_value, scaled, stock_value / STOCK_RATE))
        site.out = scaled.to_bytes(width, "little")
        sites.append(site)

    # The rev/blip machine. Same refusal discipline as the countdowns: the stock value has
    # to be there or this is not the exe this table was built from.
    for va, fmt, stock_value, exponent, note in AI_BLIP_SITES:
        off = image.offset(va)
        if off is None:
            raise SystemExit("%08X is not inside any section's raw data" % va)
        if fmt == "i4":
            have = struct.unpack_from("<I", data, off)[0]
            match = have == stock_value
        else:
            have = struct.unpack_from("<f", data, off)[0]
            match = abs(have - stock_value) <= abs(stock_value) * 1e-6
        if not match:
            raise SystemExit(
                "REFUSED: %08X should hold %s but holds %s -- this table was built from one "
                "exact revision of gplc67.exe." % (va, stock_value, have))
        value = stock_value * (scale if exponent > 0 else 1.0 / scale)
        site = Site(va, va, data[off:off + 4], data[off:off + 4], "rate-conversion",
                    note="blip machine, rate^%+d: %s -> %s (%s)"
                         % (exponent, stock_value,
                            int(round(value)) if fmt == "i4" else round(value, 8), note))
        site.out = (struct.pack("<I", int(round(value))) if fmt == "i4"
                    else struct.pack("<f", value))
        sites.append(site)

    # The tick-count immediates from the consumer audit. rate^+1, same refusal discipline.
    for va, stock_value, width, note in AI_EXTRA_TICK_SITES:
        off = image.offset(va)
        if off is None:
            raise SystemExit("%08X is not inside any section's raw data" % va)
        # A one-byte field is a sign-extended imm8 wherever it appears in x86, and some of
        # these counts are negative (`add eax, -5`), so read and write it signed.
        signed = width == 1
        have = int.from_bytes(data[off:off + width], "little", signed=signed)
        if have != stock_value:
            raise SystemExit(
                "REFUSED: %08X should hold the stock tick count %d but holds %d."
                % (va, stock_value, have))
        scaled = int(round(stock_value * scale))
        site = Site(va, va, data[off:off + width], data[off:off + width], "tick-count",
                    note="%s: %d -> %d ticks" % (note, stock_value, scaled))
        try:
            site.out = scaled.to_bytes(width, "little", signed=signed)
        except OverflowError:
            raise SystemExit("REFUSED: %08X scales to %d, which does not fit its %d-byte "
                             "field. The instruction has to be widened first."
                             % (va, scaled, width))
        sites.append(site)

    # The dimensional constants from the consumer audit. Each verified exclusive to its
    # consumer by xref before being listed here.
    for va, fmt, stock_value, exponent, note in AI_EXTRA_FLOAT_SITES:
        off = image.offset(va)
        if off is None:
            raise SystemExit("%08X is not inside any section's raw data" % va)
        n = 8 if fmt == "f8" else 4
        have = struct.unpack_from("<d" if fmt == "f8" else "<f", data, off)[0]
        if abs(have - stock_value) > abs(stock_value) * 1e-6:
            raise SystemExit("REFUSED: %08X should hold %s but holds %s."
                             % (va, stock_value, have))
        value = stock_value * scale ** exponent
        site = Site(va, va, data[off:off + n], data[off:off + n], "rate-conversion",
                    note="audit, rate^%+d: %s -> %s (%s)"
                         % (exponent, stock_value, round(value, 10), note))
        site.out = (struct.pack("<d", value) if fmt == "f8"
                    else struct.pack("<f", value))
        sites.append(site)

    # Periods stored as period-minus-one. See PERIOD_MASK_SITES.
    for va, stock_mask, prefix, note in PERIOD_MASK_SITES:
        off = image.offset(va)
        if off is None:
            raise SystemExit("%08X is not inside any section's raw data" % va)
        if data[off - len(prefix):off] != prefix or data[off] != stock_mask:
            raise SystemExit(
                "REFUSED: %08X should hold %s %02X but holds %s %02X -- not the "
                "instruction this site was built for."
                % (va, prefix.hex().upper(), stock_mask,
                   data[off - len(prefix):off].hex().upper(), data[off]))
        value = int(round((stock_mask + 1) * scale)) - 1
        if value > 127:
            raise SystemExit(
                "REFUSED: %08X would need %d, which does not fit a sign-extended imm8. "
                "The instruction has to be widened first." % (va, value))
        site = Site(va, va, data[off:off + 1], data[off:off + 1], "rate-conversion",
                    note="audit, period+1 x %g: %d -> %d (%s)"
                         % (scale, stock_mask, value, note))
        site.out = bytes([value])
        sites.append(site)

    # The geometric sites. These do NOT follow stock*scale^k -- see GEOMETRIC_DECAY_SITES.
    for va, kind, stock_value, note in GEOMETRIC_DECAY_SITES:
        off = image.offset(va)
        if off is None:
            raise SystemExit("%08X is not inside any section's raw data" % va)
        have = struct.unpack_from("<f", data, off)[0]
        if abs(have - stock_value) > abs(stock_value) * 1e-6:
            raise SystemExit("REFUSED: %08X should hold %s but holds %s."
                             % (va, stock_value, have))
        per_tick = STOCK_RATE / rate
        if kind == "retained":
            value = stock_value ** per_tick
        elif kind == "alpha":
            value = 1.0 - (1.0 - stock_value) ** per_tick
        else:
            raise SystemExit("unknown geometric kind %r at %08X" % (kind, va))
        site = Site(va, va, data[off:off + 4], data[off:off + 4], "rate-conversion",
                    note="audit, geometric-%s: %s -> %s (%s)"
                         % (kind, stock_value, round(value, 10), note))
        site.out = struct.pack("<f", value)
        sites.append(site)

    # The adj-coeff dual-use fix: detour + cave. See the comment at V13_DETOUR_VA.
    off = image.offset(V13_DETOUR_VA)
    if off is None or data[off:off + len(V13_DETOUR_STOCK)] != V13_DETOUR_STOCK:
        raise SystemExit(
            "REFUSED: %08X does not hold the expected `fsub [esp+4]; mov [esi+170h],eax` "
            "-- gpl_aiComputeDriverSpeedParams is not the code this fix was built for."
            % V13_DETOUR_VA)
    cave_off = image.offset(V13_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + 56]):
        raise SystemExit("REFUSED: the cave at %08X is not zero raw data." % V13_CAVE_VA)

    const_va = V13_CAVE_VA + 0x18
    cave = bytearray()
    cave += bytes.fromhex("898670010000")                       # mov [esi+170h], eax
    cave += b"\xd9\x05" + struct.pack("<I", const_va)           # fld [rate/36]
    cave += bytes.fromhex("d84c2404")                           # fmul dword [esp+4]
    cave += bytes.fromhex("dee9")                               # fsubp st(1), st
    jmp_from = V13_CAVE_VA + len(cave)
    cave += b"\xe9" + struct.pack("<i", V13_RESUME_VA - (jmp_from + 5))
    cave += b"\x90" * (0x18 - len(cave))                        # pad to the constants
    cave += struct.pack("<f", scale)                            # rate/36 at +0x18
    assert len(cave) == G36_CONST_OFFSET
    cave += struct.pack("<f", ONE_G_36)                         # stock 1G at +0x1C
    assert len(cave) == TPS36_CONST_OFFSET
    cave += struct.pack("<f", STOCK_RATE)                       # stock tps at +0x20
    assert len(cave) == TC36_CONST_OFFSET
    cave += struct.pack("<f", ONE_TC_36)                        # stock tc at +0x24
    assert len(cave) == SPT36_CONST_OFFSET
    cave += struct.pack("<f", 1.0 / scale)                      # 36/rate at +0x28
    assert len(cave) == INT36_CONST_OFFSET
    cave += struct.pack("<i", int(STOCK_RATE))                  # stock tps, int, at +0x2C
    site = Site(V13_CAVE_VA, V13_CAVE_VA, bytes(len(cave)), bytes(cave), "rate-conversion",
                note="adj-coeff dual-use fix, cave: this[92] = v12 as before, then "
                     "v13 = 1 - v12*(rate/36) so the traction scalings see the "
                     "UNCONVERTED driver-quality product. rate/36 = %g. The tail float "
                     "is the STOCK 1G (m/tick^2 at 36 Hz) for the reader below." % scale)
    site.out = bytes(cave)
    sites.append(site)

    # The shared-1G split: repoint mod_aiAeroCornerSpeedScale's fld at the stock copy.
    off = image.offset(G36_FLD_VA)
    if off is None or data[off:off + len(G36_FLD_STOCK)] != G36_FLD_STOCK:
        raise SystemExit(
            "REFUSED: %08X does not hold `fld dword ptr [0053A758]` -- "
            "mod_aiAeroCornerSpeedScale is not the code this fix was built for."
            % G36_FLD_VA)
    redirect = b"\xd9\x05" + struct.pack("<I", V13_CAVE_VA + G36_CONST_OFFSET)
    site = Site(G36_FLD_VA, G36_FLD_VA, G36_FLD_STOCK, redirect, "rate-conversion",
                note="mod_aiAeroCornerSpeedScale runs in m/tick@36 .lp units and must "
                     "keep the STOCK 1G while 0x53A758 is scaled for its current-rate "
                     "readers: fld repointed at the stock copy in the cave tail")
    site.out = redirect
    sites.append(site)

    # v4: the other mod .lp-world reads of rate-carrying globals. Same shape as the G
    # split: opcode kept, disp32 repointed at a stock-value copy in the cave tail.
    for va, stockhex, const_off, note in MOD_LP_WORLD_REDIRECTS:
        stock_bytes = bytes.fromhex(stockhex)
        off = image.offset(va)
        if off is None or data[off:off + 6] != stock_bytes:
            raise SystemExit(
                "REFUSED: %08X does not hold the expected FPU load (%s) -- the mod code "
                "is not the revision this redirect table was built from." % (va, stockhex))
        out = stock_bytes[:2] + struct.pack("<I", V13_CAVE_VA + const_off)
        site = Site(va, va, stock_bytes, out, "rate-conversion",
                    note="mod .lp-world redirect: %s" % note)
        site.out = out
        sites.append(site)

    # v6: the .lp target yaw rate. See the comment at LP_YAWRATE_VA.
    off = image.offset(LP_YAWRATE_VA)
    if off is None or data[off:off + len(LP_YAWRATE_STOCK)] != LP_YAWRATE_STOCK:
        raise SystemExit(
            "REFUSED: %08X does not hold `fld [ecx+eax*4+1Ch]; retn` followed by six "
            "bytes of alignment padding -- sub_44A920 is not the accessor this fix was "
            "built for, or something else already claimed its padding." % LP_YAWRATE_VA)
    yaw = (bytes.fromhex("d944811c")                              # fld [ecx+eax*4+1Ch]
           + b"\xd8\x0d" + struct.pack("<I", V13_CAVE_VA + SPT36_CONST_OFFSET)
           + b"\xc3")                                             # fmul [36/rate]; retn
    assert len(yaw) == len(LP_YAWRATE_STOCK)
    site = Site(LP_YAWRATE_VA, LP_YAWRATE_VA, LP_YAWRATE_STOCK, yaw, "rate-conversion",
                note="the .lp target yaw rate (record field +12, rad/tick at 36 Hz) is "
                     "the one per-tick .lp quantity with no car+376 wrapper: converted "
                     "here by %g so gpl_aiIntegrateYawPerTick's spring target is a rate "
                     "in the CURRENT tick" % (1.0 / scale))
    site.out = yaw
    sites.append(site)

    # v7: the marshals' white flag. See the comment at WHITE_FLAG_VA.
    off = image.offset(WHITE_FLAG_VA)
    if off is None or data[off:off + len(WHITE_FLAG_STOCK)] != WHITE_FLAG_STOCK:
        raise SystemExit(
            "REFUSED: %08X does not hold `fimul dword ptr [005FD1B4]` -- the marshal "
            "flag test in sub_458EB0 is not the code this fix was built for."
            % WHITE_FLAG_VA)
    flag = WHITE_FLAG_STOCK[:2] + struct.pack("<I", V13_CAVE_VA + INT36_CONST_OFFSET)
    site = Site(WHITE_FLAG_VA, WHITE_FLAG_VA, WHITE_FLAG_STOCK, flag, "rate-conversion",
                note="the marshals' white-flag test converts a .lp line speed "
                     "(m/tick@36) to m/s with the CURRENT tick rate, so at %g Hz every "
                     "car counts as a slow vehicle and the flag is shown permanently: "
                     "fimul repointed at a stock int 36 in the cave tail" % rate)
    site.out = flag
    sites.append(site)

    # v13: the four missed line holds. See the comment at AI_LINE_HOLD_SITES.
    for va, stock_value, exponent, note in AI_LINE_HOLD_SITES:
        off = image.offset(va)
        if off is None or abs(struct.unpack("<f", bytes(data[off:off + 4]))[0]
                              - stock_value) > 1e-3:
            raise SystemExit("REFUSED: %08X does not hold the float %g -- the push into "
                             "gpl_aiSetLineHoldTicks is not where this fix expects it."
                             % (va, stock_value))
        out = struct.pack("<f", stock_value * scale ** exponent)
        site = Site(va, va, bytes(data[off:off + 4]), out, "tick-count", exponent, note)
        site.out = out
        sites.append(site)

    # v13: the lateral-velocity term in the dlat ceiling. See DLAT_VEL_SITES.
    for i, (va, disp, note) in enumerate(DLAT_VEL_SITES):
        stock = b"\xd8\x86" + struct.pack("<I", disp)          # fadd [esi+disp32]
        off = image.offset(va)
        if off is None or data[off:off + 6] != stock:
            raise SystemExit("REFUSED: %08X does not hold `fadd dword ptr [esi+%Xh]` -- "
                             "gpl_aiDlatSpringAccel is not the code this fix was built "
                             "for." % (va, disp))
        stub_va = DLAT_VEL_CAVE_VA + i * DLAT_VEL_STUB_STRIDE
        stub = (b"\xd9\x86" + struct.pack("<I", disp)                     # fld [esi+disp]
                + b"\xd8\x0d" + struct.pack("<I", V13_CAVE_VA + SPT36_CONST_OFFSET)
                + b"\xde\xc1"                                             # faddp st(1),st
                + b"\xc3")                                                # retn
        if image.offset(stub_va) is None or any(
                data[image.offset(stub_va) + k] for k in range(DLAT_VEL_STUB_STRIDE)):
            raise SystemExit("REFUSED: the cave at %08X is not clear for %d bytes -- "
                             "something else claims it." % (stub_va, DLAT_VEL_STUB_STRIDE))
        s = Site(stub_va, stub_va, bytes(DLAT_VEL_STUB_STRIDE), stub, "rate-conversion",
                 note="stub for %s: the fadd, with the 36/rate factor folded in" % note)
        s.out = stub
        sites.append(s)

        call = b"\xe8" + struct.pack("<i", stub_va - (va + 5)) + b"\x90"
        s = Site(va, va, stock, call, "rate-conversion",
                 note="%s is a lateral VELOCITY (m/tick) added to an m/tick^2 ceiling, so "
                      "at %g Hz it is %gx too strong and saturates the lateral command "
                      "whenever a car is alongside: routed through a stub that scales it "
                      "by 36/rate" % (note, rate, rate / STOCK_RATE))
        s.out = call
        sites.append(s)

    # v9: the flagmen. See the comment at FLAGMAN_MASK_SITES.
    steps = rate / STOCK_RATE
    if steps == int(steps) and int(steps) & (int(steps) - 1) == 0:
        for va, stock_mask, note in FLAGMAN_MASK_SITES:
            off = image.offset(va)
            if off is None or data[off - 2:off + 1] != bytes([0xF6, 0xC2, stock_mask]):
                raise SystemExit(
                    "REFUSED: %08X is not `test dl, %d` -- gpl_flagmanStepAnims is not "
                    "the code this fix was built for." % (va, stock_mask))
            mask = (stock_mask + 1) * int(steps) - 1
            if mask > 0xFF:
                raise SystemExit("REFUSED: flagman mask %d at %08X does not fit a byte"
                                 % (mask, va))
            site = Site(va, va, bytes([stock_mask]), bytes([mask]), "rate-conversion",
                        note="%s: one step in %d ticks, unchanged at %g Hz in wall-clock "
                             "terms" % (note, mask + 1, rate))
            site.out = site.ref
            sites.append(site)
    else:
        sys.stderr.write(
            "note: rate/36 = %g is not a power of two, so the flagman animation masks "
            "cannot be widened (they are `test dl, imm8`); the flagmen will wave %gx too "
            "fast.\n" % (steps, steps))

    detour = b"\xe9" + struct.pack("<i", V13_CAVE_VA - (V13_DETOUR_VA + 5)) + b"\x90" * 5
    site = Site(V13_DETOUR_VA, V13_DETOUR_VA, V13_DETOUR_STOCK, detour, "rate-conversion",
                note="adj-coeff dual-use fix, detour from gpl_aiComputeDriverSpeedParams "
                     "to the cave at %08X" % V13_CAVE_VA)
    site.out = detour
    sites.append(site)

    # The FFB smoothing filter. See FFB_IIR_BLOCK_VA.
    off = image.offset(FFB_IIR_BLOCK_VA)
    if off is None or data[off:off + len(FFB_IIR_BLOCK_STOCK)] != FFB_IIR_BLOCK_STOCK:
        raise SystemExit(
            "REFUSED: %08X does not hold `fadd [state]; fmul [0.5]; fst [state]` -- not "
            "the filter this fix was built for." % FFB_IIR_BLOCK_VA)
    for va, n in ((FFB_IIR_ALPHA_VA, 8), (FFB_IIR_STUB_VA, 0x30)):
        o = image.offset(va)
        if o is None or any(data[o:o + n]):
            raise SystemExit("REFUSED: %08X is not %d bytes of zero raw data." % (va, n))

    ffb_alpha = 1.0 - (1.0 - FFB_IIR_ALPHA_STOCK) ** (1.0 / scale)
    ffb_state = struct.pack("<I", FFB_IIR_STATE_VA)
    ffb_stub = (b"\xd8\x0d" + struct.pack("<I", FFB_IIR_ALPHA_VA)        # fmul [alpha]
                + b"\xd9\x05" + ffb_state                                # fld  [state]
                + b"\xd8\x0d" + struct.pack("<I", FFB_IIR_ONE_MINUS_VA)  # fmul [1-alpha]
                + b"\xde\xc1"                                            # faddp st(1), st
                + b"\xd9\x15" + ffb_state                                # fst  [state]
                + b"\xe9" + struct.pack(
                    "<i", (FFB_IIR_BLOCK_VA + len(FFB_IIR_BLOCK_STOCK))
                          - (FFB_IIR_STUB_VA + 26 + 5)))                 # jmp back
    for va, stock, out, note in (
            (FFB_IIR_ALPHA_VA, bytes(4), struct.pack("<f", ffb_alpha),
             "FFB filter alpha = 1 - (1-%s)^(36/%g) = %.8f. NOT alpha/4 = 0.125: "
             "preserving a time constant is exponential, and the linear answer is 27%% off"
             % (FFB_IIR_ALPHA_STOCK, rate, ffb_alpha)),
            (FFB_IIR_ONE_MINUS_VA, bytes(4), struct.pack("<f", 1.0 - ffb_alpha),
             "FFB filter 1 - alpha = %.8f, the SECOND tap the stock code does not have. "
             "One constant on both taps has DC gain 1; substituting alpha alone would "
             "collapse the wheel to 19%% of the physics' output" % (1.0 - ffb_alpha)),
            (FFB_IIR_STUB_VA, bytes(len(ffb_stub)), ffb_stub,
             "FFB filter cave: fmul alpha; fld state; fmul (1-alpha); faddp; fst state; "
             "jmp back"),
            (FFB_IIR_BLOCK_VA, FFB_IIR_BLOCK_STOCK,
             b"\xe9" + struct.pack("<i", FFB_IIR_STUB_VA - (FFB_IIR_BLOCK_VA + 5))
             + b"\x90" * (len(FFB_IIR_BLOCK_STOCK) - 5),
             "jmp into the FFB filter cave at %08X, replacing fadd/fmul/fst"
             % FFB_IIR_STUB_VA)):
        o = image.offset(va)
        if o is None or data[o:o + len(stock)] != stock:
            raise SystemExit(
                "REFUSED: %08X should hold %s but holds %s."
                % (va, stock.hex().upper(),
                   data[o:o + len(stock)].hex().upper() if o is not None else "?"))
        s = Site(va, va, stock, out, "rate-conversion", note=note)
        s.out = out
        sites.append(s)

    # The GPLRank best-lap block. See BEST_LAP_BLOCK_SITES.
    for va, stock, out, note in BEST_LAP_BLOCK_SITES:
        off = image.offset(va)
        if off is None or data[off:off + len(stock)] != stock:
            raise SystemExit(
                "REFUSED: %08X should hold %s but holds %s -- not the store this block "
                "was built for."
                % (va, stock.hex().upper(),
                   data[off:off + len(stock)].hex().upper() if off is not None else "?"))
        s = Site(va, va, stock, out, "rate-conversion",
                 note="GPLRANK BEST-LAP BLOCK (not a rate fix): " + note)
        s.out = out
        sites.append(s)

    # Leaks D, E and F. See LP_LEAK_CAVE_VA.
    cave_off = image.offset(LP_LEAK_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + LP_LEAK_CAVE_SPAN]):
        raise SystemExit("REFUSED: the cave at %08X is not zero raw data for %d bytes."
                         % (LP_LEAK_CAVE_VA, LP_LEAK_CAVE_SPAN))
    stub_va = LP_LEAK_CAVE_VA
    for site_va, stock, resume_va, body, note in _lp_leak_stubs(
            V13_CAVE_VA + 0x18, V13_CAVE_VA + SPT36_CONST_OFFSET):
        off = image.offset(site_va)
        if off is None or data[off:off + len(stock)] != stock:
            raise SystemExit(
                "REFUSED: %08X should hold %s but holds %s -- not the instruction this "
                "stub was built for."
                % (site_va, stock.hex().upper(),
                   data[off:off + len(stock)].hex().upper() if off is not None else "?"))
        if len(stock) < 5:
            raise SystemExit("REFUSED: %08X has only %d bytes for a 5-byte detour."
                             % (site_va, len(stock)))
        stub = body + b"\xe9" + struct.pack("<i", resume_va - (stub_va + len(body) + 5))
        if stub_va + len(stub) > LP_LEAK_CAVE_VA + LP_LEAK_CAVE_SPAN:
            raise SystemExit("REFUSED: the .lp leak stubs do not fit in %d bytes of cave."
                             % LP_LEAK_CAVE_SPAN)
        s = Site(stub_va, stub_va, bytes(len(stub)), stub, "rate-conversion",
                 note="v30 cave stub for %08X: %s" % (site_va, note))
        s.out = stub
        sites.append(s)

        detour = (b"\xe9" + struct.pack("<i", stub_va - (site_va + 5))
                  + b"\x90" * (len(stock) - 5))
        s = Site(site_va, site_va, stock, detour, "rate-conversion",
                 note="v30 jmp into the cave at %08X: %s" % (stub_va, note))
        s.out = detour
        sites.append(s)
        stub_va += LP_LEAK_STUB_STRIDE

    # The three multiply-by-36/rate stubs. See LP_MUL_CAVE_VA.
    cave_off = image.offset(LP_MUL_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + LP_MUL_CAVE_SPAN]):
        raise SystemExit("REFUSED: the cave at %08X is not zero raw data for %d bytes."
                         % (LP_MUL_CAVE_VA, LP_MUL_CAVE_SPAN))
    stub_va = LP_MUL_CAVE_VA
    for site_va, stock, kind, stub, note in _lp_mul_stubs(V13_CAVE_VA + SPT36_CONST_OFFSET):
        off = image.offset(site_va)
        if off is None or data[off:off + len(stock)] != stock:
            raise SystemExit(
                "REFUSED: %08X should hold %s but holds %s -- not the instruction this "
                "stub was built for."
                % (site_va, stock.hex().upper(),
                   data[off:off + len(stock)].hex().upper() if off else "?"))
        if stub_va + len(stub) > LP_MUL_CAVE_VA + LP_MUL_CAVE_SPAN:
            raise SystemExit("REFUSED: the stubs do not fit in %d bytes of cave."
                             % LP_MUL_CAVE_SPAN)

        s = Site(stub_va, stub_va, bytes(len(stub)), stub, "rate-conversion",
                 note="v21 cave stub for %08X: %s" % (site_va, note))
        s.out = stub
        sites.append(s)

        opcode = b"\xe8" if kind == "call" else b"\xe9"
        rel = struct.pack("<i", stub_va - (site_va + 5))
        patch = opcode + rel + b"\x90" * (len(stock) - 5)
        assert len(patch) == len(stock)
        s = Site(site_va, site_va, stock, patch, "rate-conversion",
                 note="v21 %s into the cave at %08X, so the value the ini supplied is "
                      "MULTIPLIED by 36/rate rather than replaced: %s"
                      % (kind, stub_va, note))
        s.out = patch
        sites.append(s)
        stub_va = (stub_va + len(stub) + 15) & ~15

    # The three task registrations, widened into a cave of our own.
    #
    # The reference's own rate carriers are superseded: it writes a one-byte immediate in
    # place, we replace the whole instruction. Both would land on the same bytes and
    # _dedupe_sites would refuse, which is the guard doing its job -- so drop them here
    # rather than let two strategies fight.
    superseded = [s for s in sites if s.real_va in RATE_CARRYING_SITES]
    for s in superseded:
        sites.remove(s)
        DROPPED_SITES.append(s)
        sys.stderr.write("dropping reference rate carrier %08X (%s) -- this patch widens "
                         "the whole instruction instead\n" % (s.real_va, s.note))

    # The reference's INSERTED CODE cannot be copied. Its `code` sites build stubs in
    # alignment padding and its `unclassified` ones are branch displacements retargeted
    # INTO those stubs -- copying the branches without the stubs would jump into whatever
    # happens to be there. Whatever functional change they make has not been re-derived,
    # so the whole group is dropped and reported.
    orphans = [s for s in sites if s.kind in ("code", "unclassified")]
    for s in orphans:
        sites.remove(s)
        DROPPED_SITES.append(s)
    if orphans:
        sys.stderr.write(
            "\nUNRESOLVED -- %d reference site(s) dropped because they are 60FPSv2's own\n"
            "  inserted code (stubs in padding, plus branches retargeted into them).\n"
            "  Whatever functional change they make is NOT yet in this patch. They\n"
            "  cluster in 0045D5xx..0045DBxx and 00410509.\n" % len(orphans))
        for s in orphans:
            sys.stderr.write("      %08X  %-13s stock %s ref %s\n"
                             % (s.real_va, s.kind, s.stock.hex().upper(),
                                s.ref.hex().upper()))

    cave_off = image.offset(STANDALONE_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + STANDALONE_CAVE_SPAN]):
        raise SystemExit(
            "REFUSED: the cave at %08X is not zero raw data for %d bytes. The 77G patch "
            "must NOT be applied alongside this one -- that is where its own caves live."
            % (STANDALONE_CAVE_VA, STANDALONE_CAVE_SPAN))
    stub_va = STANDALONE_CAVE_VA
    for site_va, stock, resume_va, kind, mult, build, note in STANDALONE_TASK_SITES:
        off = image.offset(site_va)
        if off is None or data[off:off + len(stock)] != stock:
            raise SystemExit(
                "REFUSED: %08X should hold %s but holds %s -- not the registration "
                "this stub was built for."
                % (site_va, stock.hex().upper(),
                   data[off:off + len(stock)].hex().upper() if off is not None else "?"))
        value = int(round(rate * mult))
        body = build(value)
        stub = body + b"\xe9" + struct.pack("<i", resume_va - (stub_va + len(body) + 5))
        if stub_va + len(stub) > STANDALONE_CAVE_VA + STANDALONE_CAVE_SPAN:
            raise SystemExit("REFUSED: the task stubs do not fit in %d bytes."
                             % STANDALONE_CAVE_SPAN)
        s = Site(stub_va, stub_va, bytes(len(stub)), stub, "rate-conversion",
                 note="cave stub for %08X: push %d (%s)" % (site_va, value, note))
        s.out = stub
        sites.append(s)

        patch = (b"\xe9" + struct.pack("<i", stub_va - (site_va + 5))
                 + b"\x90" * (len(stock) - 5))
        s = Site(site_va, site_va, stock, patch, "rate-conversion",
                 note="jmp into the cave at %08X, because push %d does not fit a "
                      "sign-extended imm8 (%s)" % (stub_va, value, note))
        s.out = patch
        sites.append(s)
        stub_va = (stub_va + len(stub) + 3) & ~3

    def add(va, stock, out, note):
        off = image.offset(va)
        if off is None or data[off:off + len(stock)] != stock:
            raise SystemExit(
                "REFUSED: %08X should hold %s but holds %s -- %s"
                % (va, stock.hex().upper(),
                   data[off:off + len(stock)].hex().upper() if off is not None else "?",
                   note))
        s = Site(va, va, stock, out, "rate-conversion", note=note)
        s.out = out
        sites.append(s)

    # (a) the replay chunk version stamp
    add(STANDALONE_REPLAY_VERSION_VA, STANDALONE_REPLAY_VERSION_STOCK,
        struct.pack("<I", (int(round(rate)) << 8) | 2),
        "replay RPLY chunk version = 2 | (rate << 8) = 0x%04X; a cooperative tag for "
        "external tools, which need the recording rate to interpret a frame-indexed "
        "replay" % ((int(round(rate)) << 8) | 2))

    # (b) the multiplayer frame-rate handshake
    sig = STANDALONE_MP_SIG_BASE | (int(round(rate)) & 0xFFFF)
    add(STANDALONE_MP_WRITER_VA, STANDALONE_MP_WRITER_STOCK,
        bytes.fromhex("c74648") + struct.pack("<I", sig)          # mov [esi+48h], sig
        + bytes.fromhex("c68647000000" "01")                      # mov [esi+47h], 1
        + bytes.fromhex("5f5ec3")                                 # restored epilogue
        + b"\x90" * 4,
        "gpl_driverRecordInit stamps the build signature %08X into the join record; "
        "esi is the record pointer and is not clobbered after 0x00410421" % sig)

    chain = [
        (STANDALONE_MP_ENTRY_VA, bytes.fromhex("90" * 6),
         b"\xe9" + struct.pack("<i", STANDALONE_MP_CHECK_VA
                               - (STANDALONE_MP_ENTRY_VA + 5)) + b"\x90",
         "entry hop into the signature check"),
        (0x0045D52C, bytes.fromhex("741f"),
         b"\x74" + struct.pack("<b", STANDALONE_MP_ENTRY_VA - (0x0045D52C + 2)),
         "no-password branch retargeted into the check"),
        (0x0045D53F, bytes.fromhex("750c"),
         b"\x75" + struct.pack("<b", STANDALONE_MP_ENTRY_VA - (0x0045D53F + 2)),
         "password-ok branch retargeted into the check"),
        (STANDALONE_MP_CHECK_VA, bytes.fromhex("90" * 14),
         bytes.fromhex("817e48") + struct.pack("<I", sig)         # cmp [esi+48h], sig
         + b"\x74" + struct.pack("<b", STANDALONE_MP_HOPS[0][0]
                                 - (STANDALONE_MP_CHECK_VA + 7 + 2))
         + b"\xe9" + struct.pack("<i", STANDALONE_MP_REJECT_VA
                                 - (STANDALONE_MP_CHECK_VA + 7 + 2 + 5)),
         "cmp the client's signature; match hops on, mismatch jumps to the "
         "'not qualified' reject at %08X" % STANDALONE_MP_REJECT_VA),
    ]
    for va, stock, target, note in STANDALONE_MP_HOPS:
        chain.append((va, stock,
                      b"\xeb" + struct.pack("<b", target - (va + 2)) + b"\x90" * 2,
                      "%s: the gaps are too small for anything but a short jump" % note))
    chain.append((0x0045DAF6, bytes.fromhex("90" * 10),
                  b"\xe9" + struct.pack("<i", STANDALONE_MP_ACCEPT_VA - (0x0045DAF6 + 5))
                  + b"\x90" * 5,
                  "accepted: resume the stock password/laptime/rating checks"))
    for va, stock, out, note in chain:
        add(va, stock, out, note)

    # (c) the GUI event throttle
    cave_off = image.offset(STANDALONE_GUI_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + STANDALONE_GUI_CAVE_SPAN]):
        raise SystemExit("REFUSED: the GUI cave at %08X is not zero raw data."
                         % STANDALONE_GUI_CAVE_VA)
    for va in (STANDALONE_GUI_LASTTICK_VA, STANDALONE_GUI_LASTID_VA):
        off = image.offset(va)
        if off is None or data[off:off + 4] != bytes(4):
            raise SystemExit(
                "REFUSED: the GUI throttle's state dword at %08X is not zero -- "
                "something else is using it." % va)
    gate = max(1, int(round(rate / STOCK_RATE)))
    body = _standalone_gui_cave(STANDALONE_GUI_CAVE_VA, gate)
    if len(body) > STANDALONE_GUI_CAVE_SPAN:
        raise SystemExit("REFUSED: the GUI cave needs %d bytes, %d available."
                         % (len(body), STANDALONE_GUI_CAVE_SPAN))
    add(STANDALONE_GUI_CAVE_VA, bytes(len(body)), body,
        "GUI same-id throttle cave, gate %d ticks (= the %g Hz cadence). Deliver when "
        "the widget id differs from the last one, or when %d scheduler ticks have "
        "passed; a held control is a dense run of one id, a click is a lone callback"
        % (gate, STOCK_RATE, gate))
    add(STANDALONE_GUI_DETOUR_VA, STANDALONE_GUI_DETOUR_STOCK,
        b"\xe9" + struct.pack("<i", STANDALONE_GUI_CAVE_VA
                              - (STANDALONE_GUI_DETOUR_VA + 5)),
        "jmp into the GUI throttle cave at %08X, replacing "
        "`mov eax,[esp+10h]; push eax` which the cave replays"
        % STANDALONE_GUI_CAVE_VA)

    # The stock-copy slab and the readers moved onto it. See STOCK_COPY_REDIRECTS.
    cave_off = image.offset(STOCK_COPY_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + STOCK_COPY_CAVE_SPAN]):
        raise SystemExit("REFUSED: the stock-copy cave at %08X is not zero raw data."
                         % STOCK_COPY_CAVE_VA)
    for insn_va, opcode, old_target, stock_value, slot, note in STOCK_COPY_REDIRECTS:
        off = image.offset(insn_va)
        if off is None or data[off:off + len(opcode)] != opcode or \
                struct.unpack_from("<I", data, off + len(opcode))[0] != old_target:
            raise SystemExit(
                "REFUSED: %08X does not read %08X with opcode %s -- it holds %s. The "
                "reader this redirect was built for has moved."
                % (insn_va, old_target, opcode.hex().upper(),
                   data[off:off + 6].hex().upper() if off is not None else "?"))
        slot_off = image.offset(STOCK_COPY_CAVE_VA + slot)
        if struct.unpack_from("<f", data, image.offset(old_target))[0] != \
                struct.unpack("<f", struct.pack("<f", stock_value))[0]:
            raise SystemExit(
                "REFUSED: %08X does not hold the stock value %g." % (old_target,
                                                                     stock_value))
        s = Site(STOCK_COPY_CAVE_VA + slot, STOCK_COPY_CAVE_VA + slot,
                 bytes(4), struct.pack("<f", stock_value), "rate-conversion",
                 note="v25 stock copy of %08X = %g, for a reader that must not see the "
                      "scaled value" % (old_target, stock_value))
        s.out = struct.pack("<f", stock_value)
        sites.append(s)

        disp_va = insn_va + len(opcode)
        s = Site(disp_va, disp_va, data[image.offset(disp_va):image.offset(disp_va) + 4],
                 struct.pack("<I", STOCK_COPY_CAVE_VA + slot), "rate-conversion",
                 note="v25 operand relocation at %08X: %08X -> %08X (%s)"
                      % (insn_va, old_target, STOCK_COPY_CAVE_VA + slot, note))
        s.out = struct.pack("<I", STOCK_COPY_CAVE_VA + slot)
        sites.append(s)

    # The particle cave slots and the operand relocations that point at them.
    cave_off = image.offset(PARTICLE_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + PARTICLE_CAVE_SPAN]):
        raise SystemExit("REFUSED: the particle cave at %08X is not zero raw data."
                         % PARTICLE_CAVE_VA)
    for slot, kind, stock_value, note in PARTICLE_SLOTS:
        value = (stock_value ** (STOCK_RATE / rate) if kind == "geom"
                 else stock_value * scale ** kind)
        s = Site(PARTICLE_CAVE_VA + slot, PARTICLE_CAVE_VA + slot,
                 bytes(4), struct.pack("<f", value), "rate-conversion",
                 note="v23 particle slot: %s -> %s (%s)"
                      % (stock_value, round(value, 10), note))
        s.out = struct.pack("<f", value)
        sites.append(s)

    for disp_va, insn_va, opcode, old_target, slot in PARTICLE_REDIRECTS:
        off = image.offset(insn_va)
        if off is None or data[off:off + 2] != opcode or \
                struct.unpack_from("<I", data, off + 2)[0] != old_target:
            raise SystemExit(
                "REFUSED: %08X should hold %s reading %08X -- not the instruction this "
                "relocation was built for." % (insn_va, opcode.hex().upper(), old_target))
        new_target = PARTICLE_CAVE_VA + slot
        s = Site(disp_va, disp_va, struct.pack("<I", old_target),
                 struct.pack("<I", new_target), "rate-conversion",
                 note="v23 operand relocation at %08X: %08X -> %08X (the slot is shared "
                      "with a different unit, so only this reader moves)"
                      % (insn_va, old_target, new_target))
        s.out = struct.pack("<I", new_target)
        sites.append(s)

    # The type-4 puff lifetime: an in-place recompaction, no cave. See TYPE4_BLOCK_VA.
    off = image.offset(TYPE4_BLOCK_VA)
    if off is None or data[off:off + len(TYPE4_STOCK)] != TYPE4_STOCK:
        raise SystemExit("REFUSED: %08X is not the store block this patch was built for."
                         % TYPE4_BLOCK_VA)
    pad_off = image.offset(TYPE4_SCALE_VA)
    if pad_off is None or data[pad_off:pad_off + 4] != b"\x90\x90\x90\x90":
        raise SystemExit("REFUSED: %08X is not alignment padding." % TYPE4_SCALE_VA)
    block = _type4_block()
    assert len(block) == len(TYPE4_STOCK)
    s = Site(TYPE4_BLOCK_VA, TYPE4_BLOCK_VA, TYPE4_STOCK, block, "rate-conversion",
             note="v23 type-4 puff lifetime: cache the particle pointer in ebx to free "
                  "room for an fmul, so the 1..8 tick life becomes %d..%d"
                  % (round(scale), round(8 * scale)))
    s.out = block
    sites.append(s)
    s = Site(TYPE4_SCALE_VA, TYPE4_SCALE_VA, b"\x90\x90\x90\x90",
             struct.pack("<f", scale), "rate-conversion",
             note="v23 type-4 lifetime scale = rate/36 = %g, in the alignment padding "
                  "after sub_47C4D0's retn" % scale)
    s.out = struct.pack("<f", scale)
    sites.append(s)

    # The net/replay extrapolation caves. See NET_CAVE_VA.
    cave_off = image.offset(NET_CAVE_VA)
    if cave_off is None or any(data[cave_off:cave_off + NET_CAVE_SPAN]):
        raise SystemExit("REFUSED: the net cave at %08X is not %d bytes of zero raw data."
                         % (NET_CAVE_VA, NET_CAVE_SPAN))
    alpha = 1.0 - (1.0 - NET_IIR_ALPHA_STOCK) ** (STOCK_RATE / rate)
    alpha_va, one_minus_va, free = NET_CAVE_VA, NET_CAVE_VA + 4, NET_CAVE_VA + 8
    for va, value, what in ((alpha_va, alpha, "alpha"),
                            (one_minus_va, 1.0 - alpha, "1 - alpha")):
        s = Site(va, va, bytes(4), struct.pack("<f", value), "rate-conversion",
                 note="v24 net-filter %s = %.8f" % (what, value))
        s.out = struct.pack("<f", value)
        sites.append(s)

    def place(stub, note, site_va, stock):
        """Emit a cave stub and the 5-byte jump that enters it."""
        nonlocal free
        if free + len(stub) > NET_CAVE_VA + NET_CAVE_SPAN:
            raise SystemExit("REFUSED: the net stubs do not fit in %d bytes of cave."
                             % NET_CAVE_SPAN)
        s = Site(free, free, bytes(len(stub)), stub, "rate-conversion", note="v24 " + note)
        s.out = stub
        sites.append(s)
        entry = (b"\xe9" + struct.pack("<i", free - (site_va + 5))
                 + b"\x90" * (len(stock) - 5))
        s = Site(site_va, site_va, stock, entry, "rate-conversion",
                 note="v24 jump into the cave at %08X: %s" % (free, note))
        s.out = entry
        sites.append(s)
        free = (free + len(stub) + 3) & ~3

    window_scale = int(round(36 * scale))
    for site_va, note in NET_WINDOW_SITES:
        off = image.offset(site_va)
        stock = bytes(data[off:off + 9])
        if stock[0:2] != b"\x83\xfe" or stock[3] != 0x0F or stock[4] not in (0x8C, 0x8F):
            raise SystemExit("REFUSED: %08X is not `cmp esi, imm8` + `0F 8x rel32` (%s)."
                             % (site_va, stock.hex().upper()))
        imm = struct.unpack_from("<b", stock, 2)[0]
        target = site_va + 9 + struct.unpack_from("<i", stock, 5)[0]
        widened = window_scale if imm > 0 else -window_scale
        # cmp esi, imm32 ; jcc <original target> ; jmp <fall-through>
        stub = (b"\x81\xfe" + struct.pack("<i", widened)
                + b"\x0f" + bytes([stock[4]]) + struct.pack("<i", target - (free + 12))
                + b"\xe9" + struct.pack("<i", (site_va + 9) - (free + 17)))
        place(stub, "%+d -> %+d ticks, %s" % (imm, widened, note), site_va, stock)

    off = image.offset(NET_IIR_VA)
    if off is None or data[off:off + len(NET_IIR_STOCK)] != NET_IIR_STOCK:
        raise SystemExit("REFUSED: %08X is not the rec+72 filter this fix was built for."
                         % NET_IIR_VA)
    iir = (b"\xd8\x0d" + struct.pack("<I", alpha_va)          # fmul [alpha]
           + bytes.fromhex("d94348")                          # fld  [ebx+48h]
           + b"\xd8\x0d" + struct.pack("<I", one_minus_va)    # fmul [1-alpha]
           + bytes.fromhex("dec1")                            # faddp st(1), st
           + bytes.fromhex("d95b48")                          # fstp [ebx+48h]
           + bytes.fromhex("8b8390020000" "85c0"))            # the mov/test the jz needs
    iir += b"\xe9" + struct.pack("<i", (NET_IIR_VA + len(NET_IIR_STOCK))
                                       - (free + len(iir) + 5))
    place(iir, "rec+72 ground-clearance filter, two-constant form so the DC gain stays 1",
          NET_IIR_VA, NET_IIR_STOCK)

    sites = _dedupe_sites(sites)
    _rebase_xml_addresses(image, sites)
    return image, sites, substeps, physics_hz


def check_ceiling(rate, sites):
    """Refuse rather than emit a patch that sign-flips a push imm8.

    The imm8 ceiling itself no longer binds: all three rate-carrying pushes are widened
    to `push imm32` in the cave at STANDALONE_CAVE_VA. What remains is the width check --
    a generated site must be exactly as wide as the reference site it replaces.
    """
    problems = []
    for s in sites:
        if s.ref is None:
            continue
        if s.out is not None and len(s.out) != len(s.ref):
            problems.append("%08X: generated %d bytes but the reference writes %d"
                            % (s.real_va, len(s.out), len(s.ref)))
    return problems


COMMENT = re.compile("<!--.*?-->", re.S)


def xml_comment_safe(text):
    """Make every <!-- ... --> in `text` legal XML.

    XML forbids the sequence `-`+`-` anywhere inside a comment and forbids a comment
    body ending in a single `-`. Prose written with a double hyphen as a dash trips
    both. This cost a whole build: gplexegen.ps1 validates every XML with .NET's
    XmlDocument.Load before it hands anything to GPLPatchDLL, so an illegal comment
    in the generated patch refused the run outright -- and the patch DATA was fine.

    Runs of hyphens are collapsed rather than deleted, so the prose still reads.
    """

    def fix(m):
        body = m.group(0)[4:-3]
        body = re.sub("-{2,}", "-", body)
        if body.endswith('-'):
            body = body[:-1] + chr(0x2010)   # U+2010 HYPHEN, not the ASCII one
        return '<!--' + body + '-->'

    return COMMENT.sub(fix, text)


def emit(sites, rate, substeps, physics_hz, ref_name):
    out = io.StringIO()
    out.write("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<GPLPatch>\n\n")
    out.write("   <!-- GPL %g fps patch, generated by tools/gplrategen.py\n" % rate)
    out.write("        from %s, which is the reference for every code site\n" % ref_name)
    out.write("        and for the rate^k exponent of every scaled constant.\n\n")
    out.write("        sim rate      %g Hz  (stock 36)\n" % rate)
    out.write("        timer rate    %g Hz  (2x sim)\n" % (2 * rate))
    out.write("        physics       %g Hz = %g x %d substeps per tick, exact\n"
              % (physics_hz, rate, substeps))
    out.write("        The AI's OTHER half is in gpla67.ini, in ticks, and no exe\n")
    out.write("        patch touches it. See STATUS.md. -->\n\n")

    for s in sites:
        out.write("   <BinaryPatch>\n")
        out.write("      <!-- %s%s -->\n"
                  % (s.note, "" if s.kind != "unclassified" else "  [UNCLASSIFIED]"))
        out.write("      <VirtualAddress>0x%08X</VirtualAddress>\n" % s.xml_va)
        out.write("      <Data>%s</Data>\n" % s.out.hex().upper())
        out.write("   </BinaryPatch>\n\n")

    out.write("</GPLPatch>\n")
    return xml_comment_safe(out.getvalue())


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rate", type=float, required=True,
                    help="target simulation rate in Hz")
    gplpatch.add_game_dir_arg(ap)
    ap.add_argument("--exe",
                    help="UNPATCHED reference exe to read stock values from. "
                         "Default: <game-dir>/gplc67_36.exe")
    ap.add_argument("--reference",
                    help="the known-good 60 fps patch the rules are derived from. "
                         "Default: <game-dir>/tools/60FPS/60fpsv2/60FPSv2.xml")
    ap.add_argument("--out", help="write the XML here (default: stdout)")
    ap.add_argument("--companion",
                    help="another patch this one will be applied alongside. Every address "
                         "is compared and the run is REFUSED if both write the same bytes "
                         "with different values. GPLPatchDLL has no collision detection of "
                         "its own, so this is the only thing that catches it.")
    ap.add_argument("--force", action="store_true",
                    help="emit despite a refusal. The result WILL be wrong.")
    args = ap.parse_args()

    exe = gplpatch.game_file(args, "donor", "--exe",
                             "the unpatched 36 Hz reference exe", "gplc67_36.exe")
    reference = gplpatch.game_file(args, "ref60", "--reference",
                                   "the 60 fps reference patch",
                                   "tools", "60FPS", "60fpsv2", "60FPSv2.xml")
    args.exe, args.reference = exe, reference

    image, sites, substeps, physics_hz = build(args.exe, args.reference, args.rate)

    kinds = {}
    for s in sites:
        kinds[s.kind] = kinds.get(s.kind, 0) + 1
    sys.stderr.write("exe        : %s\n" % args.exe)
    sys.stderr.write("reference  : %s\n" % os.path.basename(args.reference))
    sys.stderr.write("rate       : %g Hz  (timer %g, physics %g = %g x %d)\n"
                     % (args.rate, 2 * args.rate, physics_hz, args.rate, substeps))
    sys.stderr.write("sites      : %d  (%s)\n"
                     % (len(sites), ", ".join("%s %d" % (k, v) for k, v in sorted(kinds.items()))))

    unclassified = [s for s in sites if s.kind == "unclassified"]
    if unclassified:
        sys.stderr.write(
            "\n%d site(s) could not be classified as either a rate^k constant or inserted\n"
            "code. They are COPIED FROM THE REFERENCE UNCHANGED, which is correct only if\n"
            "they are rate-independent. Check them before trusting the patch:\n" % len(unclassified))
        for s in unclassified:
            sys.stderr.write("    %08X  stock %-16s ref %s\n"
                             % (s.real_va, s.stock.hex().upper(), s.ref.hex().upper()))

    problems = []
    overflowed = [s for s in sites if s.kind == "overflow"]
    if overflowed:
        problems.append(
            "%d site(s) cannot hold the scaled value at %g Hz:\n%s\n"
            "    These carry the rate in a fixed-width instruction immediate. The three\n"
            "    known ones are widened into the cave at %08X; a new one means another\n"
            "    entry in STANDALONE_TASK_SITES, or a lower rate."
            % (len(overflowed), args.rate,
               "\n".join("      %08X  %s" % (s.real_va, s.note) for s in overflowed),
               STANDALONE_CAVE_VA))

    if args.companion:
        # Map the companion's writes by address so an overlap can be COMPARED, not merely
        # detected. A site both patches write is fine if they agree -- the companion has it
        # covered and we drop it. If they disagree, one of us is wrong and neither should
        # win by ordering, which is exactly what GPLPatchDLL would let happen: it has no
        # collision detection at all and silently lets the later XML win.
        companion = {}
        ctext = io.open(args.companion, encoding="utf-8-sig", errors="replace").read()
        for addr, hexdata in PAT.findall(ctext):
            va = image.fixup(int(addr, 16))
            companion[va] = bytes.fromhex(hexdata)
        cbytes = set()
        for va, blob in companion.items():
            for i in range(len(blob)):
                cbytes.add(va + i)
        sys.stderr.write("companion   : %s (%d sites, %d bytes)\n"
                         % (os.path.basename(args.companion), len(companion), len(cbytes)))

        agreed, disagreed, kept = [], [], []
        for site in sites:
            overlap = any(site.real_va + i in cbytes for i in range(len(site.out)))
            if not overlap:
                kept.append(site)
                continue
            theirs = companion.get(site.real_va)
            if theirs is None or len(theirs) != len(site.out):
                disagreed.append((site, theirs, "partial or misaligned overlap"))
                continue
            if theirs == site.out:
                agreed.append((site, "identical bytes"))
                continue
            # Floats can differ only by the other author's hand-rounding. Compare as
            # numbers before calling it a disagreement.
            same = False
            if len(theirs) == 4:
                try:
                    a, = struct.unpack("<f", theirs)
                    b, = struct.unpack("<f", site.out)
                    same = approx(a, b, 5e-3)
                except struct.error:
                    same = False
            if same:
                agreed.append((site, "same value, %s rounds differently"
                               % os.path.basename(args.companion)))
            else:
                disagreed.append((site, theirs, "different values"))

        sites = kept
        if agreed:
            sys.stderr.write(
                "  %d site(s) already covered by the companion and dropped:\n" % len(agreed))
            for site, why in agreed:
                sys.stderr.write("      %08X  %s\n" % (site.real_va, why))
        if disagreed:
            problems.append(
                "%d site(s) are written by BOTH patches with different values. Stacking\n"
                "    them would let load order decide, silently:\n%s"
                % (len(disagreed),
                   "\n".join("      %08X  companion %-12s ours %-12s  (%s)"
                             % (s.real_va,
                                t.hex().upper() if t else "-",
                                s.out.hex().upper(), why)
                             for s, t, why in disagreed)))
        if not agreed and not disagreed:
            sys.stderr.write("  no overlap with the companion\n")
        sys.stderr.write("  emitting %d site(s) after the companion check\n" % len(sites))

    problems += check_ceiling(args.rate, sites)
    if problems:
        sys.stderr.write("\nREFUSING TO GENERATE.\n\n")
        for p in problems:
            sys.stderr.write("  %s\n\n" % p)
        if not args.force:
            return 2
        sys.stderr.write("  --force given: emitting anyway. The patch will be WRONG.\n\n")

    xml = emit(sites, args.rate, substeps, physics_hz, os.path.basename(args.reference))
    if args.out:
        io.open(args.out, "w", encoding="utf-8", newline="\n").write(xml)
        sys.stderr.write("\nwrote %s\n" % args.out)
        sys.stderr.write("Apply with tools/gplexegen.ps1 by appending it to the XML list.\n")
    else:
        sys.stdout.write(xml)
    return 0


if __name__ == "__main__":
    sys.exit(main())
