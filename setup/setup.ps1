<#
.SYNOPSIS
Builds Modern Warfare 3 for Android (APK) and/or Windows from your own disc image;
with -Game mw2, Modern Warfare 2 from that game's disc image; with
-Game cod4, Call of Duty 4: Modern Warfare from its;
with -Game aw, Call of Duty: Advanced Warfare from its two (docs/AW-BRINGUP.md).

.DESCRIPTION
One command for a Windows PC: setup\setup.cmd (or this script). It
  1. fetches every tool it needs into .toolchain\ beside the repository
     (portable: nothing installed system-wide, no PATH or registry changes
     outside this process). The one exception is Microsoft's C++ Build Tools,
     which the recompiler is built with: if they are missing it offers to
     install them with winget, and stops if you say no;
  2. asks for your disc image (.iso) and reads the two executables out of it
     (the image is only ever opened for reading);
  3. checks each executable's SHA-256 against the disc builds this port
     supports (tools/check_mw3_xex.py, tools/check_cod4_xex.py; Modern Warfare 2's one disc) and
     refuses any other;
  4. recompiles them to C++ (XenonRecomp) and builds the game code for
     Android (a package of its own, with no screen: the campaign's, Special
     Ops' and the multiplayer's) and, if asked, the Windows game;
  5. installs the game code and the app on a phone connected over USB, if
     asked. The app is the published one, downloaded from the project's
     releases: it contains none of the game, finds the game code you built
     beside it, and updates itself from then on (docs/game-library.md).
Everything it makes is in the git-ignored out\, mw3\ (mw2\, cod4\, aw\), ppc_*\ and
XenonRecomp\ folders: it contains code made from your copy of the game and is
for your own devices only. Never upload or share it.
Re-runnable: finished steps are skipped. A first run takes one to several
hours and about 40 GB of disk (60 GB with the Windows game).

.PARAMETER Game
mw3 (default), mw2, cod4 or aw. Modern Warfare 2 is built for Android and
Windows as Modern Warfare 3 is: from the disc's own executables (no title
update), with the published app (com.mw2.recomp); docs/MW2-ANDROID.md.
Call of Duty 4 (docs/COD4-BRINGUP.md) is built for Android and Windows as
Modern Warfare 3 is, with the published app (com.cod4.recomp).
Advanced Warfare (docs/AW-BRINGUP.md) is built for Android and Windows too,
with the published app (com.aw.recomp).
.PARAMETER Iso
The disc image. Asked for when not given.
.PARAMETER InstallIso
Advanced Warfare's second disc, the install disc, whose two content packages
the game mounts as a console does after installing them to its hard disk.
Asked for when they are not yet in aw\install.
.PARAMETER Dlc
The folder with your own downloadable content, copied off your console's
hard disk: Content\0000000000000000\<the game's number> (415608CB for Modern
Warfare 3, 41560817 for Modern Warfare 2, 415607E6 for Call of Duty 4,
41560914 for Advanced Warfare), which holds the map packs in 00000002;
docs/dlc.md. The packages are copied to the phone's app, and the Windows game
is told where they are. Advanced Warfare's packs need its title update too
(000B0000 in that folder): its multiplayer is then built a second time, as
the update makes it, which is the one the map packs and Exo Zombies are for.
The folder is only read.
.PARAMETER Title
both (default), sp (campaign and Special Ops) or mp (multiplayer): which
Windows executables to build. The Android app always holds both.
.PARAMETER Build
android (default), pc, or both. Asked for when not given.
.PARAMETER Install
Install the built APK on the connected phone (adb) without asking.
.PARAMETER NoInstall
Build only: nothing is installed on or copied to any phone, whatever is
connected.
.PARAMETER PushIso
Copy the disc image to the phone's Download folder, for the app's installer.
.PARAMETER Instance
2..9 with -Title mp: instead of the app, a copy of the multiplayer alone
(com.mw3.recomp.mp2) to run beside it on one phone, for testing two players
on one device.
.PARAMETER Recomp
Recompile even when a recompiled tree is already there.
A tree made with an older recompiler or configuration than this checkout's is
recompiled without being asked; the files that come out as they were are not
compiled again.
.PARAMETER GameOnly
On Android: build and install the game code only, and leave the app as it
is. For after `git pull`, when an update of the app expects other game code:
only what changed is built again, and nothing is installed when the phone
has that code already.
.PARAMETER Dev
On Android: build the app here instead of downloading the published one. It
is signed with this PC's debug key, so it does not update from the published
app; -Suffix puts it beside that one.
.PARAMETER Suffix
With -Dev or -GameOnly: a suffix for the app's package name and the game
code's (.dev gives com.mw3.recomp.dev and com.mw3.recomp.dev.game), so a
development build lives beside the published app, with its own game folder.
.PARAMETER Check
Read-only: report which prerequisites are present and what would be
downloaded or installed, then stop. Nothing is written.
.PARAMETER Yes
Ask nothing: take the defaults (both titles, Android only) and the switches
given. Licence terms of the Android SDK are accepted on your behalf. With
more than one phone connected and no -Device, nothing is installed: the
build is made and the command for each phone is printed.
.PARAMETER Device
The phone to install on, by its adb serial (`adb devices`). Without it you
are asked about the one connected, or the first of several; a run that asks
nothing (-Yes) installs only on a phone named here, and on none when that
phone is not connected.
.PARAMETER Toolchain
Where the tools go (default: .toolchain\ in the repository).
.PARAMETER CamoPacks
A folder of PC camouflage mods you downloaded (zip, rar, 7z, iwd, or loose
.iwi pictures, in any layout): each becomes a camouflage pack of the
multiplayer, listed below Gold beside the game's own (docs/textures.md).
Without it, the folder setup\camo-packs is used when it is there.

.EXAMPLE
setup\setup.cmd
.EXAMPLE
setup\setup.cmd -Iso "D:\Games\MW3.iso" -Build both -Install
.EXAMPLE
setup\setup.cmd -Check
.EXAMPLE
setup\setup.cmd -GameOnly
.EXAMPLE
setup\setup.cmd -Game mw2 -Iso "D:\Games\MW2.iso" -Install
#>
[CmdletBinding()]
param(
    [ValidateSet('mw3', 'mw2', 'cod4', 'aw')][string]$Game = 'mw3',
    [string]$Iso,
    [string]$InstallIso,
    [string]$Dlc,
    [ValidateSet('both', 'sp', 'mp')][string]$Title,
    [ValidateSet('android', 'pc', 'both')][string]$Build,
    [switch]$Install,
    [switch]$NoInstall,
    [switch]$PushIso,
    [ValidateRange(1, 9)][int]$Instance = 1,
    [switch]$Recomp,
    [switch]$GameOnly,
    [switch]$Dev,
    [ValidatePattern('^(\.[a-z][a-z0-9]*)?$')][string]$Suffix = '',
    [switch]$Check,
    [switch]$Yes,
    [string]$Device,
    [string]$Toolchain,
    [string]$CamoPacks
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Repo = Split-Path -Parent $PSScriptRoot
$Tc   = if ($Toolchain) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Toolchain) } else { Join-Path $Repo '.toolchain' }
$Dl   = Join-Path $Tc 'dl'
$Out  = Join-Path $Repo 'out'
$Logs = Join-Path $Out 'logs'
$Tar  = Join-Path $env:SystemRoot 'System32\tar.exe'
$Interactive = -not $Yes -and -not $Check -and [Environment]::UserInteractive

