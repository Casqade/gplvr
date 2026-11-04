<#
.SYNOPSIS
  One-shot gplc67 build: generate the AI constants XML, rescale the AI ini, build the exe.

.DESCRIPTION
  Runs the three tools in order, producing for -Rate 144:

      patches\78 144fps v34 <link>.xml       (gplrategen.py)
      patches\gpla67-144.ini                 (gplaiscale.py)
      patches\gpla67-144-changes.md          (gplaiscale.py: every AI key, its unit and value)
      out\gplc67-144[-prib]-<link>.exe       (gplexegen.ps1 + GPLPatchDLL.dll)

  and for -Rate 36 the exe alone, gplc67[-prib]-<link>.exe: GEM's 21 numbered XMLs on the
  base, then the optional XMLs below in a fixed order -- carset names (-GemExtras), the rate
  XML, the FFB preferences, enablepribluda.xml (-Pribluda), gplvr-load.xml (-LoadGplvr).
  05_unbindfix.xml (ours) follows 05.xml in every build. Every exe comes out
  /LARGEADDRESSAWARE (gplexegen sets it after the save; checked here).

  THE PATCH IS SELF-CONTAINED. Nothing third-party is applied: no 77G, no 60FPSv2
  scaffolding. gplrategen emits the rate, timer and sound-callback registrations, the
  substep constants as exact reciprocals, the replay version stamp, the multiplayer
  frame-rate handshake and the GUI event throttle itself. Three defects are removed by
  construction rather than repaired afterwards: 77G's overwrite of gpl_tickPeriodSeconds's
  only writer, its frozen sound interpolator constant, and its taskBegin cave reading
  dword_94D25C instead of dword_94D264.

  Install by copying the exe over the game's gplc67.exe and the ini over the game's
  gpla67.ini, or pass -Install and let it do that with a hash check.

  Any step failing stops the whole run. Every tool has its own refusal discipline --
  wrong stock bytes, XML collisions, sign-flipped immediates all abort rather than
  produce a wrong artefact.

.PARAMETER GplDir
  The Grand Prix Legends install directory. Defaults to the CURRENT DIRECTORY, so running
  this from inside the install needs no arguments at all. Everything it reads is checked
  before any work starts, and a missing input is a hard error naming the switch that
  supplies it.

.PARAMETER Python
  A python.exe that has `capstone`. Defaults to whatever `python` resolves to on PATH.

.PARAMETER Link
  UK or US: which of the two links of gpl.exe 1.2.0.2 the build is for. They share code and
  differ in where .data sits, and the rate XML carries payload operands for one link only, so
  the base, the donor and the XML must all agree -- each is checked with GPLPatchDLL's own
  probe (the dword at 0040E9C0) and a mismatch is refused. Default UK.

.PARAMETER Base
  The gpl.exe to patch. Default <GplDir>\gpl.exe. A base that already carries pribluda (as
  pribpatcher.exe leaves gpl.exe) is refused without -Pribluda, because the result would load
  pribluda while its name says it does not.

.PARAMETER Donor
  A copy of a WORKING, UNPATCHED gplc67.exe of the same link -- the 1967 mod as GEM+ built
  it, untouched by any frame-rate patch. Every stock value is checked against it before being
  replaced, and with -GemExtras the 31-byte field at 0054CC24 that GEM+ writes is carried
  across from it, which no XML can supply -- gplexegen.ps1's CarryBytes documents what that
  field is, and is not. Default: <GplDir>\gplc67_36.exe for UK, <GplDir>\gplc67_36-us.exe
  for US, which is this project's naming convention rather than anything the game ships.
  Make one by copying gplc67.exe before patching. Not needed at -Rate 36 without -GemExtras
  or an FFB preference.

.PARAMETER Ini36
  A copy of gpla67.ini as it was before any rescaling. Read, never written. Default:
  <GplDir>\gpla67.ini.36. At -Rate 36 it is what -Install puts back as gpla67.ini.
  Every value that is not rate-dependent passes through unchanged, so a personally tweaked
  .36 makes a personally tweaked patches\gpla67-144.ini. Keep it GEM+'s stock file for a
  build meant to be published.

