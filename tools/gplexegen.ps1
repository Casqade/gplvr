<#
.SYNOPSIS
  Build a patched GPL executable by calling the real GPLPatchDLL.dll.

.DESCRIPTION
  Exe in, exe out. Nothing else -- except the /LARGEADDRESSAWARE flag, set after the save
  unless -NoLargeAddressAware.

  This is the exe-generation half of GEM+ and nothing more. It does not read or write
  core.ini, App.ini, player setups, carsets, pictures, the registry, or any game
  configuration; it does not rename or launch anything. It copies the source exe to the
  destination, hands the destination to GPLPatchDLL.dll, applies the XMLs you name in the
  order you name them, saves, and reports hashes.

  The patch semantics are GPLPatchDLL's own -- including the hidden +8 address rebase
  between the two shipped revisions of gpl.exe -- because the DLL does the work. Nothing
  about the XML format is reimplemented here.

  MUST run 32-bit. GPLPatchDLL.dll is a 32-bit DLL. If started from a 64-bit host this
  script re-executes itself under %SystemRoot%\SysWOW64\WindowsPowerShell\v1.0\powershell.exe.

.PARAMETER Source
  Input executable. Opened read-only, never modified.

.PARAMETER XmlDir
  Directory of .xml patches. All *.xml in it are applied in ordinal name order.

.PARAMETER Xml
  Explicit ordered list of .xml patches. Applied exactly in the order given.
  Use instead of -XmlDir when the order matters and is not the name order.

.PARAMETER Out
  Output executable. Must differ from -Source. Refuses to overwrite unless -Force.

.PARAMETER CarryBytes
  Optional, repeatable. "<donorExe>@<offset>:<length>" -- copies a raw byte range from a
  donor executable into the finished output, at the same FILE OFFSET, after the DLL has
  saved. Offsets and lengths accept 0x hex. Every range is reported before/after.

  This exists for per-installation data that is in no XML and cannot be derived from the
  source exe. The known case was called "the GPL CD key" here for a long time; that was a
  guess and it is wrong. What the range at file offset 1358372 actually holds is a 31-byte
  string buffer at VA 0054CC24 whose stock value is the literal "NOT APPLICABLE", and which
  GEM+ overwrites with 16 bytes (31 52 01 05 20 01 02 03 04 05 06 07 20 20 00 00) when it
  builds the exe. Applying all 22 gpl67 XMLs through GPLPatchDLL leaves the stock value, so
  neither an XML nor the DLL writes it: GEM+ does, after the DLL has saved. Exactly one
  instruction reads it -- 0041044F, a 31-byte copy into offset +0x28 of the ~0xA8 struct
  built at 00410410, which also takes a car code at +0x80 and is handed to GEM+ mod code at
  00985412. The same bytes appear at offset 320 of every saved game, immediately after the
  player name, so that struct is serialised into saves. What the field MEANS is unestablished;
  "NOT APPLICABLE" as the stock default suggests GPL itself has no use for it and GEM+
  repurposed it as a stamp. It is a plain byte copy at a file
  offset, deliberately not GPLPatchDLL's PatchWithData: that takes a virtual address and
  silently adds 8 to it inside 0x0054C5B8..0x00971FFF, which is exactly the class of hidden
  behaviour this project has been burned by. A file offset means what it says.

.PARAMETER Expect
  Optional sha256 the finished output must equal. Mismatch is a non-zero exit.

.PARAMETER Manifest
  Optional path for a JSON record of the whole build.

.PARAMETER DryRun
  Print the plan and exit without creating anything.

.PARAMETER NoLargeAddressAware
  Leave the COFF Characteristics as the DLL saved them. By default the finished exe is marked
  /LARGEADDRESSAWARE (bit 0x20), giving GPL 4 GiB of address space on 64-bit Windows instead
  of 2. No XML can set it, so before this every regenerated exe silently lost it (2026-10-11
  to 2026-10-25: Isle of Man stopped baking in GPLVR). Reported before/after, and recorded in
  the manifest as largeAddressAware.

