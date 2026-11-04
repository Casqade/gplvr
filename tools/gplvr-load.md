# `gplvr-load.xml` -- loading gplvr.dll without the loader

Applies to **gplc67.exe / gpl.exe, 1.2.0.2** (either link). Makes the game load `gplvr.dll`
itself, so gplvr works however the player launches -- a GEM+ shortcut that drops straight into a
race, a desktop shortcut, GPLShell -- with no injector and no `CreateRemoteThread` for an
antivirus to object to.

The patch itself is `patches/gplvr-load.xml`.

**Order and pribluda do not matter** (checked 2026-11-04 with the real GPLPatchDLL): the exe
comes out byte-identical whether it is applied first, last, before or after
`enablepribluda.xml`, and with or without pribluda -- on a clean base and on a pribpatcher'd one
alike. No XML among the 77 on this machine writes either of its sites or the `LoadLibraryA`
operand its stub reads, so GEM+'s name order is safe for it.

**The XML carries no comments on purpose.** None of the 49 GEM XMLs on this machine contains
one, and GPLPatchDLL has rejected a generated file before with no diagnostic at all, so the
documentation lives here instead of in the patch.

## What it does

GPL already loads a mod DLL this way: `gpl_rasterBegin_loadsPribluda` (`0x004C4FF0`) ends with a
hardcoded `LoadLibraryA("pribluda")`, and `enablepribluda.xml` is three `<BinaryPatch>` sites
totalling 34 bytes. This is the same technique one step earlier in startup.

| site | bytes | what |
|---|---|---|
| `0x005202FD` | `E8 FEBC4600` | `call 0x0098C000` -- was `call gpl_winMain` |
| `0x0098C000` | 36 bytes | the stub and its string, in `modtext` |

The stub:

```
0098C000  60                    pushad
0098C001  8B 15 17 2B 4E 00     mov   edx, dword ptr ds:[0x004E2B17]
0098C007  68 14 C0 98 00        push  0x0098C014
0098C00C  FF 12                 call  dword ptr [edx]
0098C00E  61                    popad
0098C00F  E9 1C 2B B5 FF        jmp   gpl_winMain (0x004DEB30)
0098C014  "gplvr\gplvr.dll",0
```

**Why it is a `call` and not a `jmp`.** The stub is entered by the original call, so `0x00520302`
is already on the stack as the return address; the stub then *jumps* to `gpl_winMain`, which is
`__stdcall(HINSTANCE, HINSTANCE, LPSTR, int)` and rets to `0x00520302` popping its own 16 bytes
of arguments -- exactly as if it had been called directly. `pushad`/`popad` is balanced and
`LoadLibraryA` is `__stdcall`, so it consumes the pushed string pointer itself.

**`[0x004E2B17]` is not a pointer variable.** It is the operand bytes of the existing
`call ds:LoadLibraryA` inside GPL's own rasterizer-loading helper at `0x004E2B10`, so the stub
reads the IAT slot (`0x00536058`) out of the instruction stream instead of hardcoding a data
address. The UK and US links of 1.2.0.2 ship identical code and move globals, so reading a code
address is link-agnostic by construction. The trick is `enablepribluda.xml`'s, not ours.

**The result of `LoadLibraryA` is deliberately ignored**, so a missing or wrong-architecture
`gplvr.dll` leaves the game running vanilla rather than failing to start. The only symptom is
that no new run folder with a `gplvr.log` appears under `gplvr\logs`.

## Why this site and not pribluda's

`gpl_rasterBegin_loadsPribluda` is reached from `gpl_startupScreenInit_640x480` *after* the
rasterizer DLL has been loaded and initialised and after a `SetWindowPos`. gplvr redirects six
imports of the main module -- `ChangeDisplaySettingsA/W/ExA/ExW`, `ClipCursor`, `SetWindowPos`
(`Hooks.cpp`, `kApiReplacements`) -- and has to be in place before GPL calls any of them.

At `0x005202FD` the CRT is fully initialised and **no GPL code has run**, which is equivalent to
the injector for everything that matters. The only difference from injection is that statically
imported DLLs' `DllMain`s have already run, which nothing here depends on.

## Why `modtext` and not code padding

**There is no free padding in `.text`.** Every run of 36 or more `0x90` bytes in the section is
overwritten by a GEM option XML -- checked against all 49 XMLs on this machine, 4,412
`<BinaryPatch>` sites, and the only candidates are claimed by `05`, `09`, `20`, `50`, `60`,
the 77G and Slipstream sets and 60FPSv2. The 1,024-byte zero run at `0x00517D70` looks like
padding and is **live data** (one `Data_Read` from `0x0051958D`).

`modtext` is GEM's own code section at `0x00980000`, which GPLPatchDLL creates or extends when a
patch asks for it. Across those same 49 XMLs nothing in it is touched above `0x0098AA00`, so
`0x0098C000` is a clear 4 KB page above everything, and `MinimumFileSize 0x0000C040` covers the
36 bytes written there.

**GPLPatchDLL has no collision detection** -- two XMLs writing one address both succeed and the
later wins, with `GPLPatchLog.txt` left empty. So re-run the check if a new option set is added:
`tools/gplpatchtable.py`'s sibling scan in `docs/` has the method, or re-derive it from the XML
directory directly.

## Addresses and the +8 rebase

GPLPatchDLL adds 8 to every XML address in `[0x0054C5B8, 0x00971FFF]`. **Both
`<VirtualAddress>` values here are outside that band** -- `0x005202FD` below it and
`0x0098C000` above it -- so the XML address equals the real virtual address in both cases, and
the two operands inside the stub (`0x004E2B17`, `0x0098C014`) are outside it too.

## Applying it

Either way GPLPatchDLL.dll applies it, so `<ExeSection>` is handled:
- with GEM+, as an option like any other;
- with `tools/make144.ps1 -LoadGplvr`, which appends it last, through `tools/gplexegen.ps1`
  and the real GPLPatchDLL.dll.

Only `tools/gplpatch.py`, the standalone Python patcher, cannot: it implements `<BinaryPatch>`
only and refuses any XML using a directive it does not implement.

## Testing it

1. A new run folder appears under `gplvr\logs` with a `gplvr.log` and no `gplvr-loader.log`
   (no loader ran), its first line `gplvr 1.0 (...) attached to ...` naming the exe.
2. The log's `IAT: main module ChangeDisplaySettingsA -> hooked` style lines are present, which
   is what being early enough buys.
3. The game still starts with `gplvr.dll` renamed away (fail-open).
4. GPL's own `pribluda` load still happens if `enablepribluda.xml` is applied -- the two are
   independent sites and neither touches the other.

The one assumption to confirm rather than trust: `"gplvr\gplvr.dll"` is a relative path with a
backslash, so it resolves against the **working directory**, not the executable's folder. GPL
reads its own data relative to the working directory, so it has to be the game folder for the
game to run at all -- but verify it under a GEM+ launch rather than assume it.
