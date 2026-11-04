#!/usr/bin/env python3
r"""The AI ini rate hook: the exe scales gpla67.ini's values itself, as it parses them.

WHAT IT DOES

Every float parameter GPL reads from an ini is a 46-byte binding whose vtable is 0x536B18.
Slot +8 of that vtable, 0x405250, turns the ini text into the float:

    if (sscanf(text, "%f", dest) != 1) *dest = default;
    if (fromFile) last = *dest;

The parser calls it once per `key = value` line, for every float binding in the game, and
nothing else calls it: its only reference is the vtable dword at 0x536B20.

This patch points that dword at a copy of the setter in a modtext cave. The copy parses the
same way, then multiplies the value by (rate/36)^k when two things hold. First, the binding
belongs to one of the three AI tables gpla67.ini fills: 0x42 (descriptors), 0x44 (line
states) or 0x46 (car classes). Second, its key is in a table of rate-dependent names. The
table is keyed by the binding's KEY POINTER, the exe's own copy of the name, so a match is
one dword compare. A stock 36 Hz gpla67.ini, or a player's tweaked one, is then correct at
any rate, and no rescaled ini is shipped.

WHY NOTHING IS SCALED TWICE

  - Only text is scaled. Defaults, resets, and the seeds GPL copies from one table into
    another are floats, and never pass through the setter.
  - Every load resets its table to the defaults before parsing, so a reload re-parses and
    re-scales from the text.
  - Nothing writes these tables back to a file.

A gpla67.ini ALREADY rescaled by gplaiscale.py is scaled twice. Install the 36 Hz file.

WHAT IT LEAVES ALONE

  - track.ini (table 0x45). Its four rate-dependent values are converted where they are read
    (families B and D of the rate patch), so scaling them here as well would apply the
    factor twice.
  - Descriptor defaults. They never pass through the setter; family K patches the ones
    the ini does not supply.

THE TABLE COMES FROM gplaiscale.py, the oracle that rescales the ini file. Every (section,
key) of the 36 Hz ini is resolved by its rules and reduced to one exponent per key NAME. A
name-keyed table cannot express a name with two different exponents in two sections, so
that refuses the build. Family K's descriptor defaults are checked against the table by name.

    python tools/gplaihook.py --check            # the no-game test: hook vs gplaiscale
    python tools/gplaihook.py --check --disasm   # the same, and print the cave

See docs/audit-2026-09-18/108-ai-ini-scaling-in-the-exe.md.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gplaiscale as A  # noqa: E402
import gplpatch  # noqa: E402

STOCK_RATE = 36.0

# The float binding's set-from-text method and the one vtable dword that reaches it. Both
# lie below the +8 fixup window, so they are the same in the UK and US links.
SETTER_VA = 0x00405250
SETTER_STOCK = bytes.fromhex(
    "8b442404" "56" "8bf1" "57" "8b7e22" "57" "6874615400" "50" "e8599e1100" "83c40c"
    "83f801" "7405" "8b4e26" "890f" "8a442410" "84c0" "7405" "8b17" "89562a" "5f" "5e"
    "c20800")
SETTER_FORMAT_PUSH = 12      # offset of `push offset "%f"` in the setter
SETTER_SSCANF_CALL = 18      # offset of `call _sscanf`
VTABLE_SLOT_VA = 0x00536B20
VTABLE_SLOT_STOCK = struct.pack("<I", SETTER_VA)

# The binding's fields, read off gpl_paramDescriptorInit and the setter.
BINDING_TABLE_ID = 0x04
BINDING_KEY = 0x0C
BINDING_DEST = 0x22
BINDING_DEFAULT = 0x26
BINDING_LAST = 0x2A

AI_TABLE_IDS = (0x42, 0x44, 0x46)
TRACK_TABLE_ID = 0x45

# Free modtext between 05.xml's last write (ending 0x0098177D) and 09.xml's first
# (0x00981AF0). Zero in both donors, and no XML on disk writes into it.
CAVE_VA = 0x00981780
CAVE_END = 0x00981AF0

# AUTHORED (UK) addresses, moved onto the donor through gplrategen.AuthoredImage.
#
# The descriptor table: 203 records of 24 bytes -- tableId, section*, key*, comment*, value.
# Every 0x42 binding, and the 0x44 / 0x46 bindings seeded from a descriptor, take their key
# pointer from here.
DESCRIPTOR_TABLE_VA = 0x00554658
DESCRIPTOR_STRIDE = 24
DESCRIPTOR_COUNT = 203
DESCRIPTOR_VALUE = 0x10
# gpl_aiCorneringParamNames[10..18]: the key pointers of the nine `cornering_` line-state
# bindings, which have no descriptor of their own.
CORNERING_NAMES_VA = 0x00557D70
CORNERING_NAMES_COUNT = 9


def _f32(value):
    """The float32 a value rounds to, as a Python float."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def factor(rate, exponent):
    return _f32((rate / STOCK_RATE) ** exponent)