.PARAMETER Rate
  Simulation rate in Hz. Default 144. The exe build takes 144, or 36 for GPL's own rate with
  no rate XML and no ini rescale; other rates are for experimenting with the XML and the ini,
  which -SkipExe will produce on their own.

.PARAMETER Pribluda
  Apply <GplDir>\enablepribluda.xml, GPL's own LoadLibraryA("pribluda") at the rasterizer
  start. The same three .text sites are right for both links.

.PARAMETER LoadGplvr
  Apply patches\gplvr-load.xml: the exe loads gplvr\gplvr.dll itself before gpl_winMain, so
  gplvr runs however the game is launched, without gplvr.exe. See tools\gplvr-load.md.

.PARAMETER GemExtras
  The two things GEM+ adds beyond its XMLs, both UK-only here: gpl67-carset-names.xml and the
  donor's bytes at file offsets 1280128 (4) and 1358372 (16). Off by default, which is what
  every out\ variant since 2026-10-11 has been built without.

.PARAMETER OutDir
  Where the exe and its manifest go. Default <repo>\out. The rate XML and the rescaled AI ini
  go to <repo>\patches, which is published.

.PARAMETER SkipExe
  Generate the XML and the ini but do not build the exe.

.PARAMETER Install
  Copy the exe over the game's gplc67.exe and the ini over its gpla67.ini, then hash
  both to prove the copies landed. Use this rather than copying by hand: the game path
  contains [Games], which PowerShell reads as a wildcard character class, so a plain
  Copy-Item/Get-FileHash on it silently finds nothing -- and gpla67.ini ships READ-ONLY,
  which fails a copy that does get the path right.

.PARAMETER FfbNoSpringDamper
  Switch off the centring spring and the damper GPL installs in the wheel itself, leaving
  only the force the physics computes.

  Those two are DirectInput CONDITION effects: the wheel's own firmware computes them from
  where the wheel is and how fast it is moving. They never pass through the tick loop and
  never see the car -- no tyre load, no kerb, no slide -- so they are a fixed feel laid on
  top of the real signal, which travels as a separate constant-force effect. On a 1998
  wheel that was worth having; on a direct-drive or belt wheel it usually just removes a
  dead, rubbery overlay, and the wheel's own driver can supply both, tuned, if wanted.

  NOT a rate fix, and equally valid on a stock 36 Hz build.

.PARAMETER FfbDeeperNotch
  Deepen the force curve's notch from 1.20 to -FfbNotchDepth (default 1.33). This is what
  GEM's ffb3test1.xml was trying to do; see tools/gplffb.py for what that file actually
  does and why it is not usable. Also NOT a rate fix.

.PARAMETER FfbNotchDepth
  Notch depth for -FfbDeeperNotch. Stock is 1.20.

#>
[CmdletBinding()]
param(
    [string] $GplDir = $PWD.Path,
    [string] $Python,
    [ValidateSet('UK', 'US')] [string] $Link = 'UK',
    [string] $Base,
    [string] $Donor,
    [string] $Ini36,
    [double] $Rate = 144,
    [switch] $Pribluda,
    [switch] $LoadGplvr,
    [switch] $GemExtras,
    [string] $OutDir,
    [switch] $SkipExe,
    [switch] $Install,
    [switch] $FfbNoSpringDamper,
    [switch] $FfbDeeperNotch,
    [double] $FfbNotchDepth = 1.33
)

$ErrorActionPreference = 'Stop'

# Nothing here is tied to one machine and nothing reads an environment variable. -GplDir
# defaults to the CURRENT DIRECTORY, so running this from inside the GPL install needs no
# arguments; from anywhere else, pass it. Every input is checked before any work starts,
# and a missing one is a hard error naming the file and the switch that supplies it --
# carrying on with a default that happens to exist is how a patch gets built against the
# wrong reference.
if ($Python) {
    $py = $Python
} elseif (Get-Command python -ErrorAction Ignore) {
    $py = (Get-Command python).Source
} else {
    throw "no Python on PATH: pass -Python <path to python.exe>. It needs `capstone`."
}
if (-not (Test-Path -LiteralPath $py)) { throw "-Python does not exist: $py" }

