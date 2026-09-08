#!/usr/bin/env python3
r"""Rescale GPL's AI parameter ini from 36 Hz to an arbitrary simulation rate.

WHY THIS IS NEEDED

The exe patch is only half of a rate change. GPL's AI reads its tunables from an ini --
`gpla67.ini` for gplc67.exe, `gpl_ai.ini` for vanilla gpl.exe -- and most of the
time-valued ones are **in ticks**, not seconds. Raise the tick rate and leave the ini
alone and every AI interval fires early by exactly the rate ratio: at 144 Hz a lookahead
of 108 ticks is 0.75 s instead of 3 s, and a gear shift takes 42 ms instead of 167 ms.

WHERE THE EXPONENTS COME FROM: an oracle, not a guess

Two files shipped by the 60 fps patch authors form a known-good 36 -> 60 pair:

    gpl_ai.ini.98                          stock, 36 Hz
    tools/60FPS/60fpsv2/gpl_ai_60fps.ini   the same file at 60 Hz, by hand

Same 265 keys, no additions, no removals. Dividing one by the other and matching the
result against (60/36)^k reads the exponent straight off a play-tested artifact:

    rate^0   139 keys      rate^+1   58 keys      rate^-1   11 keys
    rate^-2   17 keys      rate^+2    2 keys      no signal (0 in both)  37 keys
    one key is neither: aggression_trans_time_scaling 1.0 -> 0.75, a hand-tune

This is the same method `gplrategen.py` uses on the exe: take the exponent from a patch
that is known to work rather than deriving it from first principles and hoping.

THE INDEPENDENT CHECK, AND THE FOUR PLACES IT BITES

Every key in these files carries its unit in a trailing comment, so there is a second,
completely independent classifier. Running both: **90 keys agree, 4 disagree, and every
disagreement is the oracle saying "unchanged" where the comment names a real unit.**

Three of the four are genuine misses in the shipped 60 fps ini and are corrected here.
The fourth is the comment parser being wrong, and the oracle is right:

  [GP] nominal_max_accel, nominal_traction_circle -- "accel. in meters/tick^2
      (1G = 0.007562)". 0.0075668 IS 9.80665/36^2, and 0.011154/0.0075668 = 1.474, which
      the comment states as "1.475 * ONE_G". These are unambiguously rate^-2 and the 60 fps
      ini left them at the 36 Hz value. The exe patch DOES rescale its own ONE_G constant
      (gpl_ai_ONE_G_perTickSq, 0x53A758), so an unscaled ini value is compared against a
      scaled one: at 144 Hz the AI would believe it had sixteen times the grip.
      CORRECTED.

  [physics] gear_shift_rpm_decrement -- "during gear shift decrement RPM each tick by this
      amount". A per-tick rate of change, so rate^-1, NOT the rate^+1 that the word "tick"
      alone suggests. Second, independent argument for the same answer: the shift lasts
      base_gearshift_hiatus ticks, which IS scaled rate^+1, so the per-tick decrement must
      fall by the same factor for the total RPM drop across a shift to stay put.
      CORRECTED, to rate^-1.

  [physics] rolling_drag_amt -- "value * external_N_accel = drag (in meters/tick^2)".
      The "meters/tick^2" describes the DRAG, not the value: an acceleration times this
      gives an acceleration, so the value is dimensionless. The oracle is right and the
      naive comment reading is wrong. NOT changed. (Contrast
      suspension_mechanical_drag_coeff, "meters/tick^2 drag", which IS the drag itself and
      which the oracle does scale rate^-2.)

THE FIFTH CORRECTION: the dlat spring/damper gains, in every section

  The reference scales `cornering_dlat_accel_k1/k2` by rate^-2 and leaves the plain
  `dlat_accel_k1/k2` alone. Neither matches the dimensions of a spring/damper producing an
  acceleration goal, where k1 is 1/tick^2 and k2 is 1/tick. Rate-invariance again settles
  it, and the two errors have different characters: the plain pair ends up a controller
  four times too STIFF at 144 Hz, and the cornering pair ends up badly UNDER-DAMPED
  (zeta 0.634 -> 0.159). See KEY_OVERRIDES for the full working.

`--no-overrides` drops every correction and reproduces the reference exactly.

THE FOURTH CORRECTION, AND THE TEST THAT SETTLED IT

  [physics] max_lat_acc_from_speed -- the reference writes 0.0125 -> 0.0045, a factor of
      (36/60)^2, under a comment its own author wrote: "unit should be 1/tick so
      multiplying by 36/60". 36/60 would have given 0.0075. He shipped the square.

      This was left as the reference had it until the first 144 Hz drive, where the AI
      cars would not exceed about 90 km/h. THE TEST THAT SETTLES IT is rate-invariance:
      a correct unit conversion must reproduce the same PHYSICAL quantity at every rate.
      Feeding in the .LP top speed:

          36 Hz  as shipped     0.0125       ->  3.95 G of lateral allowance
          60 Hz  rate^-2        0.0045       ->  2.37 G     rate^-1  0.0075     -> 3.95 G
         144 Hz  rate^-2        0.00078125   ->  0.99 G     rate^-1  0.003125   -> 3.95 G

      Only rate^-1 is invariant. rate^-2 quietly removes lateral grip in proportion to the
      rate -- a 40% handicap at 60, which matches the 60 fps readme's "AI is not working
      well", and a 4x handicap at 144, where corner speed goes as sqrt(a) and so halves.
      CORRECTED. `--no-overrides` still reproduces the reference exactly.

USAGE

    python tools/gplaiscale.py --rate 144 --input "H:\...\gpla67.ini" --in-place
    python tools/gplaiscale.py --self-test          # rebuild the 60 Hz reference and diff

Only lines whose value actually changes are rewritten; comments, blank lines, section
order, spelling and column alignment are preserved byte for byte everywhere else.
"""