# Every download is pinned to a version and its SHA-256, so a friend's build
# uses the same tools as the one that was tested. The JDK is a fixed Temurin
# release rather than "latest" for the same reason.
$Pins = @{
    Jdk       = @{ File = 'jdk17.zip'; Sha = '98518ecf554465057ee5509462631deeee374bc368c12a411d225cd29381f84b'
                   Url = 'https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.21%2B9/OpenJDK17U-jdk_x64_windows_hotspot_17.0.21_9.zip' }
    Llvm      = @{ File = 'llvm-18.1.8.tar.xz'; Sha = '22c5907db053026cc2a8ff96d21c0f642a90d24d66c23c6d28ee7b1d572b82e8'
                   Url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/clang+llvm-18.1.8-x86_64-pc-windows-msvc.tar.xz' }
    CmdTools  = @{ File = 'cmdline-tools.zip'; Sha = '4d6931209eebb1bfb7c7e8b240a6a3cb3ab24479ea294f3539429574b1eec862'
                   Url = 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip' }
    CMake     = @{ File = 'cmake-3.31.8.zip'; Sha = '81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719'
                   Url = 'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip' }
    Ninja     = @{ File = 'ninja-1.12.1.zip'; Sha = 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a'
                   Url = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' }
    LlvmMingw = @{ File = 'llvm-mingw-20261104.zip'; Sha = '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f'
                   Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-x86_64.zip' }
    Python    = @{ File = 'python-3.13.9.nupkg'; Sha = 'ea7541c98f77b6f344460b88db6e677573069979bcbf0ba18747cc2c125aba25'
                   Url = 'https://api.nuget.org/v3-flatcontainer/python/3.13.9/python.3.13.9.nupkg' }
    Git       = @{ File = 'PortableGit-2.56.1.7z.exe'; Sha = '9dd04a608ceb5f6ab32a73709e0ecb159d211da71b697c45ff81f6dd0c6b49c7'
                   Url = 'https://github.com/git-for-windows/git/releases/download/v2.56.1.windows.1/PortableGit-2.56.1-64-bit.7z.exe' }
}
$XenonCommit  = 'ddd128bcca99fe8bfbb99bea583c972351fa6ace'   # as build.sh and tools/recomp_mw3.sh
$ReleaseRepo  = "yassinsolim/$Game-recomp"   # where the Android app is published (android/app/build.gradle.kts, UPDATE_REPO)
$AdrenoRemote = 'https://github.com/bylaws/libadrenotools.git'
$TurnipUrl    = 'https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v39/Turnip_v27.0.0-R2.zip'
$TurnipSha    = '8d30be022582d0c03b1918c019540491d00439e14e9e5e1359fe0bb19004f781'   # THIRD_PARTY.md
$SdkPackages  = 'ndk;28.2.13676358', 'platforms;android-35', 'build-tools;35.0.0', 'cmake;3.22.1', 'platform-tools'
$DefaultRelay = 'mw3.yassin.app:3074'   # Prefs.DEFAULT_RELAY_ADDRESS, the app's default

$Jdk       = Join-Path $Tc 'jdk17'
$Llvm      = Join-Path $Tc 'llvm'
$Sdk       = Join-Path $Tc 'android-sdk'
$CMakeDir  = Join-Path $Tc 'cmake'
$NinjaDir  = Join-Path $Tc 'ninja'
$MingwDir  = Join-Path $Tc 'llvm-mingw'
$PyDir     = Join-Path $Tc 'python'
$GitDir    = Join-Path $Tc 'git'
$CMake     = Join-Path $CMakeDir 'bin\cmake.exe'
$Ninja     = Join-Path $NinjaDir 'ninja.exe'
$SdkManager = Join-Path $Sdk 'cmdline-tools\latest\bin\sdkmanager.bat'
$Adb       = Join-Path $Sdk 'platform-tools\adb.exe'
$Xenon     = Join-Path $Repo 'XenonRecomp'
$XenonExe  = Join-Path $Xenon 'build\XenonRecomp\XenonRecomp.exe'
$AnalyseExe = Join-Path $Xenon 'build\XenonAnalyse\XenonAnalyse.exe'

# What differs between the disc's two executables (win_build.ps1's table), for
# each game. Modern Warfare 2's are the disc's version (tu0): its trees,
# configs and tables carry that in their names (tools/title.py).
$Mw2 = $Game -eq 'mw2'
$Cod4 = $Game -eq 'cod4'
$Aw = $Game -eq 'aw'
$GameName = if ($Mw2) { 'Modern Warfare 2' } elseif ($Cod4) { 'Call of Duty 4: Modern Warfare' } elseif ($Aw) { 'Call of Duty: Advanced Warfare' } else { 'Modern Warfare 3' }
$Disc = "$Game/tu0"
# Name is what the build calls the title (MW2_TITLE), Lib its library's name
# (CMakeLists.txt, MW2_LIBRARY_NAME).
$Titles = if ($Mw2) { @{
    sp = @{ Name = 'sp'; Lib = 'mw2'; Label = 'campaign and Special Ops'; Xex = 'default'; Ppc = 'ppc_tu0'; Toml = 'MW2.tu0.toml'
            Tables = 'mw2_switch_tables.tu0.toml'; Files = 113; Jump = 100
            Sha = '990e640d3a92544f9d585e71faf1ab65082d37ec6f5865b2599335fa123bf6ff' }
    mp = @{ Name = 'mp'; Lib = 'mw2mp'; Label = 'multiplayer'; Xex = 'default_mp'; Ppc = 'ppc_mp_tu0'; Toml = 'MW2MP.tu0.toml'
            Tables = 'mw2mp_switch_tables.tu0.toml'; Files = 117; Jump = 121
            Sha = 'aacc47aff3221f3bd9129e80827fd3bcd74aa43bf74bb5c5ddbd0597c0d5fcde' }
} } elseif ($Cod4) { @{
    # Stepped: the instructions the recompiler does not know and the
    # configuration steps over (config/COD4.toml), which it still reports.
    sp = @{ Name = 'cod4'; Lib = 'cod4'; Label = 'campaign'; Xex = 'default'; Ppc = 'ppc_cod4'; Toml = 'COD4.toml'
            Tables = 'cod4_switch_tables.toml'; Files = 76; Jump = 181; Stepped = 9 }
    mp = @{ Name = 'cod4mp'; Lib = 'cod4mp'; Label = 'multiplayer'; Xex = 'default_mp'; Ppc = 'ppc_cod4_mp'; Toml = 'COD4MP.toml'
            Tables = 'cod4mp_switch_tables.toml'; Files = 77; Jump = 179; Stepped = 9 }
} } elseif ($Aw) { @{
    # Stepped: 415 instructions of twelve kinds in each executable, which the
    # configuration steps over (config/AW.toml, tools/extra_instructions.py).
    sp = @{ Name = 'aw'; Lib = 'aw'; Label = 'campaign'; Xex = 'default'; Ppc = 'ppc_aw'; Toml = 'AW.toml'
            Tables = 'aw_switch_tables.toml'; Files = 207; Jump = 210; Stepped = 415 }
    mp = @{ Name = 'awmp'; Lib = 'awmp'; Label = 'multiplayer'; Xex = 'default_mp'; Ppc = 'ppc_aw_mp'; Toml = 'AWMP.toml'
            Tables = 'awmp_switch_tables.toml'; Files = 204; Jump = 230; Stepped = 415 }
    # The multiplayer as title update 17 makes it, for a player's own map
    # packs (-Dlc): its executable is made here from the disc's and the
    # update's patch, in a folder of its own.
    mp17 = @{ Name = 'awmp17'; Lib = 'awmp17'; Label = 'multiplayer with the title update (map packs, Exo Zombies)'; Xex = 'default_mp'
              Folder = 'aw/tu17'; Ppc = 'ppc_aw_mp_tu17'; Toml = 'AWMP.tu17.toml'
              Tables = 'awmp_switch_tables.tu17.toml'; Files = 213; Jump = 137; Stepped = 415 }
} } else { @{
    sp = @{ Name = 'mw3'; Lib = 'mw3'; Label = 'campaign and Special Ops'; Xex = 'default'; Ppc = 'ppc_mw3'; Toml = 'MW3.toml'
            Tables = 'mw3_switch_tables.toml'; Files = 146; Jump = 128 }
    mp = @{ Name = 'mw3mp'; Lib = 'mw3mp'; Label = 'multiplayer'; Xex = 'default_mp'; Ppc = 'ppc_mw3_mp'; Toml = 'MW3MP.toml'
            Tables = 'mw3mp_switch_tables.toml'; Files = 153; Jump = 156 }
} }

$Clock = [Diagnostics.Stopwatch]::StartNew()
function Step($text) { Write-Host ("[{0:hh\:mm\:ss}] {1}" -f $Clock.Elapsed, $text) -ForegroundColor Cyan }
function Note($text) { Write-Host "    $text" }
function Warn($text) { Write-Host "    warning: $text" -ForegroundColor Yellow }

function Ask([string]$question, [bool]$default) {
    if (-not $Interactive) { return $default }
    $hint = if ($default) { '[Y/n]' } else { '[y/N]' }
    while ($true) {
        $answer = (Read-Host "$question $hint").Trim().ToLowerInvariant()
        if ($answer -eq '') { return $default }
        if ($answer -in 'y', 'yes') { return $true }
        if ($answer -in 'n', 'no') { return $false }
    }
}

function Choose([string]$question, [string[]]$options, [string]$default) {
    if (-not $Interactive) { return $default }
    while ($true) {
        $answer = (Read-Host "$question ($($options -join '/'), default $default)").Trim().ToLowerInvariant()
        if ($answer -eq '') { return $default }
        if ($answer -in $options) { return $answer }
    }
}

# Runs a native program, output to a log file (and the console unless -Quiet);
# throws on a non-zero exit. stderr is merged without tripping PowerShell 5.1.
function Exec {
    param([string]$Exe, [string[]]$Arguments, [string]$Log, [string]$Cwd, [switch]$Quiet)
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    if ($Cwd) { Push-Location $Cwd }
    try {
        if ($Log) {
            $path = Join-Path $Logs $Log
            if ($Quiet) { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $path }
            else { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $path }
        } else { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } }
        $code = $LASTEXITCODE
    } finally { if ($Cwd) { Pop-Location }; $ErrorActionPreference = $old }
    if ($code -ne 0) { throw "$Exe failed with exit code $code$(if ($Log) { " (log: $(Join-Path $Logs $Log))" })" }
}

function Sha256Of($path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() }

# A file's SHA-256 as cmake/game_abi.cmake takes it: of its bytes without the
# carriage returns, so that a checkout with Windows line endings and one
# without say the same.
function AbiHash($path) {
    $latin = [Text.Encoding]::GetEncoding(28591)
    $bytes = $latin.GetBytes($latin.GetString([IO.File]::ReadAllBytes($path)).Replace("`r", ''))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) } finally { $sha.Dispose() }
}

# What a tree recompiled now is made with, as the note left in the tree says
# it and as cmake/game_abi.cmake builds the game library's statement from it:
# the recompiler, this repository's patch to it, the title's configuration
# and the script that writes the tree's shared header.
function RecompiledWith($t) {
    "recompiler $XenonCommit`n" +
    "recompiler-patch $(AbiHash (Join-Path $Repo 'patches\xenonrecomp-mw2.patch'))`n" +
    "configuration $(AbiHash (Join-Path $Repo "config\$($t.Toml)"))`n" +
    "shared-header $(AbiHash (Join-Path $Repo 'tools\genshared.py'))`n"
}

# Whether a recompiled tree is there and was made with what this checkout
# recompiles with: the game code is checked against the app's
# (docs/game-library.md).
function Test-Tree($t) {
    $tree = Join-Path $Repo $t.Ppc
    if (-not (Test-Path (Join-Path $tree 'ppc_func_mapping.cpp'))) { return $false }
    $note = Join-Path $tree 'recompiled_with.txt'
    (Test-Path $note) -and ([IO.File]::ReadAllText($note).Replace("`r", '') -eq (RecompiledWith $t))
}

# Where a title's executable is, from the repository: the disc's folder, or
# the title's own (the one an update makes).
function XexOf($t) { "$(if ($t.Folder) { $t.Folder } else { $Disc })/$($t.Xex).xex" }

# Whether an extracted executable is a disc build this port supports, as one
# line of text; throws when it is not. Modern Warfare 3's builds are listed by
# tools/check_mw3_xex.py, Call of Duty 4's by tools/check_cod4_xex.py and
# Advanced Warfare's by tools/check_aw_xex.py;
# Modern Warfare 2 has the one disc (CMakeLists.txt, MW2_XEX_SHA256_SP/MP).
function Test-Xex($python, $t) {
    $xexRel = XexOf $t
    if ($Cod4) { return Exec $python @('tools/check_cod4_xex.py', $xexRel) -Cwd $Repo }
    if ($Aw) { return Exec $python @('tools/check_aw_xex.py', $xexRel) -Cwd $Repo }
    if (-not $Mw2) { return Exec $python @('tools/check_mw3_xex.py', $xexRel) -Cwd $Repo }
    $sha = Sha256Of (Join-Path $Repo $xexRel)
    if ($sha -ne $t.Sha) { throw "${xexRel}: SHA-256 $sha is not the Modern Warfare 2 disc's ($($t.Sha))" }
    "${xexRel}: the Modern Warfare 2 disc's (1.0.557)"
}

# MSVC/Windows SDK include and library paths, for this process only.
function Use-VsTools($vcvars) {
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^(INCLUDE|LIB|LIBPATH|PATH)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
    if (-not $env:INCLUDE) { throw "vcvars64.bat did not set INCLUDE ($vcvars)" }
}

# Downloads a pinned file into .toolchain\dl (once) and checks its hash.
function Fetch($pin) {
    $dest = Join-Path $Dl $pin.File
    if ((Test-Path $dest) -and (Sha256Of $dest) -eq $pin.Sha) { return $dest }
    Step "download $($pin.File)"
    # Silent but for errors: whatever reaches the pipeline here would become
    # part of the path this function returns.
    Exec 'curl.exe' @('-fsSL', '--retry', '3', '-o', "$dest.part", $pin.Url) | ForEach-Object { Write-Host $_ }
    if ((Sha256Of "$dest.part") -ne $pin.Sha) {
        Remove-Item -Force "$dest.part"
        throw "$($pin.File): the download does not match its SHA-256; not using it"
    }
    Move-Item -Force "$dest.part" $dest
    $dest
}

# Unpacks an archive whose contents sit in one top folder into $dest.
function Unpack($archive, $dest) {
    $tmp = "$dest.tmp"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    Exec $Tar @('-xf', $archive, '-C', $tmp)
    $top = @(Get-ChildItem $tmp)
    $source = if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $top[0].FullName } else { $tmp }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Move-Item $source $dest
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
}

# A Python 3.10+ that really runs (not the Microsoft Store's placeholder),
# by its full path; $null when there is none.
function Find-Python {
    # No double quotes in the -c program: PowerShell 5.1 drops them on the way
    # to a native program.
    foreach ($candidate in 'py -3', 'python', 'python3') {
        $parts = $candidate -split ' '
        $exe = $parts[0]
        if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { continue }
        $rest = @($parts | Select-Object -Skip 1)
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $found = & $exe @rest -c 'import sys; print(sys.executable if sys.version_info >= (3, 10) else str())' 2>$null
        $ErrorActionPreference = $old
        if ($LASTEXITCODE -eq 0 -and $found -and (Test-Path "$found")) { return "$found".Trim() }
    }
    $portable = Join-Path $PyDir 'tools\python.exe'
    if (Test-Path $portable) { return $portable }
    $null
}