$proj    = Split-Path -Parent $PSScriptRoot          # the repo root
$tools   = Join-Path $proj 'tools'
# Our own XMLs, the ones this script writes (rate, FFB) and the ones it only reads.
$patches = Join-Path $proj 'patches'
$outDir  = if ($OutDir) { $OutDir } else { Join-Path $proj 'out' }
$gplDir  = $GplDir
$gpl67   = Join-Path $gplDir 'Mods\GEM+\gpl67'
$source  = if ($Base) { $Base } else { Join-Path $gplDir 'gpl.exe' }
$linkTag = $Link.ToLower()
$stock36 = $Rate -eq 36
# The donor is a copy of a WORKING, UNPATCHED gplc67.exe: the 1967 mod as GEM+ built it,
# untouched by any frame-rate patch. Every stock value is checked against it before being
# replaced, and with -GemExtras GEM+'s 31-byte field at 0054CC24 is carried across from it.
# The .36 suffix is this project's convention, not something the game ships -- name yours with
# -Donor if it differs.
$donor   = if ($Donor) { $Donor }
           elseif ($Link -eq 'US') { Join-Path $gplDir 'gplc67_36-us.exe' }
           else { Join-Path $gplDir 'gplc67_36.exe' }
# Likewise: a copy of gpla67.ini as it was before any rescaling. Read, never written.
$ini36   = if ($Ini36) { $Ini36 } else { Join-Path $gplDir 'gpla67.ini.36' }
$pribXml = Join-Path $gplDir 'enablepribluda.xml'
$loadXml = Join-Path $patches 'gplvr-load.xml'

$needDonor = -not $stock36 -or $GemExtras -or $FfbNoSpringDamper -or $FfbDeeperNotch
$needIni36 = -not $stock36 -or $Install
$needs = @(
    @{ p = $gplDir; f = '-GplDir';  d = $true;  what = 'the GPL install directory' },
    @{ p = $gpl67;  f = '-GplDir';  d = $true;  what = "the gpl67 mod XMLs (Mods\GEM+\gpl67)" },
    @{ p = $source; f = '-Base';    d = $false; what = 'gpl.exe, the patch source' }
)
if ($needDonor) { $needs += @{ p = $donor; f = '-Donor'; d = $false; what = 'an unpatched gplc67.exe of the same link to read stock values from' } }
if ($needIni36) { $needs += @{ p = $ini36; f = '-Ini36'; d = $false; what = 'an unpatched 36 Hz gpla67.ini' } }
if ($Pribluda)  { $needs += @{ p = $pribXml; f = '-GplDir'; d = $false; what = 'enablepribluda.xml, as pribluda ships it' } }
if ($LoadGplvr) { $needs += @{ p = $loadXml; f = '(the repo)'; d = $false; what = 'patches\gplvr-load.xml' } }
foreach ($need in $needs) {
    $ok = if ($need.d) { Test-Path -LiteralPath $need.p -PathType Container }
          else { Test-Path -LiteralPath $need.p -PathType Leaf }
    if (-not $ok) {
        throw (("cannot find {0}:`n    {1}`nPass {2}, or run this from inside the GPL " +
                "install so -GplDir defaults to it.") -f $need.what, $need.p, $need.f)
    }
}

