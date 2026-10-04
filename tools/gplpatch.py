#!/usr/bin/env python3
"""Apply GEM+ style XML byte patches to a GPL executable, without GEM+.

WHY THIS EXISTS

GEM+ works, when it works. It is also a VB6 application that rewrites parts of the
install you did not ask it to touch -- core.ini, driver settings, backups -- with no
record of what it changed. This does one job: it writes the bytes a patch XML names into
the executable, and nothing else. No config, no registry, no other file.

WHAT IT GUARANTEES

  * Every site is READ AND CHECKED before anything is written. If one site is not what
    the journal or the patch expects, NOTHING is written at all. There is no half-patched
    outcome.
  * The original bytes of every site are recorded in a journal next to the exe, so
    --revert restores exactly what was there, site by site, rather than trusting a backup
    to still be correct.
  * A whole-file backup is made once, named by the hash of the ORIGINAL file, and is
    never overwritten. Running --apply twice cannot destroy your pristine copy.
  * Applying two patch sets that disagree about the same address is refused, with both
    values shown.
  * --dry-run and --status change nothing, ever.

GEM+'s patches are pure in-place byte writes at fixed virtual addresses -- no insertions,
no relocation, no section changes -- so a patched exe keeps every address a disassembly
of the original produced. That is what makes this approach safe and it is why an IDB of
the unpatched file stays valid.

USAGE

  python gplpatch.py --exe PATH --patch FILE.xml [FILE2.xml ...] --dry-run
  python gplpatch.py --exe PATH --patch FILE.xml [FILE2.xml ...] --apply
  python gplpatch.py --exe PATH --status
  python gplpatch.py --exe PATH --revert [NAME ...]

NAME is the patch file's stem, as shown by --status. --revert with no NAME reverts
everything, most recent first.
"""

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time

# WHERE THE GAME IS. No tool here hardcodes an install path and none reads an environment
# variable: every one takes --game-dir, defaulting to the CURRENT DIRECTORY, and derives
# the files it needs from that. Run a tool from inside the GPL folder and it just works;
# run it anywhere else and pass --game-dir, or name the individual file.
#
# Anything a tool cannot find is a hard error naming both the file and the flag that
# supplies it, because the alternative -- carrying on with a default that happens to exist
# -- is how a patch gets built against the wrong reference.


def add_game_dir_arg(ap):
    ap.add_argument("--game-dir", default=os.getcwd(),
                    help="the Grand Prix Legends install directory. Default: the current "
                         "directory, so running from inside it needs no argument.")


def game_file(args, name, flag, what, *parts):
    """Resolve one game file: an explicit flag if given, else <game-dir>/<parts>.

    Refuses rather than returning a path that is not there. `flag` is the option a caller
    would use to override it, and appears in the error so the fix is obvious.
    """
    explicit = getattr(args, flag.lstrip("-").replace("-", "_"), None)
    path = explicit if explicit else os.path.join(args.game_dir, *parts)
    if not os.path.isfile(path):
        raise SystemExit(
            "cannot find %s:\n    %s\n"
            "Pass %s to name it directly, or --game-dir to point at the GPL install.\n"
            "%s" % (what, path, flag, _WHERE_FROM.get(name, "")))
    return path


# What each file IS, for anyone who does not have one. These are not files this project
# ships: they are the unmodified inputs a correct patch has to be derived FROM.
_WHERE_FROM = {
    "donor": "    This is a copy of a WORKING, UNPATCHED gplc67.exe -- the 1967 mod built\n"
             "    by GEM+ and not touched by any frame-rate patch. Every stock value is\n"
             "    checked against it before being replaced, and the 31-byte field at\n"
             "    0054CC24 that GEM+ writes is carried across from it -- gplexegen.ps1's\n"
             "    CarryBytes documents what that field is, and is not. Make one by\n"
             "    copying gplc67.exe before patching.\n",
    "ini36": "    This is a copy of the UNPATCHED gpla67.ini, the 36 Hz AI settings. The\n"
             "    rescaler reads it and never writes it. Make one by copying gpla67.ini\n"
             "    before patching.\n",
    "ref60": "    This is 60FPSv2.xml from the community 60 fps patch. It is the ORACLE\n"
             "    this project reads its exponents off: each of its sites is matched\n"
             "    against stock*(60/36)^k to recover k, which is then re-applied at the\n"
             "    target rate. Without it there is nothing to derive family A from.\n",
}

JOURNAL_SUFFIX = ".gplpatch.json"


# --------------------------------------------------------------------------- PE mapping

