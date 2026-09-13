<#
.SYNOPSIS
  One-shot 144 fps build: generate the AI constants XML, rescale the AI ini, build the exe.

.DESCRIPTION
  Runs the three tools in order with the exact recipe of the working build
  (out/gplc67-144.json), producing:

      tools\78 144fps v34.xml                (gplrategen.py)
      out\gpla67-144.ini                     (gplaiscale.py)
      out\gplc67-144-v34.exe                 (gplexegen.ps1 + GPLPatchDLL.dll)

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

.PARAMETER Donor
  A copy of a WORKING, UNPATCHED gplc67.exe -- the 1967 mod as GEM+ built it, untouched by
  any frame-rate patch. Every stock value is checked against it before being replaced, and
  the CD key is carried across from it, which no XML can supply. Default:
  <GplDir>\gplc67_36.exe, which is this project's naming convention rather than anything
  the game ships. Make one by copying gplc67.exe before patching.

.PARAMETER Ini36
  A copy of gpla67.ini as it was before any rescaling. Read, never written. Default:
  <GplDir>\gpla67.ini.36.

.PARAMETER Rate
  Simulation rate in Hz. Default 144. The exe build stays pinned to 144; other rates are
  for experimenting with the XML and the ini, which -SkipExe will produce on their own.

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
    [string] $Donor,
    [string] $Ini36,
    [double] $Rate = 144,
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
$outDir  = Join-Path $proj 'out'
$gplDir  = $GplDir
$gpl67   = Join-Path $gplDir 'Mods\GEM+\gpl67'
$source  = Join-Path $gplDir 'gpl.exe'
# The donor is a copy of a WORKING, UNPATCHED gplc67.exe: the 1967 mod as GEM+ built it,
# untouched by any frame-rate patch. Every stock value is checked against it before being
# replaced, and the CD key is carried across from it. The .36 suffix is this project's
# convention, not something the game ships -- name yours with -Donor if it differs.
$donor   = if ($Donor) { $Donor } else { Join-Path $gplDir 'gplc67_36.exe' }
# Likewise: a copy of gpla67.ini as it was before any rescaling. Read, never written.
$ini36   = if ($Ini36) { $Ini36 } else { Join-Path $gplDir 'gpla67.ini.36' }

foreach ($need in @(
    @{ p = $gplDir; f = '-GplDir';  d = $true;  what = 'the GPL install directory' },
    @{ p = $gpl67;  f = '-GplDir';  d = $true;  what = "the gpl67 mod XMLs (Mods\GEM+\gpl67)" },
    @{ p = $source; f = '-GplDir';  d = $false; what = 'stock gpl.exe, the patch source' },
    @{ p = $donor;  f = '-Donor';   d = $false; what = 'an unpatched gplc67.exe to read stock values and the CD key from' },
    @{ p = $ini36;  f = '-Ini36';   d = $false; what = 'an unpatched 36 Hz gpla67.ini to rescale' }
)) {
    $ok = if ($need.d) { Test-Path -LiteralPath $need.p -PathType Container }
          else { Test-Path -LiteralPath $need.p -PathType Leaf }
    if (-not $ok) {
        throw (("cannot find {0}:`n    {1}`nPass {2}, or run this from inside the GPL " +
                "install so -GplDir defaults to it.") -f $need.what, $need.p, $need.f)
    }
}

$aiXml   = Join-Path $tools ('78 {0}fps v34.xml' -f $Rate)
$outExe  = Join-Path $outDir 'gplc67-144-v34.exe'
$manifest= Join-Path $outDir 'gplc67-144-v34.json'
$aiIni   = Join-Path $outDir ('gpla67-{0}.ini' -f $Rate)

New-Item -ItemType Directory -Force $outDir | Out-Null

# The two optional FFB patches. NEITHER IS A RATE FIX -- GPL's FFB averager runs inside the
# physics substep loop, so it is 288 Hz at every tick rate. They are preferences, off by
# default, and equally valid on a 36 Hz build.
$ffbCondXml = Join-Path $tools '81 ffb no spring damper v1.xml'
$ffbCurveXml = Join-Path $tools '82 ffb deeper notch v1.xml'
if ($FfbNoSpringDamper -or $FfbDeeperNotch) {
    Write-Host "`n== 0c/4 gplffb.py: optional force-feedback patches =="
    $ffbArgs = @('--game-dir', $gplDir, '--exe', $donor)
    if ($FfbNoSpringDamper) { $ffbArgs += @('--out-spring-damper', $ffbCondXml) }
    if ($FfbDeeperNotch) { $ffbArgs += @('--out-curve', $ffbCurveXml,
                                         '--notch-depth', $FfbNotchDepth) }
    & $py (Join-Path $tools 'gplffb.py') @ffbArgs
    if ($LASTEXITCODE) { throw "gplffb.py failed ($LASTEXITCODE)" }
}

Write-Host "`n== 1/4 gplrategen.py: the $Rate Hz patch XML =="
& $py (Join-Path $tools 'gplrategen.py') --rate $Rate --game-dir $gplDir `
    --exe $donor --out $aiXml
if ($LASTEXITCODE) { throw "gplrategen.py failed ($LASTEXITCODE)" }

Write-Host "`n== 2/4 gplaiscale.py: AI ini for $Rate Hz =="
& $py (Join-Path $tools 'gplaiscale.py') --rate $Rate --game-dir $gplDir `
    --input $ini36 --out $aiIni
if ($LASTEXITCODE) { throw "gplaiscale.py failed ($LASTEXITCODE)" }

if ($SkipExe) { Write-Host "`n-SkipExe: done."; exit 0 }
if ($Rate -ne 144) { throw "the exe recipe is pinned to 144" }

# The XML list of the known-good build. Order matters: gpl67's own numbered patches, then
# ours. Nothing third-party appears here -- the 144 Hz XML is self-contained, and applying
# 77G alongside it would land on the same caves with no collision detection to catch it.
$gem = @(
    '05.xml','09.xml','10.xml','15.xml','20.xml','21.xml','22.xml','23.xml','24.xml',
    '25.xml','26.xml','30.xml','31.xml','32.xml','33.xml','40.xml','45.xml','50.xml',
    '60.xml','90.xml','95.xml')
$xmlList = @(
    $gem | ForEach-Object { Join-Path $gpl67 $_ }
) + @(
    (Join-Path $tools 'gpl67-carset-names.xml'),
    $aiXml
) + $(if ($FfbNoSpringDamper) { @($ffbCondXml) } else { @() }
) + $(if ($FfbDeeperNotch) { @($ffbCurveXml) } else { @() }
)

Write-Host "`n== 3/4 gplexegen.ps1: building $outExe =="
& (Join-Path $tools 'gplexegen.ps1') `
    -Source $source `
    -Xml $xmlList `
    -Out $outExe `
    -CarryBytes ("{0}@1280128:4" -f $donor), ("{0}@1358372:16" -f $donor) `
    -Manifest $manifest -Force
if ($LASTEXITCODE) { throw "gplexegen.ps1 failed ($LASTEXITCODE)" }

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