# WHICH LINK A FILE IS, by the probe GPLPatchDLL itself makes: the dword at 0040E9C0 names the
# first option global, 0054C5C0 in the UK link and 0054C5B8 in the US one. 0040E9C0 is in
# .text, whose file offset is the VA minus 0x400C00 in gpl.exe and every gplc67 alike.
function LinkOf([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    switch ([BitConverter]::ToUInt32($b, 0x40E9C0 - 0x400C00)) {
        0x0054C5C0 { return 'UK' }
        0x0054C5B8 { return 'US' }
    }
    return '?'
}
# pribpatcher.exe's string at 0x004C4FD5 (STATUS 2026-10-11): nine nops in a clean base.
function HasPribluda([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    return [Text.Encoding]::ASCII.GetString($b, 0x4C4FD5 - 0x400C00, 8) -eq 'pribluda'
}
# IMAGE_FILE_LARGE_ADDRESS_AWARE in the COFF Characteristics: 4 GiB for GPL instead of 2.
function IsLargeAddressAware([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    return ([BitConverter]::ToUInt16($b, $pe + 22) -band 0x20) -ne 0
}

if ((LinkOf $source) -ne $Link) {
    throw ("{0} is the {1} link, not {2}: pass -Base with a {2} gpl.exe, or -Link {1}." -f
           $source, (LinkOf $source), $Link)
}
if ($needDonor -and (LinkOf $donor) -ne $Link) {
    throw ("the donor {0} is the {1} link, not {2}: pass -Donor with a {2} gplc67.exe." -f
           $donor, (LinkOf $donor), $Link)
}
if ((HasPribluda $source) -and -not $Pribluda) {
    throw ("{0} already loads pribluda (pribpatcher.exe patched it). Pass -Pribluda, or -Base " +
           "with a clean gpl.exe.") -f $source
}
if ($GemExtras -and $Link -ne 'UK') {
    throw "-GemExtras is UK-only: the carried file offsets are the UK link's."
}

$aiXml   = Join-Path $patches ('78 {0}fps v34 {1}.xml' -f $Rate, $Link)
$name    = 'gplc67' + $(if ($stock36) { '' } else { '-{0}' -f $Rate }) +
           $(if ($Pribluda) { '-prib' } else { '' }) + '-' + $linkTag
$outExe  = Join-Path $outDir ($name + '.exe')
$manifest= Join-Path $outDir ($name + '.json')
$aiIni   = if ($stock36) { $ini36 } else { Join-Path $patches ('gpla67-{0}.ini' -f $Rate) }

New-Item -ItemType Directory -Force $outDir | Out-Null

# The two optional FFB patches. NEITHER IS A RATE FIX -- GPL's FFB averager runs inside the
# physics substep loop, so it is 288 Hz at every tick rate. They are preferences, off by
# default, and equally valid on a 36 Hz build.
$ffbCondXml = Join-Path $patches '81 ffb no spring damper v2.xml'
$ffbCurveXml = Join-Path $patches '82 ffb deeper notch v2.xml'
if ($FfbNoSpringDamper -or $FfbDeeperNotch) {
    Write-Host "`n== 0c/4 gplffb.py: optional force-feedback patches =="
    $ffbArgs = @('--game-dir', $gplDir, '--exe', $donor)
    if ($FfbNoSpringDamper) { $ffbArgs += @('--out-spring-damper', $ffbCondXml) }
    if ($FfbDeeperNotch) { $ffbArgs += @('--out-curve', $ffbCurveXml,
                                         '--notch-depth', $FfbNotchDepth) }
    & $py (Join-Path $tools 'gplffb.py') @ffbArgs
    if ($LASTEXITCODE) { throw "gplffb.py failed ($LASTEXITCODE)" }
}

if ($stock36) {
    Write-Host "`n== 1-2/4 skipped: 36 Hz is GPL's own rate, no rate XML and no ini rescale =="
} else {
    Write-Host "`n== 1/4 gplrategen.py: the $Rate Hz patch XML ($Link) =="
    & $py (Join-Path $tools 'gplrategen.py') --rate $Rate --game-dir $gplDir `
        --exe $donor --out $aiXml
    if ($LASTEXITCODE) { throw "gplrategen.py failed ($LASTEXITCODE)" }

    Write-Host "`n== 2/4 gplaiscale.py: AI ini for $Rate Hz =="
    & $py (Join-Path $tools 'gplaiscale.py') --rate $Rate --game-dir $gplDir `
        --input $ini36 --out $aiIni --markdown (Join-Path $patches ('gpla67-{0}-changes.md' -f $Rate))
    if ($LASTEXITCODE) { throw "gplaiscale.py failed ($LASTEXITCODE)" }
}

if ($SkipExe) { Write-Host "`n-SkipExe: done."; exit 0 }
if ($Rate -ne 144 -and -not $stock36) { throw "the exe recipe takes -Rate 144 or 36" }

# The XML list. Order matters: gpl67's own numbered patches, then ours. Nothing third-party
# appears here -- the 144 Hz XML is self-contained, and applying 77G alongside it would land on
# the same caves with no collision detection to catch it. gplvr-load writes modtext at
# 0x0098C000, above everything GEM's XMLs and the rate XML (to 0x00988BE5) write there.
$gem = @(
    '05.xml','09.xml','10.xml','15.xml','20.xml','21.xml','22.xml','23.xml','24.xml',
    '25.xml','26.xml','30.xml','31.xml','32.xml','33.xml','40.xml','45.xml','50.xml',
    '60.xml','90.xml','95.xml')
# 05_unbindfix.xml (ours) right after 05.xml: 05.xml raises the flagman limit to 127 and leaves
# the unbind reading at the stock 55 spacing, a crash leaving a track after one with more than
# 55 flagmen (see the file).
$xmlList = @(
    $gem | ForEach-Object {
        Join-Path $gpl67 $_
        if ($_ -eq '05.xml') { Join-Path $patches '05_unbindfix.xml' }
    }
) + $(if ($GemExtras) { @(Join-Path $patches 'gpl67-carset-names.xml') } else { @() }
) + $(if (-not $stock36) { @($aiXml) } else { @() }
) + $(if ($FfbNoSpringDamper) { @($ffbCondXml) } else { @() }
) + $(if ($FfbDeeperNotch) { @($ffbCurveXml) } else { @() }
) + $(if ($Pribluda) { @($pribXml) } else { @() }
) + $(if ($LoadGplvr) { @($loadXml) } else { @() }
)
$carry = if ($GemExtras) { @(("{0}@1280128:4" -f $donor), ("{0}@1358372:16" -f $donor)) }
         else { @() }

Write-Host "`n== 3/4 gplexegen.ps1: building $outExe =="
$genArgs = @{ Source = $source; Xml = $xmlList; Out = $outExe; Manifest = $manifest; Force = $true }
if ($carry) { $genArgs.CarryBytes = $carry }
& (Join-Path $tools 'gplexegen.ps1') @genArgs
if ($LASTEXITCODE) { throw "gplexegen.ps1 failed ($LASTEXITCODE)" }
if ((LinkOf $outExe) -ne $Link) { throw "$outExe came out as the $(LinkOf $outExe) link" }
if ((HasPribluda $outExe) -ne [bool]$Pribluda) { throw "$outExe pribluda state is not what was asked" }
if (-not (IsLargeAddressAware $outExe)) { throw "$outExe is not /LARGEADDRESSAWARE (gplexegen sets it)" }

Write-Host "`nDone."
Write-Host "  exe : $outExe   -> copy over $gplDir\gplc67.exe"
Write-Host "  ini : $aiIni    -> copy over $gplDir\gpla67.ini"

if ($Install) {
    Write-Host "`n== install =="
    foreach ($pair in @(@($outExe, (Join-Path $gplDir 'gplc67.exe')),
                        @($aiIni,  (Join-Path $gplDir 'gpla67.ini')))) {
        $src, $dst = $pair
        if (Test-Path -LiteralPath $dst) {
            Set-ItemProperty -LiteralPath $dst -Name IsReadOnly -Value $false
        }
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $a = (Get-FileHash -LiteralPath $src).Hash
        $b = (Get-FileHash -LiteralPath $dst).Hash
        if ($a -ne $b) { throw "install of $dst did not take (hash mismatch)" }
        Write-Host ("  ok  {0}  {1}" -f $dst, $a.Substring(0, 16))
    }
    Write-Host ("  {0} is the 36 Hz source and was not touched." -f $ini36)
}