.EXAMPLE
  .\gplexegen.ps1 -Source 'H:\[Games]\GrandPrixLegends\gpl.exe' -XmlDir 'H:\[Games]\GrandPrixLegends\Mods\GEM+\gpl67' -Out 'C:\work\gplc67.exe'
#>
[CmdletBinding()]
param(
    [string]   $Source,
    [string]   $XmlDir,
    [string[]] $Xml,
    [string]   $Out,
    [string[]] $CarryBytes,
    [string]   $Expect,
    [string]   $Manifest,
    [string]   $Dll = (Join-Path $env:SystemRoot 'SysWOW64\GPLPatchDLL.dll'),
    [switch]   $Force,
    [switch]   $DryRun,
    [switch]   $NoLargeAddressAware,
    # internal: set only by the 32-bit re-exec below, never by a caller
    [string]   $ArgsFile
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- 32-bit re-exec ----
# GPLPatchDLL.dll is 32-bit. A 64-bit process cannot load it, so hand the whole invocation
# to 32-bit PowerShell. Arguments go through a CLIXML file rather than a command line
# because powershell.exe -File does not bind array arguments reliably.
if ([IntPtr]::Size -ne 4) {
    $ps32 = Join-Path $env:SystemRoot 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $ps32)) {
        Write-Host "REFUSED: GPLPatchDLL.dll is 32-bit and this host is 64-bit, but no 32-bit PowerShell exists at $ps32" -ForegroundColor Red
        exit 3
    }
    Write-Host "[host] 64-bit PowerShell cannot load a 32-bit DLL. Re-executing under $ps32" -ForegroundColor DarkGray
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("gplexegen-args-" + [Guid]::NewGuid().ToString('N') + ".clixml")
    $bag = @{}
    foreach ($k in $PSBoundParameters.Keys) { if ($k -ne 'ArgsFile') { $bag[$k] = $PSBoundParameters[$k] } }
    $bag | Export-Clixml -LiteralPath $tmp
    try   { & $ps32 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -ArgsFile $tmp; $code = $LASTEXITCODE }
    finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    exit $code
}

if ($ArgsFile) {
    if (-not (Test-Path -LiteralPath $ArgsFile)) { Write-Host "REFUSED: -ArgsFile not found: $ArgsFile" -ForegroundColor Red; exit 3 }
    $bag = Import-Clixml -LiteralPath $ArgsFile
    foreach ($k in $bag.Keys) { Set-Variable -Name $k -Value $bag[$k] -Scope 0 }
}

function Die([string]$msg, [int]$code = 1) {
    Write-Host ""
    Write-Host "REFUSED: $msg" -ForegroundColor Red
    exit $code
}
function Sha256([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() }
# The DLL keeps GPLPatchLog.txt open for writing for the whole OpenExe..CloseExe session,
# so it has to be read with FileShare.ReadWrite.
function ReadLogText([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) { return '' }
    try {
        $s = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try { $r = New-Object IO.StreamReader($s); try { return $r.ReadToEnd() } finally { $r.Dispose() } }
        finally { $s.Dispose() }
    } catch { return '' }
}
function ParseNum([string]$s) {
    if ($s -match '^\s*0[xX]([0-9a-fA-F]+)\s*$') { return [Convert]::ToInt64($Matches[1], 16) }
    if ($s -match '^\s*[0-9]+\s*$')              { return [int64]$s }
    throw "not a number: '$s'"
}

# ---------------------------------------------------------------- validate: DLL ----
if (-not $Source) { Die "-Source is required" }
if (-not $Out)    { Die "-Out is required" }
if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) { Die "GPLPatchDLL not found at $Dll" }
$DllFull = (Resolve-Path -LiteralPath $Dll).ProviderPath

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GplPatch {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true, BestFitMapping=false)]
    public static extern IntPtr GetProcAddress(IntPtr h, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool FreeLibrary(IntPtr h);

    // GPLPatchDLL.dll exports are MSVC-decorated __stdcall: leading underscore, @bytes suffix.
    // Each returns int: 1 = success, 0 = failure. The reason (if any) is appended to
    // GPLPatchLog.txt in the process current directory.
    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet=CharSet.Ansi, BestFitMapping=false)]
    public delegate int PfnStr(string s);   // LPCSTR: the DLL does a byte-wise strlen on it
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate int PfnVoid();
}
'@

