"""Optional force-feedback XMLs for the 144 fps patch.

These are NOT rate fixes. Nothing here depends on the tick rate at all -- GPL's FFB
averager runs inside the physics substep loop, so it executes 288 times a second whether
the game ticks at 36 Hz or 144. They are offered as switches because they change how the
wheel feels, which is a preference, not a defect.

Two independent patches:

    --out-spring-damper  Switch off the centring spring and the damper that GPL installs
                         in the wheel itself, leaving only the force the physics computes.

    --out-curve          Deepen the notch in the force curve, which is what GEM's
                         `ffb3test1.xml` was trying to do before it went wrong.

Each refuses unless the stock bytes are exactly what it expects.

WHAT ffb3test1.xml ACTUALLY DOES, and why this file exists
----------------------------------------------------------
The curve object at 0x55BA84 is `{float domainMax = 5.0; int count = 313; float y[313]}`,
running to 0x55BF6C. y is symmetric about index 157: it rises from +0.12044 to a peak of
+0.23055 at i=123, crosses zero, and plunges to a notch bottom of -1.20 at i=157, then
mirrors back. (Entries 0 and 1 are both 0.12044, a duplicated first sample.)

ffb3test1.xml writes 315 contiguous dwords starting at 0x55BA84 -- the HEADER's address,
not the table's. Classifying every one of them against the stock image:

    0x55BA84..0x55BCF4   157 entries   equal to stock shifted 4 entries EARLIER
    0x55BCF8..0x55BD00     3 entries   -1.33, -1.28, -1.28  (match neither: hand edits)
    0x55BD04..0x55BF6C   155 entries   byte-identical to stock (no-ops)

So two thirds of the file is a misalignment and a third of it does nothing. The
misalignment overwrites `domainMax` with 0.12094 and the integer count 313 with
0x3DF8B589 = 1039791497, and it desynchronises the two limbs of a curve whose whole
structure is a mirror -- the ascending side moves, the descending side does not.

The three hand edits are the only real content, they are round numbers, and they sit in
the notch. **The destination is recoverable -- a deeper notch -- but the author's exact
intended shape is not**: as written the three land at indices 155, 156, 157 as
(-1.33, -1.28, -1.28), which is neither symmetric about the bottom nor consistent with any
single-offset correction of the shift. So this does not try to replicate their numbers.

WHAT THIS EMITS INSTEAD
-----------------------
The stock curve, with every NEGATIVE sample scaled by depth/1.20 so the notch reaches the
requested depth. That keeps the mirror symmetry exact, keeps the header, changes nothing
on the positive lobe, and introduces no step: at the zero crossing the samples are ~0, so
scaling them leaves them ~0 and only the local slope changes.
"""

import argparse
import io
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gplpatch  # noqa: E402

# THE SPRING AND THE DAMPER GPL INSTALLS IN THE WHEEL ITSELF.
#
# gpl_ffbSetEnabled builds two DirectInput effects of the CONDITION class -- a centring
# spring and a damper -- once, at FFB init. These two `push` immediates are their
# coefficients (DirectInput scales them x10000).
#
# The distinction that matters: a CONDITION is computed by the wheel's own firmware from
# where the wheel is and how fast it is moving. It never passes through GPL's tick loop and
# never sees the car -- no tyre load, no kerb, no slide. The force the physics actually
# computes is a separate CONSTANT-force effect, and that is the one carrying the driving
# information. So the spring and damper are a fixed feel laid on top of the real signal.
#
# On a 1998 wheel that was worth having: the hardware could not self-centre convincingly
# and the damper hid the notchiness. On a modern belt or direct-drive wheel it fights the
# real force and is a common cause of a dead or rubbery feel -- and the wheel's own driver
# can supply both effects, tuned, if they are wanted.
#
# It is rate-independent, so this switch is equally valid on a stock 36 Hz build.
CONDITION_SITES = [
    (0x004A1116, 0.75, "gpl_ffbEffectSetTriple's condition coefficient"),
    (0x004A113F, 0.20, "gpl_ffbEffectSetSingle's condition coefficient"),
]

# The FFB SMOOTHING FILTER is deliberately NOT here. It is the one genuine rate fix on the
# force-feedback path, so it belongs to every build rather than to a switch, and it lives
# in gplrategen.py as FFB_IIR_BLOCK_VA. This file is preferences only.
CURVE_DOMAIN_VA = 0x0055BA84
CURVE_COUNT_VA = 0x0055BA88
CURVE_BODY_VA = 0x0055BA8C
CURVE_COUNT = 313
CURVE_DOMAIN = 5.0
CURVE_STOCK_NOTCH = -1.20


def read_f4(data, image, va):
    off = image.offset(va)
    if off is None:
        raise SystemExit("%08X is not inside any section's raw data" % va)
    return struct.unpack_from("<f", data, off)[0]


def comment_safe(text):
    """A double hyphen cannot appear inside an XML comment, and a parser rejects the whole
    file over one. The prose here is written for humans and uses them freely, so fold them
    down rather than expecting every future note to remember."""
    while "--" in text:
        text = text.replace("--", "-")
    return text


def emit(sites, title, note):
    out = io.StringIO()
    out.write('<?xml version="1.0"?>\n<GPLPatch>\n\n')
    out.write("<!-- %s\n\n%s\n-->\n\n" % (comment_safe(title), comment_safe(note)))
    for va, blob, comment in sites:
        out.write("   <BinaryPatch>\n")
        out.write("      <!-- %s -->\n" % comment_safe(comment))
        out.write("      <VirtualAddress>0x%08X</VirtualAddress>\n" % va)
        out.write("      <Data>%s</Data>\n" % blob.hex().upper())
        out.write("   </BinaryPatch>\n\n")
    out.write("</GPLPatch>\n")
    return out.getvalue()