# THE +8 FIXUP. A GEM+ XML address is NOT always the address that gets written.
#
# GPL ships in two revisions of the base exe, and everything above a point in the tail of
# .data moved by 8 bytes between them. GPLPatchDLL.dll absorbs that difference rather than
# shipping two sets of XMLs: it probes one dword, and if the exe is the shifted revision it
# adds 8 to EVERY address coming out of the XML -- <BinaryPatch> targets and both ends of a
# <Relocation> alike -- when the address falls in the affected window.
#
# This tool did not do that, and the consequence was not subtle. The 60 fps patches carry
# three sites in the window; without the fixup they landed 8 bytes low, which in gplc67.exe
# means they overwrote the float constants pi and 2*pi and left two of their three intended
# targets at stock. It looked like the XML on disk was the wrong revision. It was not.
#
# The probe dword is at .text+0xD9C0, and only two values are legal. A third value means
# this is an exe neither GPLPatchDLL nor this tool has ever seen, and guessing which window
# applies to it would be exactly the mistake above -- so refuse rather than assume.
VARIANT_PROBE_VA = 0x0040E9C0
VARIANT_SHIFTED = 0x0054C5C0  # this revision needs +8
VARIANT_PLAIN = 0x0054C5B8    # this one does not
FIXUP_LOW = 0x0054C5B8
FIXUP_HIGH = 0x00971FFF


class Image:
    """Just enough PE to turn a virtual address into a file offset."""

    def __init__(self, data):
        if data[:2] != b"MZ":
            raise ValueError("not a PE file (no MZ signature)")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file (no PE signature)")
        n_sections = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != 0x10B:
            raise ValueError("expected a 32-bit PE (PE32), got magic 0x%04X" % magic)
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        self.sections = []
        table = opt + opt_size
        for i in range(n_sections):
            base = table + i * 40
            name = data[base:base + 8].rstrip(b"\0").decode("latin1")
            vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, base + 8)
            self.sections.append((name, vaddr, vsize, raw, rsize))

        # Which revision this is, and therefore whether addresses need rebasing. Done
        # here so no caller can forget to ask.
        probe_off = self.offset(VARIANT_PROBE_VA)
        if probe_off is None or probe_off + 4 > len(data):
            raise ValueError(
                "cannot read the exe-revision probe at %08X -- this does not look like a "
                "GPL executable, and applying GEM+ addresses to it would be a guess"
                % VARIANT_PROBE_VA)
        probe = struct.unpack_from("<I", data, probe_off)[0]
        if probe == VARIANT_SHIFTED:
            self.va_shift = 8
        elif probe == VARIANT_PLAIN:
            self.va_shift = 0
        else:
            raise ValueError(
                "the exe-revision probe at %08X reads %08X, which is neither %08X nor "
                "%08X. GPLPatchDLL.dll asserts on exactly this and so does this tool: an "
                "unknown revision means the XML addresses cannot be trusted."
                % (VARIANT_PROBE_VA, probe, VARIANT_SHIFTED, VARIANT_PLAIN))

    def fixup(self, va):
        """The address GEM+ would actually write, given this exe's revision."""
        if self.va_shift and FIXUP_LOW <= va <= FIXUP_HIGH:
            return va + self.va_shift
        return va

    @property
    def variant(self):
        """Which revision this exe is, in words, for anything that reports its donor."""
        return "shifted (+8 fixup)" if self.va_shift else "plain (no fixup)"

    def xml_address(self, va):
        """The address an XML must CARRY so that GEM+ writes `va` on this exe.

        The inverse of fixup(), and the thing a tool that located a site by SCANNING has to
        apply before emitting. Scanning yields the real address; writing that straight into
        <VirtualAddress> is correct on a plain exe and two floats late on a shifted one.

        The XML address space is therefore the PLAIN revision's, which is what makes a
        data-only XML revision-agnostic: GPLPatchDLL translates it for whichever exe it is
        applied to. Payload BYTES are not translated -- an address baked into an instruction
        inside <Data> is specific to the revision it was read from.

        Refuses rather than return an address that does not round-trip. That is the 8 bytes
        at the bottom of the window: reachable on a plain exe, unreachable on a shifted one.
        """
        if self.fixup(va) == va:
            return va
        candidate = va - self.va_shift
        if self.fixup(candidate) != va:
            raise ValueError(
                "%08X cannot be expressed as an XML address on this exe: fixup(%08X) is "
                "%08X, not %08X. It is within %d bytes of the fixup window's edge."
                % (va, candidate, self.fixup(candidate), va, self.va_shift))
        return candidate

    def offset(self, va):
        """File offset for a virtual address, or None if it is not in raw data."""
        rva = va - self.image_base
        for name, vaddr, vsize, raw, rsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                delta = rva - vaddr
                # A VA inside the virtual size but past the raw size is uninitialised
                # data with no bytes in the file. Writing it is not possible.
                if delta >= rsize:
                    return None
                return raw + delta
        return None