$hDll = [GplPatch]::LoadLibraryW($DllFull)
if ($hDll -eq [IntPtr]::Zero) {
    Die ("LoadLibrary failed on {0} (win32 error {1}). This host IS 32-bit, so bitness is not the cause." -f $DllFull, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
}

$exportNames = [ordered]@{
    OpenExe             = '_OpenExe@4'
    PatchWithXml        = '_PatchWithXml@4'
    SaveExe             = '_SaveExe@0'
    CloseExe            = '_CloseExe@0'
    CloseExeWithoutSave = '_CloseExeWithoutSave@0'
}
$fn = @{}
foreach ($k in $exportNames.Keys) {
    $name = $exportNames[$k]
    $addr = [GplPatch]::GetProcAddress($hDll, $name)
    if ($addr -eq [IntPtr]::Zero) { Die "export $name does not resolve in $DllFull" }
    $t = if ($name -like '*@4') { [GplPatch+PfnStr] } else { [GplPatch+PfnVoid] }
    $fn[$k] = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($addr, $t)
}

# ---------------------------------------------------------------- validate: paths ----
if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { Die "source not found: $Source" }
$SourceFull = (Resolve-Path -LiteralPath $Source).ProviderPath

$hdr = New-Object byte[] 0x400
$fs = [IO.File]::OpenRead($SourceFull)
try { $null = $fs.Read($hdr, 0, $hdr.Length) } finally { $fs.Dispose() }
if ($hdr[0] -ne 0x4D -or $hdr[1] -ne 0x5A) { Die "source is not a PE image (no MZ): $SourceFull" }
$e_lfanew = [BitConverter]::ToInt32($hdr, 0x3C)
if ($e_lfanew -lt 0 -or ($e_lfanew + 6) -gt $hdr.Length) { Die "source PE header offset out of range: $SourceFull" }
if ([BitConverter]::ToUInt32($hdr, $e_lfanew) -ne 0x00004550) { Die "source is not a PE image (no PE signature): $SourceFull" }
$machine = [BitConverter]::ToUInt16($hdr, $e_lfanew + 4)
if ($machine -ne 0x014C) { Die ("source is not a 32-bit x86 PE (machine 0x{0:X4}): {1}" -f $machine, $SourceFull) }

$OutFull = [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).ProviderPath, $Out))
if ([string]::Equals($OutFull, $SourceFull, [StringComparison]::OrdinalIgnoreCase)) {
    Die "-Out is the same file as -Source. This tool never writes over its input."
}
$outDir = Split-Path -Parent $OutFull
if (-not (Test-Path -LiteralPath $outDir -PathType Container)) { Die "output directory does not exist: $outDir" }
if ((Test-Path -LiteralPath $OutFull) -and -not $Force) { Die "output already exists: $OutFull  (pass -Force to replace it)" }

# ---------------------------------------------------------------- validate: XMLs ----
if ($XmlDir -and $Xml) { Die "give -XmlDir or -Xml, not both" }
$xmlList = @()
if ($XmlDir) {
    if (-not (Test-Path -LiteralPath $XmlDir -PathType Container)) { Die "xml directory not found: $XmlDir" }
    $xmlList = @(Get-ChildItem -LiteralPath $XmlDir -Filter *.xml -File |
                 Sort-Object -Property Name |
                 ForEach-Object { $_.FullName })
    if ($xmlList.Count -eq 0) { Die "no .xml files in $XmlDir" }
} elseif ($Xml) {
    foreach ($x in $Xml) {
        if (-not (Test-Path -LiteralPath $x -PathType Leaf)) { Die "xml not found: $x" }
        $xmlList += (Resolve-Path -LiteralPath $x).ProviderPath
    }
} else { Die "give -XmlDir or -Xml" }

# Well-formedness is checked up front, before anything is created, because the DLL only
# says "An error occurred interpreting xml file <name>" and by then a partial exe exists.
foreach ($x in $xmlList) {
    $doc = New-Object System.Xml.XmlDocument
    $doc.XmlResolver = $null
    try { $doc.Load($x) } catch { Die "cannot parse $x : $($_.Exception.Message)" }
    if ($doc.DocumentElement.Name -ne 'GPLPatch') {
        Die "root element of $x is <$($doc.DocumentElement.Name)>, expected <GPLPatch>"
    }
}

