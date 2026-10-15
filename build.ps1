<#
.SYNOPSIS
Configure + build the PS1 executable and CD image (PowerShell twin of build.sh).

.DESCRIPTION
Usage:  .\build.ps1 [iso|clean] [-BuildDir d] [-Grp f] [-Sf2 f] [-Viewer ON] [-ExtraCFlags "-DFOO=1"]
                    [-GameOpt -Os] [-GameKeep "actors;player"] [-Python python.exe]
(from cmd.exe: build.cmd [iso|clean] ...; Windows PowerShell 5.1 and PowerShell 7 both work. If build.cmd, i.e.
 PowerShell's -File mode, reports that a parameter named like a compiler flag cannot be found, attach the value
 with a colon: build.cmd iso -ExtraCFlags:"-DPS1_NOAUDIO=1", or use the environment variable instead.)

  (no target)  build the executable and run the RAM budget check (CMake target `ramcheck`, prints
               "RAM: _end=0x..., stack reserve N KB" and fails when less than 32 KB of stack reserve is left)
  iso          additionally build DUKE.DAT and the CD image (duke3d.bin/.cue)
  clean        delete the build directory

Every setting can be given as a parameter or as the environment variable used by build.sh; a parameter that
is given wins over the environment variable. Settings are only passed to CMake when given, so a configured
build directory keeps its cached values.

  -BuildDir      env BUILD_DIR        build directory, relative to this script (default: build)
  -Grp           env DUKE_GRP         path of DUKE3D.GRP (CMake default: %USERPROFILE%\Downloads\DUKE3D\DUKE3D.GRP)
  -Sf2           env DUKE_SF2         SoundFont for the music instrument bank (default: built-in synthesis)
  -Viewer        env DUKE_VIEWER      ON = build the engine map viewer instead of the game
  -ExtraCFlags   env EXTRA_CFLAGS     extra compiler flags, e.g. "-DPS1_NOAUDIO=1" (quote values that start with "-")
  -GameOpt       env DUKE_GAME_OPT    optimisation flag for src/game/*.c (CMake default -Os; '' = the SDK's -O2)
  -GameKeep      env DUKE_GAME_KEEP   game sources (base names) that keep the SDK -O2 (default "actors")
  -Python        env DUKE_PYTHON      Python 3 interpreter for tools/mkdata.py (default: found by CMake)

A parameter given as an explicit empty string (-ExtraCFlags '', -Sf2 '', -GameOpt '', -GameKeep '') is passed
to CMake as an empty value, which clears the cached value of that build directory. Environment variables cannot
do that in PowerShell (setting one to "" deletes it); build.sh treats a set-but-empty variable the same way.
Empty -Grp / -Viewer / -Python are ignored, as in build.sh.

Layout: Windows uses tools\sdk\PSn00bSDK-0.24-win32 (which bundles gcc, ninja, mkpsxiso, elf2x) and the Ninja
generator; the toolchain is tools\gcc when tools\gcc\bin\mipsel-none-elf-gcc.exe exists, otherwise the SDK
directory itself. Under PowerShell on Linux/macOS it uses tools/sdk/PSn00bSDK-0.24-Linux, tools/gcc and the
default CMake generator, exactly like build.sh. Changing generator in an existing build dir needs `clean`.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Target = 'ramcheck',
    [string]$BuildDir,
    [string]$Grp,
    [string]$Sf2,
    [string]$Viewer,
    [string]$ExtraCFlags,
    [string]$GameOpt,
    [string]$GameKeep,
    [string]$Python
)

$ErrorActionPreference = 'Stop'
$onWin = ($env:OS -eq 'Windows_NT')

# Parameter > environment variable > not given ($null). $KeepEmpty: an explicit empty value counts as given.
function Get-Setting($Bound, [string]$Key, [string]$EnvName, [bool]$KeepEmpty) {
    if ($Bound.ContainsKey($Key)) {
        $v = [string]$Bound[$Key]
    } else {
        $v = [Environment]::GetEnvironmentVariable($EnvName)
    }
    if ($null -eq $v) { return $null }
    if (($v -eq '') -and (-not $KeepEmpty)) { return $null }
    return $v
}

function Fwd([string]$p) { return $p.Replace('\', '/') }

$P = $PSScriptRoot
if (-not $P) { $P = (Get-Location).Path }

$bd = Get-Setting $PSBoundParameters 'BuildDir' 'BUILD_DIR' $false
if (-not $bd) { $bd = 'build' }
if ([IO.Path]::IsPathRooted($bd)) { $B = $bd } else { $B = Join-Path $P $bd }

if ($Target -eq 'clean') {
    if (Test-Path -LiteralPath $B) { Remove-Item -LiteralPath $B -Recurse -Force }
    exit 0
}

if ($onWin) {
    $sdk = Join-Path (Join-Path $P 'tools') 'sdk'
    $sdk = Join-Path $sdk 'PSn00bSDK-0.24-win32'
} else {
    $sdk = Join-Path (Join-Path $P 'tools') 'sdk'
    $sdk = Join-Path $sdk 'PSn00bSDK-0.24-Linux'
}
$gcc = Join-Path $P 'tools'
$gcc = Join-Path $gcc 'gcc'
$gccExe = 'mipsel-none-elf-gcc'
if ($onWin) { $gccExe = 'mipsel-none-elf-gcc.exe' }
if ($onWin -and -not (Test-Path -LiteralPath (Join-Path (Join-Path $gcc 'bin') $gccExe))) {
    # No separate GCC download: the SDK zip bundles the toolchain in its own bin/.
    $tc = $sdk
} else {
    $tc = $gcc
}

$toolchainFile = Join-Path $sdk 'lib'
$toolchainFile = Join-Path $toolchainFile 'libpsn00b'
$toolchainFile = Join-Path $toolchainFile 'cmake'
$toolchainFile = Join-Path $toolchainFile 'sdk.cmake'
if (-not (Test-Path -LiteralPath $toolchainFile)) {
    [Console]::Error.WriteLine("error: PSn00bSDK not found: $toolchainFile is missing (unpack the SDK zip into tools/sdk/, see README.md)")
    exit 1
}

# Same PATH layout as build.sh: separate GCC first, then the SDK's own bin.
$sep = [IO.Path]::PathSeparator
$env:PATH = (Join-Path $gcc 'bin') + $sep + (Join-Path $sdk 'bin') + $sep + $env:PATH

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    [Console]::Error.WriteLine('error: cmake (>= 3.21) not found in PATH')
    exit 1
}

$cfg = @(
    '-S', (Fwd $P), '-B', (Fwd $B),
    ('-DCMAKE_TOOLCHAIN_FILE=' + (Fwd $toolchainFile)),
    ('-DPSN00BSDK_TC=' + (Fwd $tc)),
    '-DCMAKE_BUILD_TYPE=Release'
)
if ($onWin) {
    # Visual Studio generators cannot drive the MIPS cross compiler; use Ninja (bundled in the SDK's bin/).
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        [Console]::Error.WriteLine('error: ninja not found (it is bundled in the PSn00bSDK win32 zip under bin/, or put ninja.exe in PATH)')
        exit 1
    }
    $cfg += @('-G', 'Ninja')
}

# name of the CMake variable, parameter key, environment variable, explicit empty value counts as given
$settings = @(
    @('DUKE_GRP',       'Grp',         'DUKE_GRP',       $false),
    @('DUKE_SF2',       'Sf2',         'DUKE_SF2',       $true),
    @('DUKE_VIEWER',    'Viewer',      'DUKE_VIEWER',    $false),
    @('Python3_EXECUTABLE', 'Python',  'DUKE_PYTHON',    $false),
    @('DUKE_GAME_OPT',  'GameOpt',     'DUKE_GAME_OPT',  $true),
    @('DUKE_GAME_KEEP', 'GameKeep',    'DUKE_GAME_KEEP', $true),
    @('EXTRA_CFLAGS',   'ExtraCFlags', 'EXTRA_CFLAGS',   $true)
)
foreach ($s in $settings) {
    $v = Get-Setting $PSBoundParameters $s[1] $s[2] $s[3]
    if ($null -ne $v) {
        if ($s[1] -eq 'Grp' -or $s[1] -eq 'Sf2' -or $s[1] -eq 'Python') { if ($v -ne '') { $v = Fwd $v } }
        $cfg += ('-D' + $s[0] + '=' + $v)
    }
}

# Native tools write progress to stderr; with 'Stop' that would turn into terminating errors in Windows
# PowerShell 5.1, so success is judged by the exit code only.
$ErrorActionPreference = 'Continue'

& cmake @cfg > $null
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build (Fwd $B) -j8 --target $Target
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
exit 0