# git, and the openssl.exe that ships with Git for Windows (tools/xexdump.py
# decrypts the executable with it).
function Find-Git {
    $portable = Join-Path $GitDir 'cmd\git.exe'
    if (Test-Path $portable) { return @{ Git = $portable; OpenSsl = (Join-Path $GitDir 'usr\bin\openssl.exe') } }
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return $null }
    $ssl = Get-Command openssl -ErrorAction SilentlyContinue
    $sslPath = if ($ssl) { $ssl.Source } else { Join-Path (Split-Path -Parent (Split-Path -Parent $git.Source)) 'usr\bin\openssl.exe' }
    if (-not (Test-Path $sslPath)) { return $null }
    @{ Git = $git.Source; OpenSsl = $sslPath }
}

function Find-VsTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) { return $null }
    $vcvars = Join-Path "$path".Trim() 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $vcvars) { $vcvars } else { $null }
}

function Get-Devices {
    if (-not (Test-Path $Adb)) { return @() }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $lines = & $Adb devices 2>$null
    $ErrorActionPreference = $old
    @($lines | Where-Object { $_ -match '^(\S+)\s+(device|unauthorized)$' } | ForEach-Object {
        $parts = $_ -split '\s+'; @{ Serial = $parts[0]; State = $parts[1] } })
}

# Which of the connected phones the APKs go on: the one -Device names, or the
# only one there is. With several and none named, somebody at the keyboard is
# warned and asked about the first; a run that asks nothing (-Yes, or no
# keyboard) gets none, because it would be a phone nobody chose: such a run,
# with two phones on one PC, put an app and 4 GB on the one adb listed first.
function Select-InstallDevice([object[]]$ready, [string]$device, [bool]$interactive) {
    $chosen = @($ready | Where-Object { $_.Serial -eq $device })
    if ($chosen.Count) { return $chosen[0].Serial }
    # A phone that was named and is not there is never replaced by another,
    # and a run that asks nothing takes none it was not given: such a run,
    # with one phone connected that was somebody else's at the time, put an
    # app and 4 GB on it.
    if ($device -or -not $interactive) { return $null }
    if ($ready.Count -gt 1) { Warn "more than one phone connected; using $($ready[0].Serial) (-Device <serial> chooses)" }
    return $ready[0].Serial
}

# Whether the phone's game code is the code just built: its package holds
# these libraries and no others, byte for byte. A run that changed nothing,
# or nothing of this game's code, then installs nothing.
function Test-GameCodeOnPhone($serial, $item) {
    try {
        $base = "$(Exec $Adb @('-s', $serial, 'shell', 'pm', 'path', $item.Package))".Trim()
        if ($base -notmatch '^package:(.+)/base\.apk$') { return $false }
        $there = @(Exec $Adb @('-s', $serial, 'shell', "sha256sum $($Matches[1])/lib/arm64/*.so") |
            ForEach-Object { if ("$_" -match '^([0-9a-f]{64})\s+.*/([^/\s]+)\s*$') { "$($Matches[2]) $($Matches[1])" } } | Sort-Object)
        $here = @(Get-ChildItem -File $item.Libraries | ForEach-Object { "$($_.Name) $(Sha256Of $_.FullName)" } | Sort-Object)
        $here.Count -gt 0 -and ($there -join "`n") -eq ($here -join "`n")
    } catch { $false }
}

# Installs one of step 5's packages on the phone. Android replaces an app only
# with one signed by the same key, and the copies of this one are signed three
# ways: the published app with the project's release key, an app built here
# with this PC's debug key, one built elsewhere with that PC's. Going from one
# to another means uninstalling, which takes the app's folder on the phone
# with it -- the game files and the saves. So that folder is moved aside
# first and back afterwards (a rename, not a copy), with the player's leave.
# The game code's package holds nothing of the player's and is simply
# replaced. Never throws: the build is done by now, and what is wrong is said.
function Install-Package($serial, $item) {
    $name = Split-Path -Leaf $item.Apk
    if ($item.Libraries -and (Test-GameCodeOnPhone $serial $item)) {
        Note "the game code on $serial is the code just built: $name is not installed again."
        return
    }
    Step "adb install $name"
    $install = {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $text = (& $Adb -s $serial install -r $item.Apk 2>&1 | ForEach-Object { "$_" }) -join ' '
        $ErrorActionPreference = $old
        @{ Done = ($LASTEXITCODE -eq 0); Text = $text }
    }
    $result = & $install
    if ($result.Done) { return }
    # Another key, or (a build made here over the published app) an older version number.
    if ($result.Text -notmatch 'INSTALL_FAILED_UPDATE_INCOMPATIBLE|INSTALL_FAILED_VERSION_DOWNGRADE') { Warn "could not install $name on ${serial}: $($result.Text)"; return }
    try {
        if (-not $item.Data) {
            Exec $Adb @('-s', $serial, 'uninstall', $item.Package) | Out-Null
            $result = & $install
            if (-not $result.Done) { Warn "could not install $name on ${serial}: $($result.Text)" }
            return
        }
        Warn "the app on the phone ($($item.Package)) is not one Android will put $name over: it is signed with another key (one was built on a PC, the other is the published app) or is a newer version."
        if (-not (Ask 'Replace the app on the phone? Its game files and saves are kept; its settings (graphics driver, controls, graphics) start afresh.' $true)) {
            Note "left as it is; $($item.Apk) is not installed."
            return
        }
        $data = "/sdcard/Android/data/$($item.Package)"
        # Beside the app's own folder, not in /sdcard itself: Android keeps
        # Android/data apart from the rest of the shared storage (a mount or
        # a quota of its own), so a move out of it is a copy of every game
        # file (6 to 8 GB, minutes), and one within it a rename. Uninstalling
        # takes the package's own folder alone.
        $keep = "/sdcard/Android/data/mw3-recomp-keep-$($item.Package)"
        $moved = "$(Exec $Adb @('-s', $serial, 'shell', "if [ -e $keep ]; then echo busy; elif [ -d $data/files ]; then mv $data/files $keep && echo moved; else echo none; fi"))"
        if ($moved -notmatch 'moved|none') {
            Warn "$keep is on the phone from a replacement that did not finish. Move it back to $data/files (or delete it), then run setup again."
            return
        }
        Exec $Adb @('-s', $serial, 'uninstall', $item.Package) | Out-Null
        $result = & $install
        if ($moved -match 'moved') {
            $back = "$(Exec $Adb @('-s', $serial, 'shell', "mkdir -p $data && mv $keep $data/files && echo back"))"
            if ($back -notmatch 'back') { Warn "the game files and saves could not be moved back: they are in $keep on the phone; move that folder to $data/files." }
            else { Note 'the game files and saves are back in the app''s folder.' }
        }
        if (-not $result.Done) { Warn "could not install $name on ${serial}: $($result.Text)" }
    } catch { Warn "could not replace $($item.Package) on ${serial}: $($_.Exception.Message)" }
}

# The multiplayer's camouflage packs (docs/textures.md, "Camouflage packs"),
# into $dest -- the game folder's `camos`, or the app's assets:
#  - the project's own, drawn by tools/camo_gen.py;
#  - those of setup\camo-packs.json that have an `asset`: packs whose art is
#    their author's own, converted once and kept as assets of a release of
#    the project, checked against the SHA-256 noted there;
#  - the PC mods the player downloaded, in -CamoPacks or setup\camo-packs,
#    converted here by tools/import_pack.py. That is the only way in for a
#    pack the project may not pass on (a port of another game's camouflage).
# None is committed. A failure leaves the list without that pack rather than
# stopping the build.
$CamoPackList = Join-Path $PSScriptRoot 'camo-packs.json'
$CamoPackMods = if ($CamoPacks) { $CamoPacks } else { Join-Path $PSScriptRoot 'camo-packs' }
$PyLibs    = Join-Path $Tc 'pylibs'
$PyImaging = 'pillow==12.3.0', 'numpy==2.4.4'
function Install-CamoPacks($dest) {
    try {
        # No double quotes in the -c program (Find-Python).
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        & $Python -c 'import PIL, numpy' 2>$null | Out-Null
        $have = $LASTEXITCODE -eq 0
        if (-not $have -and -not (Test-Path (Join-Path $PyLibs 'PIL'))) {
            & $Python -m pip --version 2>$null | Out-Null
            if ($LASTEXITCODE -ne 0) { & $Python -m ensurepip --default-pip 2>$null | Out-Null }
        }
        $ErrorActionPreference = $old
        if (-not $have) {
            if (-not (Test-Path (Join-Path $PyLibs 'PIL'))) {
                Step 'Pillow and numpy, for the camouflage tools'
                Exec $Python (@('-m', 'pip', 'install', '--disable-pip-version-check', '--quiet', '--target', $PyLibs) + $PyImaging) -Log 'pip_imaging.log' -Quiet
            }
            $env:PYTHONPATH = $PyLibs
        }
        Step "camouflage packs into $dest"
        Exec $Python @('tools/camo_gen.py', '--pack', (Join-Path $dest 'mw3-recomp-originals')) -Cwd $Repo -Log 'camo_pack.log' -Quiet
    } catch {
        Warn "no camouflage packs: $($_.Exception.Message)"
        return
    }
    $list = Get-Content -Raw $CamoPackList | ConvertFrom-Json
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    foreach ($pack in $list.packs) {
        if (-not $pack.asset) { continue }
        try {
            $archive = Join-Path $Dl "camo-$($pack.asset)"
            if (-not ((Test-Path $archive) -and (Sha256Of $archive) -eq $pack.sha256.ToLowerInvariant())) {
                Step "the camouflage pack $($pack.name) by $($pack.author)"
                Remove-Item -Force -ErrorAction SilentlyContinue "$archive.part"
                $got = $false
                # A release that is not public yet is read with the GitHub CLI's sign-in.
                if ($gh) {
                    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
                    & $gh.Source release download $list.release.tag -R $list.release.repo -p $pack.asset -O "$archive.part" 2>$null | Out-Null
                    $got = $LASTEXITCODE -eq 0
                    $ErrorActionPreference = $old
                }
                if (-not $got) {
                    Exec 'curl.exe' @('-fsSL', '--retry', '3', '-o', "$archive.part",
                                      "https://github.com/$($list.release.repo)/releases/download/$($list.release.tag)/$($pack.asset)")
                }
                if ((Sha256Of "$archive.part") -ne $pack.sha256.ToLowerInvariant()) { throw 'SHA-256 of the download does not match setup\camo-packs.json' }
                Move-Item -Force "$archive.part" $archive
            }
            $folder = Join-Path $dest $pack.id
            if (Test-Path $folder) { Remove-Item -Recurse -Force $folder }
            Expand-Archive -Path $archive -DestinationPath $dest -Force
        } catch {
            Warn "camouflage pack $($pack.id) is left out: $($_.Exception.Message)"
        }
    }
    if (Test-Path $CamoPackMods) {
        try {
            Step "your camouflage mods in $CamoPackMods"
            Exec $Python @('tools/import_pack.py', '--folder', (Resolve-Path $CamoPackMods).Path, '--camos', $dest) -Cwd $Repo -Log 'camo_import.log' -Quiet
        } catch {
            Warn "the camouflage mods in $CamoPackMods were not all imported (see camo_import.log): $($_.Exception.Message)"
        }
    } elseif ($CamoPacks) {
        Warn "-CamoPacks: there is no folder $CamoPacks"
    }
}