# -- the table: one exponent per key name --------------------------------------------------

def name_exponents(ai_ini, oracle_stock, oracle_reference):
    """name -> exponent for every rate-dependent key of the 36 Hz AI ini, by gplaiscale's rules.

    Refuses when a name resolves to two different exponents in two sections, or when
    gplaiscale leaves a non-zero value unscaled under a name the hook would scale.
    """
    exps, _, _, _ = A.derive_exponents(oracle_stock, oracle_reference,
                                       A.DEFAULT_REFERENCE_RATE)
    values, _, _ = A.read_ini(ai_ini)
    votes, unknown = {}, {}
    for key in sorted(values):
        exponent, source, _ = A.resolve(key, exps, True)
        if source == "unknown":
            unknown.setdefault(key[1], []).append((key[0], values[key]))
            continue
        votes.setdefault(key[1], {}).setdefault(exponent, []).append(
            "[%s] (%s)" % (key[0], source))

    problems = []
    for name, by_exponent in sorted(votes.items()):
        if len(by_exponent) > 1:
            problems.append("%s: %s" % (name, "; ".join(
                "rate^%+d in %s" % (e, ", ".join(where))
                for e, where in sorted(by_exponent.items()))))
    table = {name: next(iter(by_exponent)) for name, by_exponent in votes.items()
             if len(by_exponent) == 1 and next(iter(by_exponent)) != 0}
    for name, entries in sorted(unknown.items()):
        live = [(sec, v) for sec, v in entries if v != 0.0]
        if name in table and live:
            problems.append("%s: rate^%+d by name, but gplaiscale leaves %s unscaled"
                            % (name, table[name], ", ".join("[%s] = %g" % e for e in live)))
    if problems:
        raise SystemExit(
            "REFUSED: the AI hook scales by key NAME, and these names are not one exponent "
            "everywhere in %s:\n  %s" % (os.path.basename(ai_ini), "\n  ".join(problems)))
    return table


# -- the donor: which key pointers the bindings use ----------------------------------------

def _cstring(image, data, va):
    off = image.offset_real(va)
    if off is None:
        raise SystemExit("REFUSED: key pointer %08X is not inside the donor's raw data" % va)
    end = data.index(b"\0", off)
    return data[off:end].decode("latin-1")


def descriptors(image, data):
    """[(authored value-slot VA, tableId, key pointer, name)] for every descriptor record."""
    out = []
    for i in range(DESCRIPTOR_COUNT):
        record = DESCRIPTOR_TABLE_VA + DESCRIPTOR_STRIDE * i
        off = image.offset(record)
        table_id, _, key_ptr = struct.unpack_from("<III", data, off)
        if table_id not in (0x42, TRACK_TABLE_ID):
            raise SystemExit("REFUSED: descriptor %d at %08X has table id %#x, not 0x42 or "
                             "0x45 -- not the table this hook was built for"
                             % (i, image.authored(record), table_id))
        out.append((record + DESCRIPTOR_VALUE, table_id, key_ptr,
                    _cstring(image, data, key_ptr)))
    return out


def binding_keys(image, data):
    """name -> the key pointers GPL's AI bindings use for it, read from the donor."""
    keys = {}
    for _, _, key_ptr, name in descriptors(image, data):
        keys.setdefault(name, set()).add(key_ptr)
    for i in range(CORNERING_NAMES_COUNT):
        ptr = struct.unpack_from("<I", data, image.offset(CORNERING_NAMES_VA + 4 * i))[0]
        name = _cstring(image, data, ptr)
        if not name.startswith("cornering_"):
            raise SystemExit("REFUSED: gpl_aiCorneringParamNames[%d] names %r, not a "
                             "cornering_ key" % (10 + i, name))
        keys.setdefault(name, set()).add(ptr)
    return keys


# -- the cave -------------------------------------------------------------------------------