# --------------------------------------------------------------------------- patch file

def parse_patch(path):
    """[(virtual_address, bytes)] from a GEM+ XML patch, in file order."""
    with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
        text = handle.read()

    pattern = re.compile(
        r"<VirtualAddress>\s*(0[xX])?([0-9A-Fa-f]+)\s*</VirtualAddress>\s*"
        r"<Data>\s*([0-9A-Fa-f]+)\s*</Data>",
        re.S,
    )
    out = []
    for _prefix, addr, data in pattern.findall(text):
        if len(data) % 2:
            raise ValueError("%s: <Data> '%s' has an odd number of hex digits"
                             % (path, data))
        out.append((int(addr, 16), bytes.fromhex(data)))
    if not out:
        raise ValueError("%s: no <VirtualAddress>/<Data> pairs found" % path)

    # This tool does byte patches and NOTHING else. GEM+ patch files can in principle
    # carry other directives -- copying files, editing core.ini -- and silently ignoring
    # one would mean handing back a half-applied mod while reporting success. So refuse
    # anything whose vocabulary we do not fully understand.
    known = {"gplpatch", "binarypatch", "virtualaddress", "data", "xml"}
    unknown = sorted({t.lower() for t in re.findall(r"</?([A-Za-z_][\w.-]*)", text)}
                     - known)
    if unknown:
        raise ValueError(
            "%s contains directives this tool does not implement: %s. It only applies "
            "<BinaryPatch> byte writes, so applying this file here would silently do "
            "less than the patch intends." % (path, ", ".join(unknown)))
    return out


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def writable(path):
    """Can we actually write this file? Checked in phase 1, not discovered at write time.

    A GPL install copied off a CD, or restored from an archive, very often has the
    read-only attribute set on the executable -- the reference copy in this project does.
    Finding that out halfway through writing would be the one thing this tool promises
    cannot happen.
    """
    if not os.access(path, os.W_OK):
        return False
    try:
        with open(path, "r+b"):
            return True
    except OSError:
        return False


# --------------------------------------------------------------------------- journal

def journal_path(exe):
    return exe + JOURNAL_SUFFIX


def load_journal(exe):
    path = journal_path(exe)
    if not os.path.exists(path):
        return {"applied": []}
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def save_journal(exe, journal):
    with open(journal_path(exe), "w", encoding="utf-8") as handle:
        json.dump(journal, handle, indent=2)
        handle.write("\n")


def applied_sites(journal):
    """address -> (patch name, patched hex) for everything currently applied."""
    out = {}
    for entry in journal["applied"]:
        for site in entry["sites"]:
            out[int(site["va"], 16)] = (entry["name"], site["new"])
    return out


# --------------------------------------------------------------------------- operations

def do_status(exe, data):
    journal = load_journal(exe)
    print("exe    : %s" % exe)
    print("size   : %d bytes" % len(data))
    print("sha256 : %s" % sha256(data))
    print("variant: %s" % ("shifted (XML addresses rebased +8 in %08X..%08X)"
                           % (FIXUP_LOW, FIXUP_HIGH)
                           if Image(data).va_shift else "plain (no rebase)"))
    if not journal["applied"]:
        print("\nNo patches recorded as applied.")
        return 0

    print("\nApplied, oldest first:")
    image = Image(data)
    for entry in journal["applied"]:
        intact = 0
        drifted = []
        for site in entry["sites"]:
            va = int(site["va"], 16)
            off = image.offset(va)
            new = bytes.fromhex(site["new"])
            if off is not None and data[off:off + len(new)] == new:
                intact += 1
            else:
                drifted.append(site["va"])
        state = "all %d sites intact" % intact
        if drifted:
            state = "%d of %d intact -- DRIFTED: %s" % (
                intact, len(entry["sites"]),
                ", ".join(drifted[:6]) + (" ..." if len(drifted) > 6 else ""))
        print("  %-24s %s   applied %s" % (entry["name"], state, entry["when"]))
    print("\nRevert with:  --revert %s" %
          " ".join(e["name"] for e in reversed(journal["applied"])))
    return 0