import argparse
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gplpatch  # noqa: E402  (only for the --game-dir helpers)

STOCK_RATE = 36.0

DEFAULT_REFERENCE_RATE = 60.0

# (section, key) -> (exponent, why). Applied on top of the oracle. Every one of these is a
# place the reference patch left a dimensional constant at its 36 Hz value; see the module
# docstring for the reasoning behind each.
OVERRIDES = {
    ("GP", "nominal_max_accel"):
        (-2, "meters/tick^2; the comment gives 1G = 0.007562 = 9.80665/36^2"),
    ("GP", "nominal_traction_circle"):
        (-2, "meters/tick^2; 0.011154 = 1.474 * ONE_G, as its own comment says"),
    ("physics", "gear_shift_rpm_decrement"):
        (-1, "RPM per tick, a rate of change; the shift's length scales rate^+1"),
    ("physics", "max_lat_acc_from_speed"):
        (-1, "value * speed(m/tick) = accel(m/tick^2), so the value is 1/tick. Proved by "
             "rate-invariance: only rate^-1 reproduces stock's 3.95 G at every rate"),

    # The yaw and alt spring/damper pairs. Same controller form as dlat_accel_k1/k2 and
    # confirmed from the DISASSEMBLY this time, not inferred from names: sub_4982F0 is the
    # per-tick yaw integrator (yaw += vel + accel/2; vel += accel with accel =
    # scalings*k1*(target-yaw) - k2*vel), and sub_497AB0 is the same Heun step for the
    # fake-physics ride height (accel = (0.43152-alt)*alt_k1 - vel*alt_k2). dt = 1 tick is
    # implicit in both, so k1 is 1/tick^2 and k2 is 1/tick. The 60 fps reference left all
    # four at their 36 Hz values -- the same class of miss as the dlat pair, found in the
    # 2026-09-04 audit of every AI constant against its consumer.
    ("physics", "yaw_accel_k1"):
        (-2, "spring gain of the per-tick yaw integrator sub_4982F0, 1/tick^2"),
    ("physics", "yaw_accel_k2"):
        (-1, "damping gain of the yaw integrator on a rad/tick velocity, 1/tick"),
    ("physics", "alt_accel_k1"):
        (-2, "spring gain of the fake-physics height integrator sub_497AB0, 1/tick^2"),
    ("physics", "alt_accel_k2"):
        (-1, "damping gain of the height integrator on a m/tick velocity, 1/tick"),
}