def _stub(cave_va, sscanf_va, format_va, table_va):
    """The setter, plus the scale. thiscall: ecx = binding, [esp+4] = text, [esp+8] = flag."""
    code = bytearray()
    labels, jumps = {}, []

    def emit(hexes):
        code.extend(bytes.fromhex(hexes))

    def jump(opcode, label):
        code.extend((opcode, 0))
        jumps.append((len(code) - 1, label))

    def mark(label):
        labels[label] = len(code)

    emit("56 8bf1 57 8b7e22 57")                       # push esi/edi, edi = dest; push dest
    code.extend(b"\x68" + struct.pack("<I", format_va))  # push "%f"
    emit("ff742414")                                   # push text
    code.extend(b"\xe8" + struct.pack("<i", sscanf_va - (cave_va + len(code) + 5)))
    emit("83c40c 83f801")                              # cdecl cleanup; parsed one value?
    jump(0x75, "fail")
    emit("8b46%02x 83f8%02x" % (BINDING_TABLE_ID, AI_TABLE_IDS[0]))
    jump(0x74, "look")
    emit("83f8%02x" % AI_TABLE_IDS[1])
    jump(0x74, "look")
    emit("83f8%02x" % AI_TABLE_IDS[2])
    jump(0x75, "done")
    mark("look")
    emit("8b46%02x" % BINDING_KEY)                     # eax = the binding's key pointer
    code.extend(b"\xb9" + struct.pack("<I", table_va))  # ecx = the table
    mark("next")
    emit("8b11 85d2")                                  # edx = entry key; 0 ends the table
    jump(0x74, "done")
    emit("39c2")
    jump(0x74, "hit")
    emit("83c108")
    jump(0xEB, "next")
    mark("hit")
    emit("d907 d84904 d91f")                           # *dest *= entry factor
    jump(0xEB, "done")
    mark("fail")
    emit("8b4e%02x 890f" % BINDING_DEFAULT)            # unparsable: the default, unscaled
    mark("done")
    emit("807c241000")                                 # from a file?
    jump(0x74, "out")
    emit("8b17 8956%02x" % BINDING_LAST)
    mark("out")
    emit("5f 5e c20800")
    for pos, label in jumps:
        rel = labels[label] - (pos + 1)
        if not -128 <= rel <= 127:
            raise SystemExit("REFUSED: the hook stub's jump to %s does not fit rel8" % label)
        code[pos] = rel & 0xFF
    return bytes(code)


def build_cave(image, data, rate, table):
    """(cave bytes, [(key pointer, name, exponent, factor)], unresolved names).

    Reads the sscanf call and the "%f" operand from the donor's own setter, so the stub
    calls exactly what the stock one does on either link.
    """
    off = image.offset_real(SETTER_VA)
    if data[off:off + len(SETTER_STOCK)] != SETTER_STOCK:
        raise SystemExit("REFUSED: %08X is not the stock float setter this hook copies."
                         % SETTER_VA)
    off = image.offset_real(VTABLE_SLOT_VA)
    if data[off:off + 4] != VTABLE_SLOT_STOCK:
        raise SystemExit("REFUSED: the vtable slot %08X does not point at %08X."
                         % (VTABLE_SLOT_VA, SETTER_VA))
    setter = image.offset_real(SETTER_VA)
    format_va = struct.unpack_from("<I", data, setter + SETTER_FORMAT_PUSH + 1)[0]
    sscanf_va = (SETTER_VA + SETTER_SSCANF_CALL + 5
                 + struct.unpack_from("<i", data, setter + SETTER_SSCANF_CALL + 1)[0])

    cave = image.offset_real(CAVE_VA)
    if cave is None or any(data[cave:cave + (CAVE_END - CAVE_VA)]):
        raise SystemExit("REFUSED: the AI hook cave %08X..%08X is not zero raw data."
                         % (CAVE_VA, CAVE_END))

    keys = binding_keys(image, data)
    entries, unresolved = [], []
    for name, exponent in sorted(table.items()):
        if name not in keys:
            unresolved.append(name)
            continue
        for ptr in sorted(keys[name]):
            entries.append((ptr, name, exponent, factor(rate, exponent)))

    stub = _stub(CAVE_VA, sscanf_va, format_va, 0)
    table_va = (CAVE_VA + len(stub) + 3) & ~3
    stub = _stub(CAVE_VA, sscanf_va, format_va, table_va)
    blob = bytearray(stub + b"\0" * (table_va - CAVE_VA - len(stub)))
    for ptr, _, _, value in entries:
        blob += struct.pack("<If", ptr, value)
    blob += b"\0" * 4
    if CAVE_VA + len(blob) > CAVE_END:
        raise SystemExit("REFUSED: the AI hook needs %d bytes and the cave holds %d."
                         % (len(blob), CAVE_END - CAVE_VA))
    return bytes(blob), entries, unresolved