# ---------------------------------------------------------------- validate: carry ----
$carry = @()
foreach ($c in @($CarryBytes)) {
    if (-not $c) { continue }
    if ($c -notmatch '^(.*)@([^@:]+):([^@:]+)$') { Die "bad -CarryBytes '$c', expected <donorExe>@<offset>:<length>" }
    $dpath = $Matches[1]; $doff = $Matches[2]; $dlen = $Matches[3]
    if (-not (Test-Path -LiteralPath $dpath -PathType Leaf)) { Die "carry donor not found: $dpath" }
    try { $off = ParseNum $doff; $len = ParseNum $dlen } catch { Die "bad -CarryBytes '$c': $($_.Exception.Message)" }
    if ($len -le 0) { Die "bad -CarryBytes '$c': length must be positive" }
    $dfull = (Resolve-Path -LiteralPath $dpath).ProviderPath
    $dsize = (Get-Item -LiteralPath $dfull).Length
    if (($off + $len) -gt $dsize) { Die "-CarryBytes '$c' runs past the end of the donor ($dsize bytes)" }
    $carry += [pscustomobject]@{ Donor = $dfull; Offset = $off; Length = [int]$len; Sha256 = (Sha256 $dfull) }
}

# ---------------------------------------------------------------- the plan ----
$srcHash = Sha256 $SourceFull
$srcSize = (Get-Item -LiteralPath $SourceFull).Length

Write-Host ""
Write-Host "gplexegen -- GPLPatchDLL exe generation, and nothing else" -ForegroundColor Cyan
Write-Host "  dll      $DllFull"
Write-Host "  source   $SourceFull"
Write-Host "           sha256 $srcHash  size $srcSize"
Write-Host "  order    $($xmlList.Count) xml, applied in this order:"
$plan = @()
$i = 0
foreach ($x in $xmlList) {
    $i++
    $h = Sha256 $x
    $s = (Get-Item -LiteralPath $x).Length
    $plan += [pscustomobject]@{ Index = $i; Path = $x; Sha256 = $h; Size = $s }
    Write-Host ("           {0,2}. {1,-34} {2}  {3} bytes" -f $i, (Split-Path $x -Leaf), $h.Substring(0,16), $s)
}
foreach ($c in $carry) {
    Write-Host ("  carry    0x{0:x} + {1} bytes from {2}" -f $c.Offset, $c.Length, $c.Donor)
}
Write-Host "  dest     $OutFull"
if ($Expect) { Write-Host "  expect   $($Expect.ToLower())" }
Write-Host ""
Write-Host "  This run creates exactly one file: the destination above." -ForegroundColor DarkGray
Write-Host "  No game configuration is read or written. The source is opened read-only." -ForegroundColor DarkGray
Write-Host ""

if ($DryRun) { Write-Host "-DryRun: nothing was created." -ForegroundColor Yellow; exit 0 }