# Overrides that apply to a key in EVERY section it appears in. The dlat spring/damper
# gains are the whole of this table, and the reference is internally inconsistent about
# them: it scales the `cornering_` pair rate^-2 and leaves the plain pair alone. Neither
# branch matches the dimensions.
#
# The controller produces a lateral ACCELERATION GOAL from a position error and a velocity:
#
#     a = k1 * (x_target - x)  -  k2 * v          [a]=m/tick^2  [x]=m  [v]=m/tick
#
# so k1 is 1/tick^2 (rate^-2) and k2 is 1/tick (rate^-1). Both are confirmed by
# rate-invariance -- express each as a per-SECOND quantity and it must not move:
#
#   k1  0.0045 at 36 Hz  = 0.0045 * 36^2  = 5.83 1/s^2
#       rate^-2 at 144   = 0.00028125     -> 5.83 1/s^2   invariant
#       reference rate^0 = 0.0045         -> 93.3 1/s^2   SIXTEEN times too stiff
#   k2  0.1202 at 36 Hz  = 0.1202 * 36    = 4.33 1/s
#       rate^-1 at 144   = 0.03005        -> 4.33 1/s     invariant
#       reference rate^0 = 0.1202         -> 17.3 1/s     four times too damped
#
# What each error DOES, which is why this is not cosmetic:
#
#   * plain pair, both left at rate^0: the damping ratio survives (both are wrong by the
#     same factor) but the natural frequency does not -- sqrt(93.3) vs sqrt(5.83) is a
#     controller FOUR TIMES too stiff at 144 Hz. It darts at the racing line, demands
#     large lateral accelerations, and eats the traction circle that the longitudinal
#     controller needs, which shows up as cars that will not go fast.
#   * cornering pair, both at rate^-2: k1 is right by luck, k2 is not, so the damping
#     ratio collapses -- zeta 0.634 at 36 Hz becomes 0.159 at 144. Badly under-damped,
#     i.e. weaving through corners.
#
# At 60 Hz both errors are a factor of 1.67 and merely make the AI poor. At 144 they are
# a factor of 4 and 16.
KEY_OVERRIDES = {
    "dlat_accel_k1":
        (-2, "spring gain on a position error, 1/tick^2"),
    "dlat_accel_k2":
        (-1, "damping gain on a m/tick velocity, 1/tick"),
    "cornering_dlat_accel_k1":
        (-2, "spring gain on a position error, 1/tick^2 (the reference agrees here)"),
    "cornering_dlat_accel_k2":
        (-1, "damping gain on a m/tick velocity, 1/tick; the reference used rate^-2 and "
             "collapsed the damping ratio"),

    # THE AVOIDANCE TIMER IS A FRACTION, NOT A DURATION. gpl_aiState_evaluateBlocker
    # computes
    #
    #     distanceMetres / (car+510 - blocker.car+510)   [m / (m/tick)]  =  TICKS
    #         * map[+2984]                               (1.0, or the long_term coeff)
    #         * avoid_time_coeff
    #
    # and compares the result against a counter of consecutive ticks. The time to
    # collision is already in ticks at the CURRENT rate, so the coefficient is a pure
    # fraction -- which is what its own ini comment says: "value * est. time to collision
    # = ticks before do obstacle avoidance".
    #
    # The reference scaled it rate^+1 and left `long_term_avoid_time_coeff` alone, which
    # cannot both be right: the two multiply into the SAME expression. rate^0 is the only
    # reading consistent with the reference's own choices, with the comment, and with the
    # units.
    #
    # Effect of the reference's version at 144 Hz: the bot keeps doing the reactive
    # lateral dodge four times longer in wall-clock before escalating to a planned pass,
    # so it hovers alongside instead of committing or backing off.
    "avoid_time_coeff":
        (0, "a fraction of the time-to-collision, which is ALREADY in current-rate ticks; "
            "the reference scaled it rate^+1 while leaving long_term_avoid_time_coeff -- "
            "which multiplies into the same expression -- at rate^0"),
    "cornering_avoid_time_coeff":
        (0, "the cornering twin of avoid_time_coeff, same expression, same units"),
    "long_term_avoid_time_coeff":
        (0, "multiplies into the same product as avoid_time_coeff; the reference already "
            "left this one alone, and pinning it makes the pair explicit"),
}