def with_family_k(image, data, table, defaults):
    """The table plus the names whose descriptor DEFAULT family K scales.

    `defaults` is gplrategen's AI_EXTRA_FLOAT_SITES; the descriptor defaults among them are
    family K. A name in both must carry one exponent, or a key present in the ini and the same
    key absent from it would run at two rates. A name only family K knows -- a default the
    stock ini never overrides -- joins the table, so a player who adds that key to the ini
    gets it scaled like the default it replaces.
    """
    by_slot = {slot: name for slot, _, _, name in descriptors(image, data)}
    full = dict(table)
    for va, _, _, exponent, _ in defaults:
        name = by_slot.get(va)
        if name is None:
            continue
        if name in table and table[name] != exponent:
            raise SystemExit(
                "REFUSED: family K patches %s's default at %08X as rate^%+d, but the AI "
                "hook scales its ini value as rate^%+d." % (name, va, exponent, table[name]))
        full[name] = exponent
    return full


def sites(image, data, rate, table, defaults):
    """The hook's patch sites as (va, stock, out, note), in authored space."""
    table = with_family_k(image, data, table, defaults)
    blob, entries, unresolved = build_cave(image, data, rate, table)
    names = sorted({name for _, name, _, _ in entries})
    out = [
        (CAVE_VA, bytes(len(blob)), blob,
         "AI ini hook: the float setter plus (rate/36)^k for %d key names (%d key pointers) "
         "of tables 0x42/0x44/0x46 -- %s" % (len(names), len(entries), ", ".join(
             "%s rate^%+d" % (n, table[n]) for n in names))),
        (VTABLE_SLOT_VA, VTABLE_SLOT_STOCK, struct.pack("<I", CAVE_VA),
         "AI ini hook: the float binding vtable's set-from-text slot -> the cave at %08X"
         % CAVE_VA),
    ]
    if unresolved:
        sys.stderr.write("AI hook   : %d rate-dependent ini name(s) have no AI binding in the "
                         "exe, so GPL never reads them: %s\n"
                         % (len(unresolved), ", ".join(unresolved)))
    return out


# -- the no-game test ---------------------------------------------------------------------

def compare(ai_ini, oracle_stock, oracle_reference, rate, table, track_keys):
    """Apply the table the way the stub does and diff against gplaiscale's rescaled file.

    The stub parses with sscanf("%f"), which the CRT does through a double, then multiplies
    in the FPU and stores a float -- one rounding of an exact product. Returns
    (checked, scaled, mismatches, track_only).
    """
    exps, _, _, _ = A.derive_exponents(oracle_stock, oracle_reference,
                                       A.DEFAULT_REFERENCE_RATE)
    text, _, _, _, _ = A.rescale(ai_ini, rate, exps, True, False)
    want = A.parse_values(text)
    have, _, _ = A.read_ini(ai_ini)
    checked, scaled, mismatches, track_only = 0, 0, [], []
    for key in sorted(have):
        expected = _f32(want[key])
        if key in track_keys:
            if expected != _f32(have[key]):
                track_only.append(key)
            continue
        parsed = _f32(have[key])
        exponent = table.get(key[1], 0)
        hooked = _f32(parsed * factor(rate, exponent)) if exponent else parsed
        checked += 1
        if hooked != parsed:
            scaled += 1
        if hooked != expected:
            mismatches.append((key, have[key], hooked, expected))
    return checked, scaled, mismatches, track_only


def _ulps(a, b):
    ia = struct.unpack("<i", struct.pack("<f", a))[0]
    ib = struct.unpack("<i", struct.pack("<f", b))[0]
    return abs(ia - ib)