def do_apply(exe, data, patch_files, dry_run):
    image = Image(data)
    journal = load_journal(exe)
    already = applied_sites(journal)
    known_names = {e["name"] for e in journal["applied"]}

    plans = []
    # --- Phase 1: read and check EVERYTHING. Nothing is written in this phase. -------
    problems = []
    seen = {}  # va -> (name, new bytes) within this run

    if not dry_run and not writable(exe):
        problems.append(
            "%s is not writable. If it is marked read-only, clear that with "
            "attrib -R on it, and check the game is not running." % exe)

    for path in patch_files:
        name = os.path.splitext(os.path.basename(path))[0]
        if name in known_names:
            problems.append("%s is already recorded as applied. Revert it first." % name)
            continue

        sites = []
        no_change = 0
        shifted = 0
        for raw_va, new in parse_patch(path):
            # The XML address is what the patch author wrote; `va` is where it goes.
            va = image.fixup(raw_va)
            if va != raw_va:
                shifted += 1
            off = image.offset(va)
            if off is None:
                problems.append("%s: %08X is not inside any section's raw data" %
                                (name, va))
                continue
            if off + len(new) > len(data):
                problems.append("%s: %08X runs past the end of the file" % (name, va))
                continue

            old = data[off:off + len(new)]

            if va in already:
                other_name, other_new = already[va]
                if bytes.fromhex(other_new) != new:
                    problems.append(
                        "%s: %08X is already patched by %s to %s, and this patch wants "
                        "%s. Refusing to stack conflicting patches."
                        % (name, va, other_name, other_new, new.hex().upper()))
                    continue

            if va in seen:
                other_name, other_new = seen[va]
                if other_new != new:
                    problems.append(
                        "%08X: %s wants %s but %s wants %s in the same run"
                        % (va, other_name, other_new.hex().upper(), name,
                           new.hex().upper()))
                    continue
            seen[va] = (name, new)

            if old == new:
                no_change += 1
            site = {"va": "%08X" % va, "off": off,
                    "old": old.hex().upper(), "new": new.hex().upper()}
            if va != raw_va:
                site["xml_va"] = "%08X" % raw_va
            sites.append(site)

        plans.append({"name": name, "source": os.path.abspath(path),
                      "sites": sites, "no_change": no_change, "shifted": shifted})

    if problems:
        print("REFUSING TO PATCH. Nothing has been written.\n")
        for problem in problems:
            print("  %s" % problem)
        return 2

    total = sum(len(p["sites"]) for p in plans)
    print("exe    : %s" % exe)
    print("sha256 : %s  (before)" % sha256(data))
    print("variant: %s" % (
        "shifted -- XML addresses in %08X..%08X are written 8 bytes higher, the same "
        "rebase GPLPatchDLL.dll applies" % (FIXUP_LOW, FIXUP_HIGH)
        if image.va_shift else
        "plain -- XML addresses are used as written"))
    print()
    for plan in plans:
        print("%s: %d sites%s%s" % (
            plan["name"], len(plan["sites"]),
            ", %d rebased +8" % plan["shifted"] if plan["shifted"] else "",
            ", %d already hold the patched value" % plan["no_change"]
            if plan["no_change"] else ""))
        for site in plan["sites"]:
            mark = "  (no change)" if site["old"] == site["new"] else ""
            print("    %s  %-16s -> %-16s%s" %
                  (site["va"], site["old"], site["new"], mark))
        print()

    if dry_run:
        print("DRY RUN -- %d sites would be written. Nothing was changed." % total)
        if not writable(exe):
            print("\nNOTE: %s is not writable as things stand. Clear the read-only "
                  "attribute with attrib -R before running --apply." % exe)
        return 0

    # --- Phase 2: back up once, then write. -----------------------------------------
    original_hash = sha256(data)
    backup = "%s.orig-%s" % (exe, original_hash[:8])
    if os.path.exists(backup):
        print("backup : %s already exists, keeping it" % backup)
    else:
        with open(backup, "wb") as handle:
            handle.write(data)
        print("backup : %s" % backup)

    patched = bytearray(data)
    for plan in plans:
        for site in plan["sites"]:
            new = bytes.fromhex(site["new"])
            patched[site["off"]:site["off"] + len(new)] = new

    with open(exe, "wb") as handle:
        handle.write(patched)

    when = time.strftime("%Y-%m-%d %H:%M:%S")
    for plan in plans:
        journal["applied"].append({
            "name": plan["name"],
            "source": plan["source"],
            "when": when,
            "exe_sha256_before": original_hash,
            "sites": [{"va": s["va"], "old": s["old"], "new": s["new"]}
                      for s in plan["sites"]],
        })
    journal["exe_sha256_after"] = sha256(bytes(patched))
    save_journal(exe, journal)

    print("sha256 : %s  (after)" % journal["exe_sha256_after"])
    print("\nWrote %d sites. Journal: %s" % (total, journal_path(exe)))
    print("Revert with:  python %s --exe \"%s\" --revert" %
          (os.path.basename(__file__), exe))
    return 0