# Keys the reference changed for reasons that are not a unit conversion. Never scaled;
# always reported.
HAND_TUNED = {
    ("driver_modeling", "aggression_trans_time_scaling"):
        "reference sets 1.0 -> 0.75, which is not a power of the rate ratio",
}

VALUE_LINE = re.compile(
    r"^(?P<lead>\s*)(?P<key>[A-Za-z_][A-Za-z0-9_]*)(?P<mid>\s*=\s*)"
    r"(?P<value>-?[0-9]+(?:\.[0-9]*)?|-?\.[0-9]+)(?P<pad>[ \t]*)(?P<rest>;.*)?$")
SECTION_LINE = re.compile(r"^\s*\[\s*(?P<name>.+?)\s*\]\s*$")


def read_ini(path):
    """(section, key) -> value, plus (section, key) -> trailing comment."""
    lines = io.open(path, encoding="latin1", newline="").read().splitlines(True)
    values, comments, section = {}, {}, None
    for line in lines:
        if line.lstrip().startswith(";"):
            continue
        m = SECTION_LINE.match(line.split(";", 1)[0])
        if m:
            section = m.group("name")
            continue
        m = VALUE_LINE.match(line.rstrip("\r\n"))
        if m and section is not None:
            key = (section, m.group("key"))
            values[key] = float(m.group("value"))
            comments[key] = (m.group("rest") or "").lstrip("; ").strip()
    return values, comments, lines


def unit_exponent(comment):
    """The exponent the trailing comment implies, or None. A CROSS-CHECK ONLY.

    Deliberately not used to decide anything: it is wrong on rolling_drag_amt and on
    gear_shift_rpm_decrement, and being wrong twice out of four disagreements is not an
    authority. Its job is to make disagreements with the oracle visible.
    """
    t = comment.lower()
    if "meters/tick^2" in t or "meters/tick2" in t:
        return -2, "meters/tick^2"
    if "meters/tick" in t or "ms./tick" in t or "ms/tick" in t:
        return -1, "meters/tick"
    if "in seconds" in t:
        return 0, "seconds"
    if "tick" in t:
        return 1, "ticks"
    if "in meters" in t:
        return 0, "meters"
    if "chance in" in t or "%" in t:
        return 0, "percent"
    return None, ""


def derive_exponents(stock_path, ref_path, ref_rate):
    """Read the exponent of every key off the known-good 36 -> ref_rate pair."""
    stock, comments, _ = read_ini(stock_path)
    ref, _, _ = read_ini(ref_path)

    differ = sorted(set(stock) ^ set(ref))
    if differ:
        sys.exit("REFUSED: the oracle pair does not have the same keys (%d differ), so it "
                 "is not a 36 -> %g Hz version of one file:\n  %s"
                 % (len(differ), ref_rate,
                    "\n  ".join("%s / %s" % k for k in differ[:10])))

    ratio = ref_rate / STOCK_RATE
    exps, unclassified, silent = {}, [], []
    for key in stock:
        a, b = stock[key], ref[key]
        if a == 0.0 or b == 0.0:
            silent.append(key)
            continue
        for e in (2, 1, 0, -1, -2, 3, -3):
            if abs(b / a - ratio ** e) <= 0.02 * ratio ** e:
                exps[key] = e
                break
        else:
            unclassified.append((key, a, b, b / a))
    return exps, comments, silent, unclassified


def resolve(key, exps, use_overrides):
    """(exponent, source, note) for one key of the file being rescaled."""
    if key in HAND_TUNED:
        return 0, "hand-tuned", HAND_TUNED[key]
    if use_overrides and key in OVERRIDES:
        return OVERRIDES[key][0], "override", OVERRIDES[key][1]
    if use_overrides and key[1] in KEY_OVERRIDES:
        return KEY_OVERRIDES[key[1]][0], "override", KEY_OVERRIDES[key[1]][1]
    if key in exps:
        return exps[key], "oracle", ""

    # Not in the oracle at all -- gpla67.ini adds one key vanilla does not have. Fall back
    # to the same key NAME in other sections, but only when they are unanimous.
    same = {e for (sec, k), e in exps.items() if k == key[1]}
    if len(same) == 1:
        return same.pop(), "by name", "not in the oracle; every other section agrees"
    return 0, "unknown", "no signal from the oracle and no unanimous sibling"