# ---------------------------------------------------------------- build ----
# GPLPatchDLL opens GPLPatchLog.txt in the PROCESS CURRENT DIRECTORY and truncates it at
# OpenExe. Point the CWD at a scratch directory so it can never land in a game folder.
$logDir = Join-Path ([IO.Path]::GetTempPath()) ("gplexegen-" + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $logDir
$logPath = Join-Path $logDir 'GPLPatchLog.txt'
$prevCwd = [IO.Directory]::GetCurrentDirectory()

$building = "$OutFull.building"
if (Test-Path -LiteralPath $building) { Remove-Item -LiteralPath $building -Force }
Copy-Item -LiteralPath $SourceFull -Destination $building
Set-ItemProperty -LiteralPath $building -Name IsReadOnly -Value $false

$records = @()
$failed  = $null
try {
    [IO.Directory]::SetCurrentDirectory($logDir)

    $r = $fn.OpenExe.Invoke($building)
    Write-Host ("      OpenExe                                -> {0}" -f $r)
    if ($r -ne 1) { $failed = 'OpenExe returned 0'; throw 'openexe' }

    $logSeen = 0
    foreach ($p in $plan) {
        $r = $fn.PatchWithXml.Invoke($p.Path)
        $note = ''
        $all = ReadLogText $logPath
        if ($all.Length -gt $logSeen) { $note = $all.Substring($logSeen).Trim(); $logSeen = $all.Length }
        $records += [pscustomobject]@{ Index = $p.Index; Xml = $p.Path; Sha256 = $p.Sha256; Result = $r; Log = $note }
        Write-Host ("  {0,2}. PatchWithXml {1,-34} -> {2}" -f $p.Index, (Split-Path $p.Path -Leaf), $r)
        if ($note) { foreach ($l in ($note -split "`r?`n")) { if ($l.Trim()) { Write-Host "      | $l" -ForegroundColor Yellow } } }
        if ($r -ne 1) { $failed = "PatchWithXml returned 0 on $($p.Path)"; break }
    }

    if ($failed) { $null = $fn.CloseExeWithoutSave.Invoke(); throw 'patchfail' }

    $r = $fn.SaveExe.Invoke()
    Write-Host ("      SaveExe                                -> {0}" -f $r)
    if ($r -ne 1) { $null = $fn.CloseExeWithoutSave.Invoke(); $failed = 'SaveExe returned 0'; throw 'savefail' }

    $r = $fn.CloseExe.Invoke()
    Write-Host ("      CloseExe                               -> {0}" -f $r)
    if ($r -ne 1) { $failed = 'CloseExe returned 0'; throw 'closefail' }
}
catch {
    [IO.Directory]::SetCurrentDirectory($prevCwd)
    # The DLL's log is a buffered ofstream held in a static; unloading runs its destructor
    # and flushes it. Nothing is called into the DLL after this point.
    $null = [GplPatch]::FreeLibrary($hDll)
    $tail = ReadLogText $logPath
    if ($tail.Trim()) { Write-Host ""; Write-Host "GPLPatchLog.txt:" -ForegroundColor Yellow; Write-Host $tail }
    if (Test-Path -LiteralPath $building) { Remove-Item -LiteralPath $building -Force }
    Remove-Item -LiteralPath $logDir -Recurse -Force -ErrorAction SilentlyContinue
    if (-not $failed) { $failed = $_.Exception.Message }
    Die "$failed  -- no output was written."
}
[IO.Directory]::SetCurrentDirectory($prevCwd)
# Unload the DLL so its static, buffered GPLPatchLog.txt stream is destroyed and flushed.
# Nothing is called into the DLL after this point.
$null = [GplPatch]::FreeLibrary($hDll)

# ---------------------------------------------------------------- carry bytes ----
$carryReport = @()
if ($carry.Count -gt 0) {
    $bytes = [IO.File]::ReadAllBytes($building)
    foreach ($c in $carry) {
        if (($c.Offset + $c.Length) -gt $bytes.Length) {
            Remove-Item -LiteralPath $building -Force
            Remove-Item -LiteralPath $logDir -Recurse -Force -ErrorAction SilentlyContinue
            Die ("-CarryBytes 0x{0:x}+{1} runs past the end of the produced exe ({2} bytes) -- no output was written." -f $c.Offset, $c.Length, $bytes.Length)
        }
        $donorBytes = New-Object byte[] $c.Length
        $ds = [IO.File]::OpenRead($c.Donor)
        try { $null = $ds.Seek($c.Offset, [IO.SeekOrigin]::Begin); $null = $ds.Read($donorBytes, 0, $c.Length) } finally { $ds.Dispose() }
        $before = New-Object byte[] $c.Length
        [Array]::Copy($bytes, $c.Offset, $before, 0, $c.Length)
        [Array]::Copy($donorBytes, 0, $bytes, $c.Offset, $c.Length)
        $bh = ($before     | ForEach-Object { $_.ToString('x2') }) -join ''
        $ah = ($donorBytes | ForEach-Object { $_.ToString('x2') }) -join ''
        Write-Host ("      carry 0x{0:x} +{1,-4} was {2}" -f $c.Offset, $c.Length, $bh)
        Write-Host ("                         now {0}" -f $ah)
        $carryReport += [pscustomobject]@{ Donor=$c.Donor; DonorSha256=$c.Sha256; Offset=$c.Offset; Length=$c.Length; Before=$bh; After=$ah }
    }
    [IO.File]::WriteAllBytes($building, $bytes)
}

# ---------------------------------------------------------------- large address aware ----
# IMAGE_FILE_LARGE_ADDRESS_AWARE (0x20) in the COFF header's Characteristics, which no XML can
# reach: GPLPatchDLL maps an XML's VirtualAddress through the section table, so the headers are
# out of its range. Without it the 32-bit process has 2 GiB instead of 4 on 64-bit Windows.
# The PE checksum is left as it is: the loader does not check it for an exe.
$laaReport = $null
if (-not $NoLargeAddressAware) {
    $bytes = [IO.File]::ReadAllBytes($building)
    $pe = if ($bytes.Length -ge 0x40) { [BitConverter]::ToInt32($bytes, 0x3C) } else { -1 }
    if ($pe -lt 0 -or ($pe + 24) -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) {
        Remove-Item -LiteralPath $building -Force
        Remove-Item -LiteralPath $logDir -Recurse -Force -ErrorAction SilentlyContinue
        Die "the produced exe has no PE header at the offset 0x3C names -- no output was written."
    }
    $at = $pe + 22
    $before = [BitConverter]::ToUInt16($bytes, $at)
    $after = $before -bor 0x20
    $bytes[$at] = [byte]($after -band 0xFF)
    $bytes[$at + 1] = [byte]($after -shr 8)
    [IO.File]::WriteAllBytes($building, $bytes)
    Write-Host ("      large address aware: Characteristics 0x{0:X4} -> 0x{1:X4} (file offset 0x{2:X})" -f $before, $after, $at)
    $laaReport = [pscustomobject]@{ Offset = $at; Before = ('0x{0:X4}' -f $before); After = ('0x{0:X4}' -f $after) }
} else {
    Write-Host "      large address aware: left as the DLL saved it (-NoLargeAddressAware)" -ForegroundColor Yellow
}

# ---------------------------------------------------------------- finish ----
if (Test-Path -LiteralPath $OutFull) { Remove-Item -LiteralPath $OutFull -Force }
Move-Item -LiteralPath $building -Destination $OutFull

$outHash = Sha256 $OutFull
$outSize = (Get-Item -LiteralPath $OutFull).Length
$log = ReadLogText $logPath
Remove-Item -LiteralPath $logDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "  result   $OutFull"
Write-Host "           sha256 $outHash  size $outSize"
if ($log.Trim()) {
    Write-Host ""
    Write-Host "GPLPatchLog.txt (the DLL's own notes for this build):" -ForegroundColor Yellow
    Write-Host $log
} else {
    Write-Host "           GPLPatchLog.txt was empty -- the DLL reported no warnings." -ForegroundColor DarkGray
}

$ok = $true
if ($Expect) {
    if ($outHash -eq $Expect.ToLower()) {
        Write-Host ""
        Write-Host "  MATCHES -Expect" -ForegroundColor Green
    } else {
        Write-Host ""
        Write-Host "  DOES NOT MATCH -Expect ($($Expect.ToLower()))" -ForegroundColor Red
        $ok = $false
    }
}

if ($Manifest) {
    $m = [pscustomobject]@{
        tool         = 'gplexegen.ps1'
        utc          = (Get-Date).ToUniversalTime().ToString('o')
        dll          = $DllFull
        dllSha256    = (Sha256 $DllFull)
        source       = $SourceFull
        sourceSha256 = $srcHash
        sourceSize   = $srcSize
        xml          = $records
        carry        = $carryReport
        largeAddressAware = $laaReport
        output       = $OutFull
        outputSha256 = $outHash
        outputSize   = $outSize
        gplPatchLog  = $log
    }
    $m | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Manifest -Encoding UTF8
    Write-Host "  manifest $Manifest"
}
Write-Host ""
if (-not $ok) { exit 4 }
exit 0