def build_conditions(data, image):
    sites = []
    for va, stock, what in CONDITION_SITES:
        have = read_f4(data, image, va)
        if abs(have - stock) > abs(stock) * 1e-6:
            raise SystemExit("REFUSED: %08X should hold %s but holds %s -- this is not "
                             "the code this patch was built for." % (va, stock, have))
        sites.append((va, struct.pack("<f", 0.0),
                      "%s, %s -> 0.0" % (what, stock)))
    return emit(sites,
                "FFB: disable GPL's two canned DirectInput condition effects.",
                "GPL installs a centring spring and a damper as DirectInput CONDITION\n"
                "effects at FFB init. Conditions are computed in the wheel's firmware from\n"
                "wheel position and velocity, so they never see the car and never pass\n"
                "through the tick loop -- they are rate-independent, and this patch is\n"
                "equally valid at 36 Hz. Zeroing their coefficients leaves only the\n"
                "constant force GPL computes from the physics. On a direct-drive or belt\n"
                "wheel that usually removes a dead, rubbery overlay; the wheel's own\n"
                "software can supply the same effects, tuned, if they are wanted.\n"
                "NOT A RATE FIX. Reversible: remove this XML and rebuild.")


def build_curve(data, image, depth):
    if read_f4(data, image, CURVE_DOMAIN_VA) != CURVE_DOMAIN:
        raise SystemExit("REFUSED: %08X is not the curve's 5.0 domain max."
                         % CURVE_DOMAIN_VA)
    count = struct.unpack_from("<i", data, image.offset(CURVE_COUNT_VA))[0]
    if count != CURVE_COUNT:
        raise SystemExit("REFUSED: %08X should hold the count %d but holds %d."
                         % (CURVE_COUNT_VA, CURVE_COUNT, count))

    y = [read_f4(data, image, CURVE_BODY_VA + 4 * i) for i in range(count)]
    notch = min(y)
    if abs(notch - CURVE_STOCK_NOTCH) > 1e-5:
        raise SystemExit("REFUSED: the curve's notch is %s, not the expected %s."
                         % (notch, CURVE_STOCK_NOTCH))

    # Symmetry is the structure worth preserving, so assert it rather than assume it.
    axis = y.index(notch)
    for k in range(1, min(axis, count - 1 - axis) + 1):
        if abs(y[axis - k] - y[axis + k]) > 1e-6:
            raise SystemExit("REFUSED: the curve is not symmetric about index %d at "
                             "offset %d -- the reconstruction assumes it is." % (axis, k))

    factor = abs(depth) / abs(notch)
    sites = []
    for i, value in enumerate(y):
        if value >= 0.0:
            continue
        scaled = value * factor
        sites.append((CURVE_BODY_VA + 4 * i, struct.pack("<f", scaled),
                      "y[%d] %+.5f -> %+.5f" % (i, value, scaled)))
    return emit(
        sites,
        "FFB: deepen the force curve's notch from %.2f to %.2f."
        % (CURVE_STOCK_NOTCH, -abs(depth)),
        "This is what GEM's ffb3test1.xml was trying to do. That file writes 315 dwords\n"
        "starting at the HEADER's address: 157 of them are the stock table misaligned by\n"
        "four entries, 155 are byte-identical no-ops, and only three are real edits. The\n"
        "misalignment overwrites the 5.0 domain max and the integer count 313, and moves\n"
        "the curve's ascending limb while leaving the descending limb in place, breaking a\n"
        "symmetry the whole table is built on.\n\n"
        "This emits the stock curve with every NEGATIVE sample scaled by %.6f so the notch\n"
        "reaches %.2f. Symmetry is preserved exactly, the header is untouched, the positive\n"
        "lobe is unchanged, and there is no step at the zero crossing because the samples\n"
        "there are ~0. %d of %d entries are written.\n"
        "NOT A RATE FIX."
        % (abs(depth) / abs(CURVE_STOCK_NOTCH), -abs(depth), len(sites), count))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    gplpatch.add_game_dir_arg(ap)
    ap.add_argument("--exe",
                    help="unpatched reference exe to read stock values from. "
                         "Default: <game-dir>/gplc67_36.exe")
    ap.add_argument("--out-spring-damper", help="write the condition-zeroing XML here")
    ap.add_argument("--out-curve", help="write the deeper-notch XML here")
    ap.add_argument("--notch-depth", type=float, default=1.33,
                    help="notch depth for --out-curve (stock 1.20, ffb3test1 wanted 1.33)")
    args = ap.parse_args()

    if not (args.out_spring_damper or args.out_curve):
        raise SystemExit("nothing to do: pass --out-spring-damper and/or --out-curve")

    args.exe = gplpatch.game_file(args, "donor", "--exe",
                                  "the unpatched 36 Hz reference exe", "gplc67_36.exe")
    data = io.open(args.exe, "rb").read()
    image = gplpatch.Image(data)

    if args.out_spring_damper:
        io.open(args.out_spring_damper, "w", encoding="utf-8").write(
            build_conditions(data, image))
        sys.stderr.write("wrote %s\n" % args.out_spring_damper)
    if args.out_curve:
        io.open(args.out_curve, "w", encoding="utf-8").write(
            build_curve(data, image, args.notch_depth))
        sys.stderr.write("wrote %s\n" % args.out_curve)


if __name__ == "__main__":
    sys.exit(main())