def format_value(new, old_text):
    """Fewest decimals that carry the value, never fewer than the original had."""
    want = len(old_text.split(".")[1]) if "." in old_text else 1
    for digits in range(max(want, 1), 13):
        text = "%.*f" % (digits, new)
        if float(text) == 0.0 and new != 0.0:
            continue
        if abs(float(text) - new) <= abs(new) * 1e-9:
            return text
    return "%.12f" % new


def rescale(path, rate, exps, use_overrides, report):
    lines = io.open(path, encoding="latin1", newline="").read().splitlines(True)
    ratio = rate / STOCK_RATE
    out, section = [], None
    counts, changed, notes, unknown = {}, 0, [], []

    for line in lines:
        if line.lstrip().startswith(";"):
            out.append(line)
            continue
        m = SECTION_LINE.match(line.split(";", 1)[0])
        if m:
            section = m.group("name")
            out.append(line)
            continue
        stripped = line.rstrip("\r\n")
        eol = line[len(stripped):]
        m = VALUE_LINE.match(stripped)
        if not m or section is None:
            out.append(line)
            continue

        key = (section, m.group("key"))
        exponent, source, note = resolve(key, exps, use_overrides)
        counts[exponent] = counts.get(exponent, 0) + 1
        if source == "unknown":
            # Carry the value: an unclassified key that is 0.0 is harmless whatever its
            # exponent would have been, and one that is not is the only kind worth
            # anyone's time. Nearly all of these are zero in both oracle files precisely
            # BECAUSE they are switches and unused knobs.
            unknown.append((key, float(m.group("value"))))
        if note:
            notes.append((key, source, exponent, note))

        old = float(m.group("value"))
        if exponent == 0 or old == 0.0:
            out.append(line)
            continue

        new = old * (ratio ** exponent)
        text = format_value(new, m.group("value"))
        # Hold the comment column: give back whatever width the number gained or lost.
        pad = m.group("pad")
        if pad:
            pad = " " * max(1, len(pad) + len(m.group("value")) - len(text))
        out.append("%s%s%s%s%s%s%s" % (m.group("lead"), m.group("key"), m.group("mid"),
                                       text, pad, m.group("rest") or "", eol))
        changed += 1
        if report:
            sys.stderr.write("   %-24s %-34s %-13s -> %-13s rate^%+d  (%s)\n"
                             % (key[0], key[1], m.group("value"), text, exponent, source))

    return "".join(out), counts, changed, notes, unknown


def cross_check(exps, comments, use_overrides):
    """Where the documented unit disagrees with the oracle, and what we did about it."""
    rows = []
    for key, exponent in sorted(exps.items()):
        ue, label = unit_exponent(comments.get(key, ""))
        if ue is not None and ue != exponent:
            effective = resolve(key, exps, use_overrides)[0]
            rows.append((key, exponent, ue, label, effective))
    return rows