def do_revert(exe, data, names, dry_run):
    image = Image(data)
    journal = load_journal(exe)
    if not journal["applied"]:
        print("Nothing is recorded as applied.")
        return 0

    # Most recent first, so stacked patches unwind in the order they went on.
    order = list(reversed(journal["applied"]))
    if names:
        wanted = set(names)
        unknown = wanted - {e["name"] for e in order}
        if unknown:
            print("Not applied: %s" % ", ".join(sorted(unknown)))
            return 2
        order = [e for e in order if e["name"] in wanted]

    problems = []
    writes = []
    for entry in order:
        for site in entry["sites"]:
            va = int(site["va"], 16)
            off = image.offset(va)
            new = bytes.fromhex(site["new"])
            old = bytes.fromhex(site["old"])
            if off is None:
                problems.append("%s: %s is no longer mappable" % (entry["name"], site["va"]))
                continue
            current = data[off:off + len(new)]
            if current == old:
                continue  # already reverted, or the patch was a no-op here
            if current != new:
                problems.append(
                    "%s: %s holds %s, but this patch wrote %s. Something else changed "
                    "it, so restoring %s could destroy that change."
                    % (entry["name"], site["va"], current.hex().upper(),
                       new.hex().upper(), old.hex().upper()))
                continue
            writes.append((off, old, site["va"], current))

    if problems:
        print("REFUSING TO REVERT. Nothing has been written.\n")
        for problem in problems:
            print("  %s" % problem)
        print("\nThe backup next to the exe is still a byte-for-byte original if you "
              "want to start over.")
        return 2

    print("Reverting %s: %d sites" %
          (", ".join(e["name"] for e in order), len(writes)))
    for _off, old, va, current in writes:
        print("    %s  %-16s -> %-16s" % (va, current.hex().upper(), old.hex().upper()))

    if dry_run:
        print("\nDRY RUN -- nothing was changed.")
        return 0

    patched = bytearray(data)
    for off, old, _va, _current in writes:
        patched[off:off + len(old)] = old
    with open(exe, "wb") as handle:
        handle.write(patched)

    reverted = {e["name"] for e in order}
    journal["applied"] = [e for e in journal["applied"] if e["name"] not in reverted]
    journal["exe_sha256_after"] = sha256(bytes(patched))
    save_journal(exe, journal)

    print("\nsha256 : %s  (after)" % journal["exe_sha256_after"])
    return 0


# --------------------------------------------------------------------------- entry

def main():
    parser = argparse.ArgumentParser(
        description="Apply GEM+ XML byte patches to a GPL executable, and nothing else.")
    parser.add_argument("--exe", required=True, help="the executable to patch")
    parser.add_argument("--patch", nargs="+", default=[], help="one or more patch XMLs")
    parser.add_argument("--apply", action="store_true", help="write the patches")
    parser.add_argument("--revert", nargs="*", default=None,
                        metavar="NAME", help="undo patches (all, or the ones named)")
    parser.add_argument("--status", action="store_true", help="show what is applied")
    parser.add_argument("--dry-run", action="store_true",
                        help="show what would happen and write nothing")
    args = parser.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("No such file: %s" % exe)
        return 2
    with open(exe, "rb") as handle:
        data = handle.read()

    try:
        if args.status:
            return do_status(exe, data)
        if args.revert is not None:
            return do_revert(exe, data, args.revert, args.dry_run)
        if not args.patch:
            print("Nothing to do. Give --patch FILE.xml with --apply or --dry-run, "
                  "or use --status.")
            return 2
        if not args.apply and not args.dry_run:
            print("Refusing to write without --apply. Add --dry-run to preview.")
            return 2
        return do_apply(exe, data, args.patch, args.dry_run or not args.apply)
    except ValueError as error:
        print("Error: %s" % error)
        return 2
    except OSError as error:
        # Anything that gets past the phase-1 checks: the file turned read-only under us,
        # the game started, the disk filled. Say which, without a traceback.
        print("Error writing: %s" % error)
        print("The exe may be partly written. Restore the .orig- backup next to it, or "
              "run --status to see what the journal thinks is applied.")
        return 2


if __name__ == "__main__":
    sys.exit(main())