# ---- 0. preflight ------------------------------------------------------------
if (-not [Environment]::Is64BitOperatingSystem) { throw 'A 64-bit Windows is needed.' }
if (-not (Test-Path $Tar)) { throw "$Tar is missing (Windows 10 1803 or later has it)." }
if ($Repo -match '\s') { Warn "the repository's path has a space in it ($Repo); the Android NDK build may fail. A path such as C:\mw3 is safest." }
$drive = (Get-Item $Repo).PSDrive
$freeGb = [math]::Floor($drive.Free / 1GB)
$memGb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)

if ($Check) {
    Step 'prerequisites (read-only check; nothing is downloaded or written)'
    Note "repository: $Repo"
    Note "toolchain folder: $Tc$(if (-not (Test-Path $Tc)) { ' (not created yet)' })"
    Note "free space on ${drive}: $freeGb GB (about 40 GB wanted, 60 GB with the Windows game)"
    Note "memory: $memGb GB$(if ($memGb -lt 16) { ' (below 16 GB the build may run out of memory)' })"
    $py = Find-Python
    Note "python: $(if ($py) { $py } else { "missing; Python $($Pins.Python.File) would be downloaded" })"
    $g = Find-Git
    Note "git + openssl: $(if ($g) { "$($g.Git), $($g.OpenSsl)" } else { "missing; $($Pins.Git.File) would be downloaded" })"
    $vc = Find-VsTools
    Note "C++ Build Tools: $(if ($vc) { $vc } elseif (Test-Path $XenonExe) { 'not found, not needed (XenonRecomp is already built)' } else { 'missing; would offer: winget install Microsoft.VisualStudio.BuildTools (C++ workload)' })"
    Note "winget: $(if (Get-Command winget -ErrorAction SilentlyContinue) { 'present' } else { 'missing' })"
    foreach ($item in @(@('JDK 17', "$Jdk\bin\java.exe", 'Jdk'), @('LLVM 18 (clang-cl)', "$Llvm\bin\clang-cl.exe", 'Llvm'),
                        @('Android cmdline-tools', $SdkManager, 'CmdTools'), @('CMake 3.31.8', $CMake, 'CMake'),
                        @('Ninja', $Ninja, 'Ninja'), @('llvm-mingw (Windows game only)', "$MingwDir\bin\x86_64-w64-mingw32-clang++.exe", 'LlvmMingw'))) {
        $state = if (Test-Path $item[1]) { 'present' } else { "would download $($Pins[$item[2]].File)" }
        Note "$($item[0]): $state"
    }
    $missing = @($SdkPackages | Where-Object { -not (Test-Path (Join-Path $Sdk ($_ -replace ';', '\'))) })
    Note "Android SDK packages: $(if ($missing) { "would install $($missing -join ', ')" } else { 'present' })"
    Note "XenonRecomp: $(if (Test-Path $XenonExe) { 'built' } else { "would clone hedge-dev/XenonRecomp at $($XenonCommit.Substring(0, 12)), patch and build it" })"
    foreach ($key in 'sp', 'mp') {
        $t = $Titles[$key]; $xex = Join-Path $Repo "$Disc\$($t.Xex).xex"
        $state = 'not extracted yet'
        if (Test-Path $xex) {
            if ($py) {
                $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
                $verdict = try { Test-Xex $py $t } catch { $_.Exception.Message }
                $ErrorActionPreference = $old
                $state = "extracted; $verdict"
            } else { $state = 'extracted (no Python to check it)' }
        }
        $tree = if (Test-Tree $t) { 'recompiled' }
                elseif (Test-Path (Join-Path $Repo "$($t.Ppc)\ppc_func_mapping.cpp")) { 'recompiled with an older recompiler or configuration, or by an older setup; would be recompiled' }
                else { 'not recompiled' }
        Note "$($t.Xex).xex ($($t.Label)): $state; $tree"
    }
    if ($Iso) { Note "disc image: $(if (Test-Path -LiteralPath $Iso -PathType Leaf) { "$Iso ($([math]::Round((Get-Item -LiteralPath $Iso).Length / 1GB, 2)) GB)" } else { "$Iso NOT FOUND" })" }
    $devices = Get-Devices
    Note "Android devices (adb): $(if (-not (Test-Path $Adb)) { 'adb not installed yet' } elseif ($devices.Count) { ($devices | ForEach-Object { "$($_.Serial) ($($_.State))" }) -join ', ' } else { 'none connected' })"
    return
}

Write-Host "$GameName (Xbox 360) recompiled: setup" -ForegroundColor Green
Write-Host 'This builds the game from your own disc image. What it makes contains code'
Write-Host 'from your copy of the game: keep it to your own devices, never share it.'
Write-Host ''
if ($freeGb -lt 40) { Warn "only $freeGb GB free on ${drive}: about 40 GB is wanted (60 GB with the Windows game)." }
if ($memGb -lt 16) { Warn "$memGb GB of memory: the recompiled code is large, and the build may run out below 16 GB." }

# ---- 1. what to build --------------------------------------------------------
if ($Mw2 -and $Instance -gt 1) { throw '-Instance is for Modern Warfare 3.' }
if (-not $Title) { $Title = Choose 'Which part of the game for Windows: both, sp (campaign and Special Ops) or mp (multiplayer)? The Android app always holds both.' @('both', 'sp', 'mp') 'both' }
if (-not $Build) { $Build = Choose 'Build for android, pc (Windows) or both?' @('android', 'pc', 'both') 'android' }
if ($Instance -gt 1 -and $Title -ne 'mp') { throw '-Instance is for -Title mp' }
$ForAndroid = $Build -in 'android', 'both'
$ForPc = $Build -in 'pc', 'both'
$TestCopy = $Instance -gt 1
# The Android app is published without the game's code, which is built here
# into a package of its own (docs/game-library.md). The multiplayer test copy
# is one build, code and all.
$Split = $ForAndroid -and -not $TestCopy
if ($NoInstall -and ($Install -or $PushIso)) { throw '-NoInstall installs and copies nothing; -Install and -PushIso do. Choose one.' }
if (($GameOnly -or $Dev -or $Suffix) -and -not $Split) { throw '-GameOnly, -Dev and -Suffix are for the Android app.' }
if ($GameOnly -and $Dev) { throw '-GameOnly leaves the app alone; -Dev builds it. Choose one.' }
if ($Suffix -and -not ($GameOnly -or $Dev)) { throw '-Suffix goes with -Dev or -GameOnly: the published app has the one name.' }
$AppPackage = "com.$Game.recomp$Suffix"
# The Windows game is an executable for each title asked for. The Android app
# carries both, so it needs both recompiled whatever -Title says; the
# multiplayer test copy (-Instance) needs only its own.
$PcKeys = if ($Title -eq 'both') { @('sp', 'mp') } else { @($Title) }
$Keys = if ($ForAndroid -and -not $TestCopy) { @('sp', 'mp') } else { $PcKeys }
# With the player's map packs, the multiplayer the title update makes as well.
# (@(): one key alone is a string, and += would append to its text.)
if ($Dlc -and $Aw -and 'mp' -in $Keys) { $Keys = @($Keys) + 'mp17' }
if ($Dlc -and $Aw -and 'mp' -in $PcKeys) { $PcKeys = @($PcKeys) + 'mp17' }

# Each game's Windows game has a folder of its own: the disc's files are copied
# into its game\, and two games' discs there would mix.
$PcFolder = Join-Path $Out $(if ($Cod4) { 'pc-cod4' } elseif ($Aw) { 'pc-aw' } elseif ($Mw2) { 'pc-mw2' } else { 'pc' })
$needIso = ($ForPc -and -not (Test-Path (Join-Path $PcFolder 'game\default.xex'))) -or
           @($Keys | Where-Object { -not $Titles[$_].Folder -and -not (Test-Path (Join-Path $Repo "$Disc\$($Titles[$_].Xex).xex")) }).Count -gt 0 -or $PushIso
while ($needIso -and -not ($Iso -and (Test-Path -LiteralPath $Iso -PathType Leaf))) {
    if ($Iso) { Warn "no file at $Iso" }
    if (-not $Interactive) { throw 'Give the disc image with -Iso <path>.' }
    $Iso = (Read-Host "Path to your $GameName disc image (.iso; you can drag the file into this window)").Trim().Trim('"')
}
if ($Iso) { $Iso = (Resolve-Path -LiteralPath $Iso).Path }
# Advanced Warfare's install disc holds two content packages, which a console
# copies to its hard disk before the game disc will start; here they are read
# out of the image once, into the git-ignored aw\install, and mounted from
# there (docs/AW-BRINGUP.md).
$InstallFolder = Join-Path $Repo 'aw\install'
$InstallPackages = @('41560914051BF3C0', '4156091407406280')
$needInstallIso = $Aw -and -not $GameOnly -and
                  @($InstallPackages | Where-Object { -not (Get-ChildItem -Recurse -File -Filter $_ $InstallFolder -ErrorAction SilentlyContinue) }).Count -gt 0
while ($needInstallIso -and -not ($InstallIso -and (Test-Path -LiteralPath $InstallIso -PathType Leaf))) {
    if ($InstallIso) { Warn "no file at $InstallIso" }
    if (-not $Interactive) { throw 'Give the install disc''s image with -InstallIso <path>.' }
    $InstallIso = (Read-Host "Path to the image of $GameName's install disc (.iso)").Trim().Trim('"')
}
if ($InstallIso) { $InstallIso = (Resolve-Path -LiteralPath $InstallIso).Path }
# A player's own downloadable content (docs/dlc.md): the packages are read
# where they lie. $DlcRoot is the folder that holds them all, $DlcFiles the
# packages (the install disc's two among them are skipped: aw\install has
# them), $DlcUpdate the title update's.
$DlcRoot = $null; $DlcFiles = @(); $DlcUpdate = $null
if ($Dlc) {
    if ($TestCopy) { throw '-Dlc is not for -Instance.' }
    if (-not (Test-Path -LiteralPath $Dlc -PathType Container)) { throw "-Dlc: no folder at $Dlc" }
    $Dlc = (Resolve-Path -LiteralPath $Dlc).Path
}

New-Item -ItemType Directory -Force $Tc, $Dl, $Out, $Logs | Out-Null

# ---- 2. tools ----------------------------------------------------------------
Step 'tools'
$GitTools = Find-Git
if (-not $GitTools) {
    $sfx = Fetch $Pins.Git
    Step 'unpack portable Git'
    if (Test-Path $GitDir) { Remove-Item -Recurse -Force $GitDir }
    # A self-extracting 7-Zip archive, a windowed program: waited for explicitly.
    $unpack = Start-Process -FilePath $sfx -ArgumentList @('-y', "-o`"$GitDir`"") -Wait -PassThru
    if ($unpack.ExitCode -ne 0) { throw "portable Git did not unpack (exit code $($unpack.ExitCode))" }
    $GitTools = Find-Git
    if (-not $GitTools) { throw "portable Git did not unpack into $GitDir" }
}
$env:PATH = "$(Split-Path -Parent $GitTools.Git);$env:PATH"
$OpenSslDir = Split-Path -Parent $GitTools.OpenSsl

$Python = Find-Python
if (-not $Python) {
    $pkg = Fetch $Pins.Python
    Step 'unpack Python'
    if (Test-Path $PyDir) { Remove-Item -Recurse -Force $PyDir }
    New-Item -ItemType Directory -Force $PyDir | Out-Null
    Exec $Tar @('-xf', $pkg, '-C', $PyDir, 'tools')
    $Python = Join-Path $PyDir 'tools\python.exe'
}
Note "python: $Python"
$env:PYTHONUTF8 = '1'

if (-not (Test-Path $CMake)) { Unpack (Fetch $Pins.CMake) $CMakeDir }
if (-not (Test-Path $Ninja)) {
    $zip = Fetch $Pins.Ninja
    New-Item -ItemType Directory -Force $NinjaDir | Out-Null
    Exec $Tar @('-xf', $zip, '-C', $NinjaDir)
}

if ($ForAndroid) {
    if (-not (Test-Path "$Jdk\bin\java.exe")) { Step 'unpack JDK 17'; Unpack (Fetch $Pins.Jdk) $Jdk }
    if (-not (Test-Path $SdkManager)) {
        $zip = Fetch $Pins.CmdTools
        Step 'unpack Android cmdline-tools'
        $tools = Join-Path $Sdk 'cmdline-tools'
        New-Item -ItemType Directory -Force $tools | Out-Null
        if (Test-Path "$tools\latest") { Remove-Item -Recurse -Force "$tools\latest" }
        Exec $Tar @('-xf', $zip, '-C', $tools)
        Move-Item "$tools\cmdline-tools" "$tools\latest"
    }
    # Process-local environment only.
    $env:JAVA_HOME = $Jdk
    $env:GRADLE_USER_HOME = Join-Path $Tc 'gradle-home'
    $env:ANDROID_USER_HOME = Join-Path $Tc 'android-user-home'
    $missing = @($SdkPackages | Where-Object { -not (Test-Path (Join-Path $Sdk ($_ -replace ';', '\'))) })
    if ($missing) {
        if (-not (Ask 'The Android SDK and NDK come under Google''s licence terms (https://developer.android.com/studio/terms). Accept them?' $true)) {
            throw 'The Android build needs the Android SDK; stopped.'
        }
        Step "sdkmanager: $($missing -join ', ')"
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        (1..40 | ForEach-Object { 'y' }) | & $SdkManager "--sdk_root=$Sdk" --licenses 2>&1 | Out-Null
        $ErrorActionPreference = $old
        Exec $SdkManager (@("--sdk_root=$Sdk") + $missing) -Log 'sdkmanager.log' -Quiet
    }
}

# ---- 3. the executables, out of the disc image -------------------------------
if ($needInstallIso) {
    Step 'copy the two content packages from the install disc''s image (read-only; 4 GB)'
    Exec $Python @('tools/xdvdfs.py', $InstallIso, $InstallFolder, 'content/0000000000000000/41560914/00000002/') -Cwd $Repo -Log 'extract_install_packages.log' -Quiet
    $missing = @($InstallPackages | Where-Object { -not (Get-ChildItem -Recurse -File -Filter $_ $InstallFolder -ErrorAction SilentlyContinue) })
    if ($missing.Count) { throw "that image has no package $($missing -join ', '): is it $GameName's install disc?" }
}
foreach ($key in @($Keys)) {
    $t = $Titles[$key]
    # One an update makes is made in step 4, with the recompiler's patcher.
    if ($t.Folder) { continue }
    $xexRel = "$Disc/$($t.Xex).xex"
    if (-not (Test-Path (Join-Path $Repo $xexRel))) {
        Step "extract $($t.Xex).xex from the disc image (read-only)"
        Exec $Python @('tools/xdvdfs.py', $Iso, $Disc, "$($t.Xex).xex") -Cwd $Repo -Log "extract_$($t.Xex).log" -Quiet
        if (-not (Test-Path (Join-Path $Repo $xexRel))) { throw "$($t.Xex).xex is not on that disc image: is it $GameName for the Xbox 360?" }
    }
    try { Test-Xex $Python $t }
    catch {
        if ($Keys.Count -gt 1) {
            Warn "$($t.Xex).xex is not a build this port supports; skipping the $($t.Label)."
            $Keys = @($Keys | Where-Object { $_ -ne $key })
        } else { throw "$($t.Xex).xex is not a disc build this port supports: $($_.Exception.Message)" }
    }
}
if ('mp' -notin $Keys) { $Keys = @($Keys | Where-Object { $_ -ne 'mp17' }) }
$PcKeys = @($PcKeys | Where-Object { $_ -in $Keys })
if ($Dlc) {
    Step 'look at the downloadable content (read-only)'
    $dlcTitle = if ($Aw) { '41560914' } elseif ($Cod4) { '415607E6' } elseif ($Mw2) { '41560817' } else { '415608CB' }
    foreach ($line in @(Exec $Python @('tools/aw_dlc.py', $Dlc, '--title', $dlcTitle) -Cwd $Repo)) {
        if ("$line" -match '^root (.+)$') { $DlcRoot = $Matches[1] }
        elseif ("$line" -match '^update (.+)$') { if (-not $DlcUpdate) { $DlcUpdate = $Matches[1] } }
        elseif ("$line" -match '^pack ([^\t]+)\t(.*)$') {
            if ((Split-Path -Leaf $Matches[1]) -notin $InstallPackages) { $DlcFiles += $Matches[1]; Note "$($Matches[2])" }
        }
        # A package that is damaged, stopped part-way or another game's: the
        # game would call it a corrupt content pack, so it is left out and said.
        elseif ("$line" -match '^bad ([^\t]+)\t(.*)$') { Warn "$($Matches[2]). It is not copied or used ($($Matches[1]))." }
    }
    if ($Aw) {
        if (-not $DlcUpdate) {
            throw "No title update is in $Dlc. The map packs are for the updated game: copy Content\0000000000000000\41560914\000B0000 off the console as well (docs\dlc.md)."
        }
        Note "title update: $(Split-Path -Leaf $DlcUpdate)"
        $DlcFiles += $DlcUpdate
    } else {
        # The other three games play the disc's executables, which read the
        # packs themselves; a title update's files are for the executable
        # it makes.
        if (-not $DlcFiles.Count) { throw "No map pack of $GameName is in ${Dlc}: its packages are in Content\0000000000000000\$dlcTitle\00000002 on the console's hard disk (docs\dlc.md)." }
        if ($DlcUpdate) { Note "title update $(Split-Path -Leaf $DlcUpdate): not used, the game is played as the disc has it" }
    }
}
if ($ForAndroid -and -not $TestCopy -and $Keys.Count -lt 2) {
    throw 'The Android app holds the campaign and the multiplayer, and needs both executables to be disc builds this port supports.'
}

# ---- 4. recompile ------------------------------------------------------------
$toRecompile = @($Keys | Where-Object { $Recomp -or -not (Test-Tree $Titles[$_]) })
# The recompiler itself, when this checkout names another commit of it or
# patches it differently than the one built here: made again from nothing.
if ($toRecompile.Count -and (Test-Path (Join-Path $Xenon 'CMakeLists.txt'))) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $head = "$(& $GitTools.Git -C $Xenon rev-parse HEAD 2>$null)".Trim()
    & $GitTools.Git -C $Xenon apply --reverse --check (Join-Path $Repo 'patches\xenonrecomp-mw2.patch') 2>$null
    $patched = $LASTEXITCODE -eq 0
    $ErrorActionPreference = $old
    if ($head -ne $XenonCommit -or -not $patched) {
        if ((Get-Item $Xenon).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "XenonRecomp\ is a link to a checkout that is not this repository's recompiler ($XenonCommit with patches\xenonrecomp-mw2.patch); update what it points at."
        }
        Step 'the recompiler changed with this update: fetching and building it again'
        Remove-Item -Recurse -Force $Xenon
    }
}
if ($toRecompile.Count -and -not ((Test-Path $XenonExe) -and (Test-Path $AnalyseExe))) {
    $vcvars = Find-VsTools
    if (-not $vcvars) {
        Warn 'the recompiler is built with Microsoft''s C++ Build Tools (its libraries and the Windows SDK), which are not installed.'
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if ($winget -and (Ask 'Install Visual Studio Build Tools with the C++ workload now (about 3 GB, asks for administrator rights)?' $true)) {
            Exec $winget.Source @('install', '--id', 'Microsoft.VisualStudio.BuildTools', '-e', '--accept-package-agreements', '--accept-source-agreements',
                                  '--override', '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
            $vcvars = Find-VsTools
        }
        if (-not $vcvars) {
            throw 'Install "Build Tools for Visual Studio" with "Desktop development with C++" (https://visualstudio.microsoft.com/downloads/), then run setup again.'
        }
    }
    if (-not (Test-Path "$Llvm\bin\clang-cl.exe")) {
        $txz = Fetch $Pins.Llvm
        Step 'unpack LLVM 18.1.8 (compiler, linker and archiver only)'
        New-Item -ItemType Directory -Force $Llvm | Out-Null
        $members = 'clang.exe', 'clang++.exe', 'clang-cl.exe', 'lld-link.exe', 'llvm-ar.exe', 'llvm-lib.exe',
                   'llvm-rc.exe', 'llvm-mt.exe', 'llvm-ranlib.exe' | ForEach-Object { "*/bin/$_" }
        Exec $Tar (@('-xf', $txz, '-C', $Llvm, '--strip-components=1') + $members + '*/lib/clang')
    }
    if (-not (Test-Path (Join-Path $Xenon 'CMakeLists.txt'))) {
        Step 'clone XenonRecomp'
        if (Test-Path $Xenon) { Remove-Item -Recurse -Force $Xenon }
        # autocrlf off: the patch is LF and must apply to LF files.
        Exec $GitTools.Git @('clone', '-q', '-c', 'core.autocrlf=false', '-c', 'core.longpaths=true',
                             'https://github.com/hedge-dev/XenonRecomp.git', $Xenon)
        Exec $GitTools.Git @('-C', $Xenon, 'checkout', '-q', $XenonCommit)
        Exec $GitTools.Git @('-C', $Xenon, '-c', 'core.longpaths=true', 'submodule', 'update', '-q', '--init', '--recursive')
        Exec $GitTools.Git @('-C', $Xenon, 'apply', (Join-Path $Repo 'patches\xenonrecomp-mw2.patch'))
    }
    Step 'build XenonRecomp (clang-cl + MSVC libraries)'
    Use-VsTools $vcvars
    $fwd = { param($p) $p -replace '\\', '/' }
    Exec $CMake @('-S', $Xenon, '-B', "$Xenon\build", '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                  "-DCMAKE_MAKE_PROGRAM=$(& $fwd $Ninja)",
                  # A newer MSVC STL insists on a newer Clang; 18 (what the pipeline pins) compiles it fine.
                  '-DCMAKE_CXX_FLAGS=/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /DWIN32 /D_WINDOWS /EHsc',
                  "-DCMAKE_C_COMPILER=$(& $fwd "$Llvm\bin\clang-cl.exe")",
                  "-DCMAKE_CXX_COMPILER=$(& $fwd "$Llvm\bin\clang-cl.exe")",
                  "-DCMAKE_LINKER=$(& $fwd "$Llvm\bin\lld-link.exe")",
                  "-DCMAKE_AR=$(& $fwd "$Llvm\bin\llvm-lib.exe")",
                  "-DCMAKE_RC_COMPILER=$(& $fwd "$Llvm\bin\llvm-rc.exe")",
                  "-DCMAKE_MT=$(& $fwd "$Llvm\bin\llvm-mt.exe")") -Log 'xenon_configure.log' -Quiet
    Exec $CMake @('--build', "$Xenon\build", '--target', 'XenonAnalyse', 'XenonRecomp') -Log 'xenon_build.log' -Quiet
}

# The executable the title update makes of the disc's multiplayer: the
# update's own patch applied to it by XenonRecomp's patcher (tools/xexpatch.cpp),
# as a console does when it starts an updated game. Kept in the git-ignored
# aw\tu17, for the recompiler; the game itself makes it again at every start.
if ('mp17' -in $Keys) {
    $t = $Titles['mp17']
    $updated = Join-Path $Repo (XexOf $t)
    if (-not (Test-Path $updated)) {
        Step 'apply the title update''s patch to default_mp.xex'
        New-Item -ItemType Directory -Force (Split-Path -Parent $updated) | Out-Null
        $patcher = Join-Path $Xenon 'build\xexpatch.exe'
        if (-not (Test-Path $patcher)) {
            $libraries = @((Join-Path $Xenon 'build\XenonUtils\XenonUtils.lib'), (Join-Path $Xenon 'build\thirdparty\disasm\disasm.lib'))
            $vcvars = Find-VsTools
            if (-not $vcvars -or @($libraries | Where-Object { -not (Test-Path $_) }).Count -or -not (Test-Path "$Llvm\bin\clang-cl.exe")) {
                throw 'The patcher is built with the recompiler''s libraries, which are not here: run setup once with -Recomp, then again with -Dlc.'
            }
            Use-VsTools $vcvars
            Exec "$Llvm\bin\clang-cl.exe" (@('/nologo', '/std:c++20', '/O1', '/EHsc', '/MT', '/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH',
                                              "/I$Xenon\XenonUtils", "/I$Xenon\thirdparty\simde", (Join-Path $Repo 'tools\xexpatch.cpp')) + $libraries +
                                            @("/Fe$patcher", "/Fo$Xenon\build\")) -Log 'xexpatch_build.log' -Quiet
        }
        $patch = Join-Path (Split-Path -Parent $updated) 'default_mp.xexp'
        Exec $Python @('tools/aw_dlc.py', $Dlc, '--patch', $patch) -Cwd $Repo -Log 'extract_update_patch.log' -Quiet
        Exec $patcher @((Join-Path $Repo "$Disc\default_mp.xex"), $patch, "$updated.part") -Log 'xexpatch.log' -Quiet
        Move-Item -Force "$updated.part" $updated
    }
    try { Test-Xex $Python $t }
    catch {
        Remove-Item -Force $updated
        throw "That title update does not make the executable this port is for (title update 17's): $($_.Exception.Message)"
    }
}

foreach ($key in @($Keys)) {
    $t = $Titles[$key]
    $xexRel = XexOf $t
    $ppcDir = Join-Path $Repo $t.Ppc
    $env:MW2_TITLE = $t.Name; $env:MW2_VERSION = 'tu0'
    if ($key -in $toRecompile) {
        Step "recompile $($t.Xex).xex ($($t.Label)); this takes a while"
        # tools/xexdump.py decrypts with openssl.exe, found on PATH.
        $savedPath = $env:PATH; $env:PATH = "$OpenSslDir;$env:PATH"
        try { Exec $Python @('tools/xexdump.py', $xexRel, ($xexRel -replace '\.xex$', '.pe')) -Cwd $Repo -Log "$($t.Name)_xexdump.log" -Quiet }
        finally { $env:PATH = $savedPath }
        Exec $AnalyseExe @($xexRel, "config/$($t.Tables)") -Cwd $Repo -Log "$($t.Name)_analyse.log" -Quiet
        # Call of Duty 4 has a kind of jump table the analyser does not
        # recognise. Its own alone: the other games' tables are part of what
        # their game code is checked against, and stay as the analyser writes them.
        if ($Cod4 -or $Aw) { Exec $Python @('tools/intext_switch_tables.py') -Cwd $Repo -Log "$($t.Name)_intext_tables.log" -Quiet }
        # XenonRecomp crashes, rather than complaining, when its output directory is missing.
        # The tree of the run before is kept beside it until this one is
        # whole, to tell which files come out as they were; a tree without
        # its note is a run that stopped halfway, and the one kept is older.
        $before = "$ppcDir.before"
        if (Test-Path (Join-Path $ppcDir 'recompiled_with.txt')) {
            if (Test-Path $before) { Remove-Item -Recurse -Force $before }
            Move-Item $ppcDir $before
        } elseif (Test-Path $ppcDir) { Remove-Item -Recurse -Force $ppcDir }
        New-Item -ItemType Directory $ppcDir | Out-Null
        Exec $XenonExe @($t.Toml, '../XenonRecomp/XenonUtils/ppc_context.h') -Cwd (Join-Path $Repo 'config') -Log "$($t.Name)_recomp.log" -Quiet
        $recompLog = Join-Path $Logs "$($t.Name)_recomp.log"
        $count = { param($file, $pattern) @(Select-String -Path $file -SimpleMatch $pattern).Count }
        $files = @(Get-ChildItem $ppcDir -File).Count
        $tables = & $count (Join-Path $Repo "config\$($t.Tables)") '[[switch]]'
        $bad = (& $count $recompLog 'Unrecognized instruction') + (& $count $recompLog 'trying to jump outside function')
        $stepped = [int]$t.Stepped
        Note "files: $files (expect $($t.Files))   jump tables: $tables (expect $($t.Jump))   recompiler errors: $bad (expect $stepped)"
        if ($files -ne $t.Files -or $tables -ne $t.Jump -or $bad -ne $stepped) { Warn 'the recompiled tree differs from the tested one; the build may fail or misbehave.' }
        Step 'generate the shared header'
        Exec $Python @('tools/genshared.py') -Cwd $Repo -Log "$($t.Name)_genshared.log" -Quiet
        # A file that came out as it was keeps its date, so the build
        # compiles only the ones that changed: a new recompiler or
        # configuration seldom changes them all, and often none. The shared
        # header lists every function, one `PPC_FUNC(name);` a line, and
        # every file includes it; it keeps its date when only that list
        # changed, since a file compiles the same whichever other functions
        # are declared, and the files that name a new one changed themselves.
        if (Test-Path $before) {
            $made = @(Get-ChildItem $ppcDir -File)
            $same = 0
            $apart = { param($path) ([IO.File]::ReadAllLines($path) | Where-Object { $_ -notmatch '^PPC_FUNC\(\w+\);$' }) -join "`n" }
            foreach ($file in $made) {
                $old = Join-Path $before $file.Name
                if (-not (Test-Path $old)) { continue }
                if ((Get-Item $old).Length -eq $file.Length -and (Sha256Of $old) -eq (Sha256Of $file.FullName)) {
                    $file.LastWriteTimeUtc = (Get-Item $old).LastWriteTimeUtc
                    $same++
                } elseif ($file.Name -eq 'ppc_recomp_shared.h' -and (& $apart $old) -eq (& $apart $file.FullName)) {
                    $file.LastWriteTimeUtc = (Get-Item $old).LastWriteTimeUtc
                }
            }
            Remove-Item -Recurse -Force $before
            Note "$same of $($made.Count) files came out as they were and are not compiled again."
        }
        # What the tree was made with, for the next run to know whether it
        # still is what this checkout recompiles to (Test-Tree).
        [IO.File]::WriteAllText((Join-Path $ppcDir 'recompiled_with.txt'), (RecompiledWith $t))
    }
    # Every run: the runtime implements more imports as it is updated (git
    # pull), and a stub left for one it now has would clash with it at the link.
    Exec $Python @('tools/gen_kernel_stubs.py') -Cwd $Repo -Log "$($t.Name)_stubs.log" -Quiet
}

# ---- 5. the Android apps -----------------------------------------------------
# What is installed in step 7, in order: @{ Apk; Package; Data }. Data says
# the package has a game folder and saves of its own on the phone.
$Apks = @()
if ($ForAndroid) {
    [IO.File]::WriteAllText((Join-Path $Repo 'android\local.properties'), "sdk.dir=$($Sdk -replace '\\', '/')`n")
    $env:MW2_LTO = 'OFF'
    # A modest daemon: most of the memory goes to the compilers it starts.
    $gradleArgs = @('--no-daemon', '-Dorg.gradle.jvmargs=-Xmx1536m -XX:MaxMetaspaceSize=512m -Dfile.encoding=UTF-8')
    if ($TestCopy) { $gradleArgs += "-PmpInstance=$Instance" }
    if ($Suffix) { $gradleArgs += "-PappSuffix=$Suffix" }
    $fwd = { param($p) $p -replace '\\', '/' }
    # Whether the app is built here: always, but for the app with the whole
    # game, which is the published one unless -Dev asks.
    $BuildApp = -not $Split -or $Dev

    if ($Split) {
        # ---- the game code: a library for each title, in a package ----------
        # With the NDK's own CMake and Ninja, as Gradle builds the app's.
        $ndkCMake = Join-Path $Sdk 'cmake\3.22.1\bin\cmake.exe'
        $ndkNinja = Join-Path $Sdk 'cmake\3.22.1\bin\ninja.exe'
        # A folder for each game: the package takes every library in the
        # folder it is given, so one shared by both games put all four in
        # each package once both had been built here.
        $libraries = Join-Path $Out "game-libraries\$Game\arm64-v8a"
        New-Item -ItemType Directory -Force $libraries | Out-Null
        $built = @()
        foreach ($key in @($Keys)) {
            $t = $Titles[$key]
            $buildDir = Join-Path $Out "build-game-$($t.Lib)"
            Step "build the game code: $($t.Label) (log: out\logs\game_$($t.Lib).log)"
            # Modern Warfare 3's titles configure only with these two.
            $titleArgs = if ($Mw2) { @() } else { @('-DMW3_RECOMP_ONLY=ON', '-DMW3_LINK_PROBE=ON') }
            Exec $ndkCMake (@('-S', $Repo, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$(& $fwd $ndkNinja)",
                              "-DCMAKE_TOOLCHAIN_FILE=$(& $fwd (Join-Path $Sdk 'ndk\28.2.13676358\build\cmake\android.toolchain.cmake'))",
                              '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-26', '-DANDROID_STL=c++_static',
                              '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS=-O3', "-DMW2_TITLE=$($t.Name)",
                              '-DMW2_SPLIT=game', '-DMW2_LTO=OFF') + $titleArgs) -Log "game_$($t.Lib)_configure.log" -Quiet
            Exec $ndkCMake @('--build', $buildDir, '--target', 'ppc_recomp') -Log "game_$($t.Lib).log" -Quiet
            Copy-Item (Join-Path $buildDir "lib$($t.Lib)game.so") $libraries -Force
            $built += [IO.File]::ReadAllText((Join-Path $buildDir 'game_abi.txt')).Trim()
        }
        Step 'package the game code (gradle :game:assembleDebug)'
        Exec (Join-Path $Repo 'android\gradlew.bat') ($gradleArgs + "-Pgame=$Game" + "-PgameLibraries=$(Split-Path -Parent $libraries)" + ':game:assembleDebug') -Cwd (Join-Path $Repo 'android') -Log 'gradle_game.log' -Quiet
        $apk = Join-Path $Out "$Game-game$Suffix.apk"
        Copy-Item (Join-Path $Repo 'android\game\build\outputs\apk\debug\game-debug.apk') $apk -Force
        $Apks += @{ Apk = $apk; Package = "$AppPackage.game"; Data = $false; Libraries = $libraries }
        Note ("{0} ({1:N1} MB; built against {2})" -f $apk, ((Get-Item $apk).Length / 1MB), ($built -join ', '))

        # ---- the app: the published one --------------------------------------
        if (-not $GameOnly -and -not $BuildApp) {
            Step "the published app (github.com/$ReleaseRepo, latest release)"
            $release = $null
            try { $release = (Exec 'curl.exe' @('-fsSL', '--retry', '3', "https://api.github.com/repos/$ReleaseRepo/releases/latest")) -join "`n" | ConvertFrom-Json } catch { }
            $asset = if ($release) { @($release.assets | Where-Object { $_.name -like "$Game-recomp-*.apk" })[0] }
            if ($asset) {
                $apk = Join-Path $Out $asset.name
                if (-not ((Test-Path $apk) -and (Get-Item $apk).Length -eq $asset.size)) {
                    Exec 'curl.exe' @('-fsSL', '--retry', '3', '-o', "$apk.part", $asset.browser_download_url)
                    Move-Item -Force "$apk.part" $apk
                }
                $Apks += @{ Apk = $apk; Package = $AppPackage; Data = $true }
                Note "$apk ($($release.tag_name))"
                # What that app was built against, published beside it: a
                # game library built from another state of this folder is
                # one the app will refuse, and better said here than there.
                $abi = @($release.assets | Where-Object { $_.name -like "$Game-recomp-*-game-abi.txt" })[0]
                if ($abi) {
                    $wanted = @((Exec 'curl.exe' @('-fsSL', '--retry', '3', $abi.browser_download_url)) | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
                    if (@($built | Where-Object { $_ -notin $wanted }).Count) {
                        Warn "the game code just built is not what the published app $($release.tag_name) was built for (this folder is newer or older than that release). The app will say so and not start the game. Either wait for the next release, or build the game code of that one: git checkout $($release.tag_name), then setup\setup.cmd$(if ($Game -ne 'mw3') { " -Game $Game" }) -GameOnly."
                    }
                }
            } else {
                Warn 'no published app could be downloaded (no release yet, or no network): building the app here instead. It is signed with this PC''s key, so it will not update itself; run setup again once there is a release.'
                $BuildApp = $true
            }
        }
    }

    if ($BuildApp) {
        $adreno = Join-Path $Repo 'third_party\libadrenotools'
        if (-not (Test-Path "$adreno\CMakeLists.txt")) {
            Step 'clone libadrenotools'
            # android/fetch_deps.sh names a revision the remote does not serve and
            # falls back to the default branch's head; this is that fallback.
            if (Test-Path $adreno) { Remove-Item -Recurse -Force $adreno }
            Exec $GitTools.Git @('clone', '-q', '--depth', '1', '-c', 'core.autocrlf=false', $AdrenoRemote, $adreno)
            Exec $GitTools.Git @('-C', $adreno, 'submodule', 'update', '-q', '--init', '--recursive', '--depth', '1')
        }
        # The Turnip driver the apps bundle (THIRD_PARTY.md): downloaded,
        # hash-checked, never committed. A failure leaves the APK without it
        # rather than stopping the build, and says so to Gradle, which
        # refuses to build an app without the driver by accident
        # (android/app/build.gradle.kts, bundledDriverCheck).
        $turnip = Join-Path $Repo 'android\app\src\mw3\assets\drivers\turnip_v27_r2.zip'
        if (-not ((Test-Path $turnip) -and (Sha256Of $turnip) -eq $TurnipSha)) {
            Step 'bundled Turnip driver'
            try {
                New-Item -ItemType Directory -Force (Split-Path -Parent $turnip) | Out-Null
                Exec 'curl.exe' @('-fsSL', '--retry', '3', '-o', "$turnip.part", $TurnipUrl)
                if ((Sha256Of "$turnip.part") -ne $TurnipSha) { throw 'SHA-256 of the Turnip package does not match' }
                Move-Item -Force "$turnip.part" $turnip
            } catch {
                Warn "no bundled Turnip driver: $($_.Exception.Message). The app is built to run on the phone's own graphics driver; on an Adreno that is slower and can show a broken picture. Run setup again with a network to bundle it."
                $gradleArgs += '-PwithoutBundledDriver=true'
                Remove-Item -Force -ErrorAction SilentlyContinue $turnip, "$turnip.part"
            }
        }
        # Modern Warfare 3's app carries the multiplayer's camouflage packs as
        # assets and copies them into the game folder when it starts
        # (CamoPacks.kt), and the list of known packs, by which it credits a
        # mod the player adds and fetches the ones the project hosts.
        if ($Game -eq 'mw3' -and @($Keys) -contains 'mp') {
            $assets = Join-Path $Repo 'android\app\src\mw3\assets'
            Install-CamoPacks (Join-Path $assets 'camos')
            Copy-Item -Force $CamoPackList (Join-Path $assets 'camo-packs.json')
        }
        # One app with both titles: the campaign's build makes the
        # multiplayer's library as well (cmake/android.cmake, MW2_BOTH_TITLES).
        $apps = if ($TestCopy) {
            @(@{ Label = "multiplayer test copy $Instance"; Task = 'assembleMw3mpTu0Debug'; Variant = 'mw3mpTu0'; Apk = "mw3mp$Instance-debug.apk"; Package = "com.mw3.recomp.mp$Instance" })
        } elseif ($Aw) {
            @(@{ Label = 'app, without the game code'; Task = 'assembleAwTu0Debug'; Variant = 'awTu0'; Apk = "aw$Suffix-debug.apk"; Package = $AppPackage })
        } elseif ($Cod4) {
            @(@{ Label = 'app, without the game code'; Task = 'assembleCod4Tu0Debug'; Variant = 'cod4Tu0'; Apk = "cod4$Suffix-debug.apk"; Package = $AppPackage })
        } elseif ($Mw2) {
            @(@{ Label = 'app, without the game code'; Task = 'assembleMw2Tu0Debug'; Variant = 'mw2Tu0'; Apk = "mw2$Suffix-debug.apk"; Package = $AppPackage })
        } else {
            @(@{ Label = 'app, without the game code'; Task = 'assembleMw3Tu0Debug'; Variant = 'mw3Tu0'; Apk = "mw3$Suffix-debug.apk"; Package = $AppPackage })
        }
        foreach ($app in $apps) {
            Step "build the $($app.Label) (gradle $($app.Task); log: out\logs\gradle_$($app.Variant).log)"
            Exec (Join-Path $Repo 'android\gradlew.bat') ($gradleArgs + ":app:$($app.Task)") -Cwd (Join-Path $Repo 'android') -Log "gradle_$($app.Variant).log" -Quiet
            $made = Get-ChildItem (Join-Path $Repo "android\app\build\outputs\apk\$($app.Variant)\debug\*.apk") | Sort-Object LastWriteTime | Select-Object -Last 1
            if (-not $made) { throw "gradle finished but no APK under android\app\build\outputs\apk\$($app.Variant)\debug" }
            $apk = Join-Path $Out $app.Apk
            Copy-Item $made.FullName $apk -Force
            $Apks += @{ Apk = $apk; Package = $app.Package; Data = $true }
            Note ("{0} ({1:N1} MB)" -f $apk, ((Get-Item $apk).Length / 1MB))
        }
    }
}

# ---- 6. the Windows game -----------------------------------------------------
if ($ForPc) {
    if (-not (Test-Path "$MingwDir\bin\x86_64-w64-mingw32-clang++.exe")) { Step 'unpack llvm-mingw'; Unpack (Fetch $Pins.LlvmMingw) $MingwDir }
    $pc = $PcFolder
    # Not $game: PowerShell names are case-insensitive, so it would be the
    # -Game parameter and fail its ValidateSet.
    $pcGame = Join-Path $pc 'game'
    New-Item -ItemType Directory -Force $pcGame | Out-Null
    # The whole disc, about 7.4 GB: the multiplayer picks its own maps, and a
    # missing zone reads as a dirty disc. Files already there are skipped.
    if ($Iso) {
        Step 'copy the game files from the disc image (read-only; about 7.4 GB)'
        Exec $Python @('tools/xdvdfs.py', $Iso, $pcGame) -Cwd $Repo -Log 'extract_pc_game.log' -Quiet
    }
    $fwd = { param($p) $p -replace '\\', '/' }
    foreach ($key in @($PcKeys)) {
        $t = $Titles[$key]
        $buildDir = Join-Path $Out "build-pc-$($t.Name)"
        Step "build the Windows $($t.Label) (log: out\logs\pc_$($t.Name).log)"
        $env:MW2_TITLE = $t.Name; $env:MW2_VERSION = 'tu0'
        # The version and the title's kind are options of the configure, which
        # does not read them from the environment: Modern Warfare 2 is the
        # disc's executable (tu0, where the desktop default is tu6) with its
        # own runtime; the other games configure as Modern Warfare 3 does.
        $titleArgs = if ($Mw2) { @('-DMW2_VERSION=tu0') } else { @('-DMW3_RECOMP_ONLY=ON', '-DMW3_LINK_PROBE=ON') }
        # A diagnostics build, as tested: its log is what a bug report needs.
        Exec $CMake (@('-S', $Repo, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$(& $fwd $Ninja)",
                       '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_TOOLCHAIN_FILE=$(& $fwd (Join-Path $Repo 'cmake\mingw-w64.cmake'))",
                       "-DLLVM_MINGW=$(& $fwd $MingwDir)", "-DMW2_TITLE=$($t.Name)",
                       '-DMW2_DIAGNOSTICS=ON', '-DMW2_PORTABLE=OFF', '-DMW2_LTO=OFF') + $titleArgs) -Log "pc_$($t.Name)_configure.log" -Quiet
        Exec $CMake @('--build', $buildDir, '--target', 'mw2') -Log "pc_$($t.Name).log" -Quiet
        # Modern Warfare 2's executable is mw2.exe for either title (CMakeLists.txt,
        # MW2_LIBRARY_NAME); beside each other they are named as their libraries.
        $built = if ($Mw2) { 'mw2.exe' } else { "$($t.Name).exe" }
        $exe = "$($t.Lib).exe"
        Copy-Item (Join-Path $buildDir $built) (Join-Path $pc $exe) -Force
        $launcher = if ($key -eq 'mp17') { 'Multiplayer with map packs.cmd' } elseif ($key -eq 'mp') { 'Multiplayer.cmd' } else { 'Campaign.cmd' }
        $lines = '@echo off', "rem Settings are in .env beside this file (docs/switches.md); the log is $Game.log.",
                 'cd /d "%~dp0"', "$exe game\$($t.Xex).xex game"
        [IO.File]::WriteAllText((Join-Path $pc $launcher), (($lines -join "`r`n") + "`r`n"))
        if ($key -eq 'mp17') {
            # Exo Zombies is the same executable in another mode, which the
            # updated campaign's menu names in the launch data: 0x3F8 bytes,
            # the first word 11 and the mode, 5, at 0x3D8 (docs/AW-BRINGUP.md).
            $data = '0000000b' + ('00' * 0x3D4) + '00000005' + ('00' * 0x1C)
            $lines = '@echo off', 'rem Exo Zombies: the multiplayer with the title update, started in its zombies mode.',
                     'cd /d "%~dp0"', "set MW2_LAUNCH_DATA=$data", "$exe game\$($t.Xex).xex game"
            [IO.File]::WriteAllText((Join-Path $pc 'Exo Zombies.cmd'), (($lines -join "`r`n") + "`r`n"))
        }
    }
    if ($Game -eq 'mw3' -and @($Keys) -contains 'mp') { Install-CamoPacks (Join-Path $pcGame 'camos') }
    $envFile = Join-Path $pc '.env'
    if (-not (Test-Path $envFile)) {
        # The name every PC starts with is its own, as every phone's is
        # (Prefs.kt): the computer's name and the end of the number Windows
        # gave this install, so two players who never chose one differ.
        $guid = "$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Cryptography' -ErrorAction SilentlyContinue).MachineGuid)" -replace '[^0-9A-Fa-f]'
        $computer = "$env:COMPUTERNAME"
        if ($computer.Length -gt 10) { $computer = $computer.Substring(0, 10) }
        $own = ("$computer " + $guid.Substring([Math]::Max(0, $guid.Length - 4)).ToUpper()).Trim()
        if (-not $own) { $own = 'Player' }
        $name = if ($Interactive) { (Read-Host "Your player name online (15 characters at most; empty for `"$own`")").Trim() } else { '' }
        if (-not $name) { $name = $own }
        if ($name.Length -gt 15) { $name = $name.Substring(0, 15) }
        $online = "MW2_NETWORK=server`nMW2_SERVER=$DefaultRelay`nMW2_NAME=$name`nMW2_FULLSCREEN=0`n"
        # Call of Duty 4 has nothing to unlock. Advanced Warfare's Create a
        # Class has padlocks under Xbox LIVE, which a relay signs it in to,
        # as Modern Warfare 3's has; it is also told where the install
        # disc's packages are rather than given a copy of their 4 GB. Each
        # has a log of its own name.
        $settings = if ($Cod4) { $online + "MW2_LOG_FILE=cod4.log`n" }
                    elseif ($Aw) { $online + "MW2_UNLOCK_ALL=1`nMW2_LOG_FILE=aw.log`nMW2_DLC_DIR=$InstallFolder`n" }
                    elseif ($Mw2) { $online + "MW2_LOG_FILE=mw2.log`n" }
                    else { $online + "MW2_UNLOCK_ALL=1`nMW2_LOG_FILE=mw3.log`n" }
        [IO.File]::WriteAllText($envFile, $settings)
    }
    # The player's packs are read where they lie too: their folder is named
    # after the install disc's (runtime/kernel/content.cpp takes several).
    if ($DlcRoot) {
        $lines = @([IO.File]::ReadAllLines($envFile) | Where-Object { $_ -notmatch '^MW2_DLC_DIR=' })
        $folders = if ($Aw) { "$InstallFolder;$DlcRoot" } else { $DlcRoot }
        [IO.File]::WriteAllText($envFile, (($lines + "MW2_DLC_DIR=$folders") -join "`n") + "`n")
    }
}

# ---- 7. onto the phone -------------------------------------------------------
if ($ForAndroid -and $Apks.Count -and $NoInstall) {
    Note 'Nothing is installed (-NoInstall): the APKs are in out\.'
} elseif ($ForAndroid -and $Apks.Count) {
    $devices = @(Get-Devices)
    $ready = @($devices | Where-Object { $_.State -eq 'device' })
    if (@($devices | Where-Object { $_.State -eq 'unauthorized' }).Count) { Warn 'a phone is connected but has not allowed USB debugging: accept the prompt on its screen.' }
    $serial = if ($ready.Count) { Select-InstallDevice $ready $Device $Interactive }
    if ($ready.Count -eq 0) {
        Note 'No phone connected with USB debugging on; install the APKs later (README, "Install on Android").'
    } elseif (-not $serial) {
        # The command this run was, with the choice it lacked: what is built
        # is skipped the second time, so it goes straight to the phone.
        $again = @($PSBoundParameters.GetEnumerator() | Where-Object { $_.Key -notin 'Install', 'Device' } | ForEach-Object {
            if ($_.Value -is [switch]) { if ($_.Value) { "-$($_.Key)" } }
            elseif ("$($_.Value)" -match '\s') { "-$($_.Key) `"$($_.Value)`"" }
            else { "-$($_.Key) $($_.Value)" } }) -join ' '
        Warn "the build is ready and was NOT installed: $(if ($Device) { "no phone with the serial $Device is connected" } else { 'this run asks nothing and no phone was named' }). To install it, run$(if ($ready.Count -gt 1) { ' one of' }):"
        foreach ($phone in $ready) { Note "setup\setup.cmd $again -Install -Device $($phone.Serial)" }
    } else {
        if ($Install -or (Ask "Install on $serial now?" $true)) {
            foreach ($item in $Apks) { Install-Package $serial $item }
            # The camouflage packs that are not in the published app, which
            # carries the project's own alone -- the ones fetched and the
            # player's own mods -- go into the game folder, where the app
            # leaves a pack it did not put there as it is (CamoPacks.kt).
            if ($Split -and $Game -eq 'mw3' -and -not $GameOnly -and -not $BuildApp) {
                $packs = Join-Path $Out 'camos'
                Install-CamoPacks $packs
                $gameFolder = "/sdcard/Android/data/$AppPackage/files/game"
                try {
                    # An if, not `[ -d ] &&`: a folder that is not there must not be a failed command.
                    if ("$(Exec $Adb @('-s', $serial, 'shell', "if [ -d $gameFolder ]; then echo there; fi"))" -match 'there') {
                        Exec $Adb @('-s', $serial, 'push', $packs, "$gameFolder/") | Out-Null
                        Note 'camouflage packs copied into the game folder.'
                    } else { Note 'the game files are not installed on the phone yet: run setup again afterwards for the camouflage packs of other authors.' }
                } catch { Warn "could not copy the camouflage packs: $($_.Exception.Message)" }
            }
            # Each title used to be an app of its own (Modern Warfare 3's
            # multiplayer; Modern Warfare 2's campaign and multiplayer). Its
            # profile, saves and rank are that app's saves folder, which the
            # app keeps as saves for the campaign and mp\saves for the
            # multiplayer (Title.home); an app cannot read another's folder on
            # a current Android, adb can. Never over saves already there, and
            # the old app and its files are left as they are.
            if ($Split -and -not $Suffix -and -not $Cod4 -and -not $Aw) {
                $files = "/sdcard/Android/data/$AppPackage/files"
                $oldApps = if ($Mw2) {
                    @(@{ Package = 'cod.modern.warfare2.sp'; Home = $files; Label = 'campaign' },
                      @{ Package = 'cod.modern.warfare2.mp'; Home = "$files/mp"; Label = 'multiplayer' })
                } else {
                    @(@{ Package = 'com.mw3.recomp.mp'; Home = "$files/mp"; Label = 'multiplayer' })
                }
                foreach ($old in $oldApps) {
                    $oldSaves = "/sdcard/Android/data/$($old.Package)/files/saves"
                    try {
                        $moved = Exec $Adb @('-s', $serial, 'shell', "if [ -d $oldSaves ] && [ ! -e $($old.Home)/saves ]; then mkdir -p $($old.Home) && cp -r $oldSaves $($old.Home)/saves && echo copied; fi")
                        if ("$moved" -match 'copied') { Note "the old $($old.Label) app's saves were copied into the app." }
                        if ("$(Exec $Adb @('-s', $serial, 'shell', 'pm', 'list', 'packages', $old.Package))" -match "$([regex]::Escape($old.Package))(\s|$)") {
                            Note "the old $($old.Label) app ($($old.Package)) is no longer needed: uninstalling it on the phone frees its copy of the game files."
                        }
                    } catch { Warn "could not look for the old $($old.Label) app's saves: $($_.Exception.Message)" }
                }
            }
            # Advanced Warfare's install disc, and a player's own map packs
            # (-Dlc): the packages go beside the game folder, where the
            # runtime looks for a title's installed content
            # (runtime/kernel/content.cpp). Files of the same size are skipped.
            if (($Aw -and -not $GameOnly) -or $DlcFiles.Count) {
                $dlc = "/sdcard/Android/data/$AppPackage/files/dlc"
                Exec $Adb @('-s', $serial, 'shell', "mkdir -p $dlc") | Out-Null
                $packages = @($DlcFiles | ForEach-Object { Get-Item -LiteralPath $_ })
                if ($Aw -and -not $GameOnly) {
                    Step 'adb push the install disc''s packages (4 GB) to the app''s dlc folder'
                    $packages = @(Get-ChildItem -Recurse -File $InstallFolder) + $packages
                }
                if ($DlcFiles.Count) { Step ("adb push your map packs{0} ({1:N1} GB) to the app's dlc folder" -f $(if ($DlcUpdate -and $Aw) { ' and the title update' } else { '' }), ((($DlcFiles | ForEach-Object { (Get-Item -LiteralPath $_).Length }) | Measure-Object -Sum).Sum / 1GB)) }
                foreach ($package in $packages) {
                    $there = "$(Exec $Adb @('-s', $serial, 'shell', "stat -c %s $dlc/$($package.Name) 2>/dev/null || echo 0"))".Trim()
                    if ($there -ne "$($package.Length)") { Exec $Adb @('-s', $serial, 'push', $package.FullName, "$dlc/$($package.Name)") | Out-Null }
                }
            }
        }
        if ($Iso -and ($PushIso -or (Ask 'Copy the disc image to the phone''s Download folder for the app''s installer (7-8 GB, several minutes)?' $false))) {
            Step 'adb push the disc image to /sdcard/Download/'
            Exec $Adb @('-s', $serial, 'push', $Iso, '/sdcard/Download/')
        }
    }
}

# ---- 8. done -----------------------------------------------------------------
Step 'done'
foreach ($item in $Apks) { Note "APK: $($item.Apk)" }
if ($ForPc) {
    Note "Windows game: $PcFolder (start Campaign.cmd or Multiplayer.cmd; settings in .env)"
    if ('mp17' -in $PcKeys) { Note 'With your map packs: "Multiplayer with map packs.cmd" and "Exo Zombies.cmd" (docs\dlc.md).' }
    elseif ($DlcRoot) { Note 'Your map packs are read from where they are (MW2_DLC_DIR in .env; docs\dlc.md).' }
}
if ($GameOnly) { Note 'The game code is built for this state of the folder. If the app on the phone still refuses it, the app is older or newer than this folder: update the app (it offers to) or the folder (git pull).' }
else {
    # Advanced Warfare's README is the front page of its own repository, where
    # it is README.md; here it lies beside the other games'.
    $guide = if ($Mw2) { 'docs\MW2-ANDROID.md' } elseif ($Cod4) { 'docs\COD4-BRINGUP.md' } elseif ($Aw -and (Test-Path (Join-Path $Repo 'README-AW.md'))) { 'README-AW.md' } else { 'README.md' }
    Note "On the phone: open the app, `"Install the game files`", choose the disc image. See $guide."
}