def main():
    import gplrategen as G  # the donor image, in this file's authored address convention

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    gplpatch.add_game_dir_arg(ap)
    ap.add_argument("--rate", type=float, default=144.0)
    ap.add_argument("--exe", help="the unpatched donor. Default: <game-dir>/gplc67_36.exe")
    ap.add_argument("--ai-ini", help="the 36 Hz AI ini. Default: <game-dir>/gpla67.ini.36")
    ap.add_argument("--oracle-stock", help="Default: <game-dir>/gpl_ai.ini.98")
    ap.add_argument("--oracle-reference",
                    help="Default: <game-dir>/tools/60FPS/60fpsv2/gpl_ai_60fps.ini")
    ap.add_argument("--check", action="store_true",
                    help="build the table and the cave, and diff the hook against gplaiscale")
    ap.add_argument("--disasm", action="store_true", help="print the cave (needs capstone)")
    args = ap.parse_args()
    if not args.check:
        ap.error("nothing to do: pass --check")

    exe, ai_ini, oracle_stock, oracle_reference = inputs(args)
    data = open(exe, "rb").read()
    image = G.AuthoredImage(gplpatch.Image(data))
    table = name_exponents(ai_ini, oracle_stock, oracle_reference)
    full = with_family_k(image, data, table, G.AI_EXTRA_FLOAT_SITES)
    blob, entries, unresolved = build_cave(image, data, args.rate, full)
    out = sites(image, data, args.rate, table, G.AI_EXTRA_FLOAT_SITES)
    table_only_k = sorted(set(full) - set(table))

    print("donor      : %s (%s revision)" % (exe, image.variant))
    print("ai ini     : %s" % ai_ini)
    print("table      : %d names from the ini + %d only family K patches (%s), %d key "
          "pointers, %d unresolved%s"
          % (len(table), len(table_only_k), ", ".join(table_only_k), len(entries),
             len(unresolved), (" (%s)" % ", ".join(unresolved)) if unresolved else ""))
    for exponent in sorted({e for e in full.values()}, reverse=True):
        print("  rate^%+d x%-6g %s" % (exponent, factor(args.rate, exponent), " ".join(
            sorted(n for n, e in full.items() if e == exponent))))
    print("cave       : %08X..%08X, %d of %d bytes"
          % (CAVE_VA, CAVE_VA + len(blob) - 1, len(blob), CAVE_END - CAVE_VA))
    print("sites      : %d" % len(out))

    track_keys = set()
    for _, table_id, key_ptr, name in descriptors(image, data):
        if table_id == TRACK_TABLE_ID:
            track_keys.update(k for k in A.read_ini(ai_ini)[0] if k[1] == name)
    checked, scaled, mismatches, track_only = compare(
        ai_ini, oracle_stock, oracle_reference, args.rate, table, track_keys)
    print("check     : %d values, %d scaled by the hook; %d differ from gplaiscale"
          % (checked, scaled, len(mismatches)))
    for key, old, hooked, expected in mismatches:
        print("  DIFFERS  [%s] %-34s %-12g hook %-14.9g gplaiscale %-14.9g (%d ulp)"
              % (key[0], key[1], old, hooked, expected, _ulps(hooked, expected)))
    if track_only:
        print("  track.ini keys in this ini, rescaled by gplaiscale but never read from it: %s"
              % ", ".join("[%s] %s" % k for k in track_only))

    if args.disasm:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        stub_len = blob.index(struct.pack("<I", entries[0][0])) if entries else len(blob)
        for insn in md.disasm(blob[:stub_len], CAVE_VA):
            print("  %08X  %-16s %s %s" % (insn.address, insn.bytes.hex(), insn.mnemonic,
                                           insn.op_str))
        for ptr, name, exponent, value in entries:
            print("  table  %08X  %-34s rate^%+d  %g" % (ptr, name, exponent, value))

    # At 144 every factor is a power of two, so the hook must reproduce gplaiscale exactly.
    # At other rates a 1 ulp difference is the ini's decimal rounding, not a disagreement.
    if args.rate in [STOCK_RATE * 2 ** n for n in range(-4, 5)]:
        return 1 if mismatches else 0
    return 0 if all(_ulps(h, e) <= 1 for _, _, h, e in mismatches) else 1


def inputs(args):
    """(donor exe, 36 Hz AI ini, oracle stock, oracle reference), each checked to exist."""
    exe = gplpatch.game_file(args, "donor", "--exe",
                             "the unpatched 36 Hz reference exe", "gplc67_36.exe")
    ai_ini = gplpatch.game_file(args, "ini36", "--ai-ini",
                                "the unpatched 36 Hz AI ini", "gpla67.ini.36")
    oracle_stock = gplpatch.game_file(args, "oracle", "--oracle-stock",
                                      "the stock 36 Hz AI ini (the oracle's 36 Hz half)",
                                      "gpl_ai.ini.98")
    oracle_reference = gplpatch.game_file(args, "ref60", "--oracle-reference",
                                          "the hand-rescaled 60 Hz AI ini",
                                          "tools", "60FPS", "60fpsv2", "gpl_ai_60fps.ini")
    return exe, ai_ini, oracle_stock, oracle_reference


if __name__ == "__main__":
    sys.exit(main())