def parse_values(text):
    values, section = {}, None
    for line in text.splitlines():
        if line.lstrip().startswith(";"):
            continue
        m = SECTION_LINE.match(line.split(";", 1)[0])
        if m:
            section = m.group("name")
            continue
        m = VALUE_LINE.match(line)
        if m and section:
            values[(section, m.group("key"))] = float(m.group("value"))
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rate", type=float, help="target simulation rate in Hz")
    ap.add_argument("--input", help="the 36 Hz ini to rescale (gpla67.ini, gpl_ai.ini)")
    ap.add_argument("--out", help="where to write it (default: stdout)")
    ap.add_argument("--in-place", action="store_true",
                    help="rewrite --input, keeping a .bak36 copy of the original")
    ap.add_argument("--stock",
                    help="oracle: the stock 36 Hz ini. "
                         "Default: <game-dir>/gpl_ai.ini.98")
    ap.add_argument("--reference",
                    help="oracle: the same file rescaled by hand. "
                         "Default: <game-dir>/tools/60FPS/60fpsv2/gpl_ai_60fps.ini")
    gplpatch.add_game_dir_arg(ap)
    ap.add_argument("--reference-rate", type=float, default=DEFAULT_REFERENCE_RATE,
                    help="the rate --reference is written for")
    ap.add_argument("--no-overrides", action="store_true",
                    help="do not correct the dimensional keys the reference missed")
    ap.add_argument("--report", action="store_true", help="print every changed line")
    ap.add_argument("--self-test", action="store_true",
                    help="rescale the stock ini to the reference rate and diff against "
                         "the reference. Uses --no-overrides semantics by definition.")
    args = ap.parse_args()

    args.stock = gplpatch.game_file(args, "ini36", "--stock",
                                    "the stock 36 Hz AI ini", "gpl_ai.ini.98")
    args.reference = gplpatch.game_file(args, "ref60", "--reference",
                                        "the hand-rescaled 60 Hz AI ini",
                                        "tools", "60FPS", "60fpsv2", "gpl_ai_60fps.ini")

    exps, comments, silent, unclassified = derive_exponents(
        args.stock, args.reference, args.reference_rate)

    sys.stderr.write("oracle      : %s -> %s  (%g -> %g Hz)\n"
                     % (os.path.basename(args.stock), os.path.basename(args.reference),
                        STOCK_RATE, args.reference_rate))
    buckets = {}
    for e in exps.values():
        buckets[e] = buckets.get(e, 0) + 1
    sys.stderr.write("              %d keys classified: %s ; %d silent (zero in both) ; "
                     "%d neither\n"
                     % (len(exps),
                        ", ".join("rate^%+d x%d" % (e, n)
                                  for e, n in sorted(buckets.items(), reverse=True)),
                        len(silent), len(unclassified)))
    for (sec, k), a, b, r in unclassified:
        sys.stderr.write("                %s / %s: %g -> %g, ratio %.5f is not a power of "
                         "%.4f -- treated as a hand-tune, left unscaled\n"
                         % (sec, k, a, b, r, args.reference_rate / STOCK_RATE))

    if args.self_test:
        text, counts, changed, notes, unknown = rescale(
            args.stock, args.reference_rate, exps,
            use_overrides=False, report=False)
        rebuilt = parse_values(text)
        want, _, _ = read_ini(args.reference)
        # Two very different kinds of difference, and lumping them together hides the
        # only one that matters. The reference writes hand-rounded decimals -- 5.56 for
        # 100/18, 0.0036 for 0.0036363... -- so a sub-percent gap is the reference being
        # less precise than us, not a disagreement about the exponent.
        rounded, bad = [], []
        for k in sorted(want):
            got, wanted = rebuilt.get(k, 0.0), want[k]
            if abs(got - wanted) <= max(abs(wanted) * 1e-9, 1e-12):
                continue
            (rounded if abs(got - wanted) <= abs(wanted) * 0.02
             else bad).append((k, got, wanted))
        sys.stderr.write("\nself-test   : rebuilt %d keys at %g Hz. %d match exactly, %d "
                         "differ only by the reference's rounding, %d really differ.\n"
                         % (len(rebuilt), args.reference_rate,
                            len(rebuilt) - len(rounded) - len(bad), len(rounded), len(bad)))
        for k, got, wanted in rounded:
            sys.stderr.write("     rounding %-22s %-30s ours %-12g reference %g\n"
                             % (k[0], k[1], got, wanted))
        for k, got, wanted in bad:
            tail = "   (expected: hand-tuned)" if k in HAND_TUNED else ""
            sys.stderr.write("     DIFFERS  %-22s %-30s ours %-12g reference %g%s\n"
                             % (k[0], k[1], got, wanted, tail))
        return 0 if all(k in HAND_TUNED for k, _, _ in bad) else 1

    if not args.rate or not args.input:
        sys.exit("--rate and --input are required (or use --self-test)")

    # Is this actually a 36 Hz file? Compare its tick-valued keys against the stock ini.
    values, _, _ = read_ini(args.input)
    stock, _, _ = read_ini(args.stock)
    probes = sorted(values[k] / stock[k] for k in values
                    if k in stock and exps.get(k) == 1 and stock[k] and values[k])
    if probes:
        median = probes[len(probes) // 2]
        sys.stderr.write("source rate : %d tick-valued keys compared against the stock "
                         "ini, median ratio %.4f -- " % (len(probes), median))
        if abs(median - 1.0) > 0.05:
            sys.stderr.write("NOT 36 Hz.\n")
            sys.exit("REFUSED: %s looks like it is already written for about %.0f Hz. "
                     "Rescaling it again would compound the error, so start from a 36 Hz "
                     "file." % (os.path.basename(args.input), STOCK_RATE * median))
        sys.stderr.write("consistent with 36 Hz.\n")

    if args.report:
        sys.stderr.write("\nchanged lines:\n")
    text, counts, changed, notes, unknown = rescale(
        args.input, args.rate, exps, use_overrides=not args.no_overrides,
        report=args.report)

    sys.stderr.write("\ninput       : %s (%d keys)\n"
                     % (os.path.basename(args.input), sum(counts.values())))
    sys.stderr.write("rate        : %g Hz, ratio %.6f\n"
                     % (args.rate, args.rate / STOCK_RATE))
    sys.stderr.write("exponents   : %s\n"
                     % ", ".join("rate^%+d x%d" % (e, n)
                                 for e, n in sorted(counts.items(), reverse=True)))
    sys.stderr.write("changed     : %d lines rewritten; every other byte is untouched\n"
                     % changed)

    sys.stderr.write("\ncross-check : the trailing-comment units against the oracle\n")
    rows = cross_check(exps, comments, not args.no_overrides)
    if not rows:
        sys.stderr.write("  they agree everywhere\n")
    for (sec, k), oe, ue, label, effective in rows:
        state = ("CORRECTED to rate^%+d" % effective if effective != oe
                 else "kept at rate^%+d" % oe)
        sys.stderr.write("  %-9s %-26s oracle rate^%+d, comment %-14s rate^%+d  ->  %s\n"
                         % (sec, k, oe, label, ue, state))

    if notes:
        sys.stderr.write("\ndecisions that were not simply the oracle:\n")
        for (sec, k), source, e, note in notes:
            sys.stderr.write("  %-15s %-30s rate^%+d  [%s] %s\n"
                             % (sec, k, e, source, note))
    if unknown:
        live = [(k, v) for k, v in unknown if v != 0.0]
        sys.stderr.write("\nUNCLASSIFIED, left unscaled: %d keys, of which %d are non-zero "
                         "and so are the only ones that can matter\n"
                         % (len(unknown), len(live)))
        for (sec, k), v in live:
            sys.stderr.write("  NON-ZERO  %-16s %-36s = %g\n" % (sec, k, v))
        if len(unknown) > len(live):
            sys.stderr.write("  the other %d are 0.0 in this file, so no exponent would "
                             "change them\n" % (len(unknown) - len(live)))

    banner = (
        "; ======================================================================\n"
        "; RESCALED FOR %g Hz by tools/gplaiscale.py. Do not hand-edit a value\n"
        "; here without rescaling it too.\n"
        ";\n"
        "; Source: %s, at %g Hz.\n"
        "; Exponents read off the %g -> %g Hz pair %s / %s.\n"
        "; %d of %d values rewritten. The time-valued keys in this file are in\n"
        "; TICKS, so they only mean what they say at the rate named above.\n"
        "; ======================================================================\n"
        % (args.rate, os.path.basename(args.input), STOCK_RATE, STOCK_RATE,
           args.reference_rate, os.path.basename(args.stock),
           os.path.basename(args.reference), changed, sum(counts.values())))
    text = banner + text

    if args.in_place:
        backup = args.input + ".bak36"
        if os.path.exists(backup):
            sys.stderr.write("\nbackup      : %s already exists, kept as it was\n" % backup)
        else:
            io.open(backup, "w", encoding="latin1", newline="").write(
                io.open(args.input, encoding="latin1", newline="").read())
            sys.stderr.write("\nbackup      : %s\n" % backup)
        io.open(args.input, "w", encoding="latin1", newline="").write(text)
        sys.stderr.write("wrote       : %s\n" % args.input)
    elif args.out:
        io.open(args.out, "w", encoding="latin1", newline="").write(text)
        sys.stderr.write("\nwrote       : %s\n" % args.out)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
