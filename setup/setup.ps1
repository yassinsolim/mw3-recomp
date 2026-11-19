<#
.SYNOPSIS
Builds Modern Warfare 3 for Android (APK) and/or Windows from your own disc image.

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
     supports (tools/check_mw3_xex.py) and refuses any other;
  4. recompiles them to C++ (XenonRecomp) and builds the Android app (one app
     with the campaign, Special Ops and the multiplayer) and, if asked, the
     Windows game;
  5. installs the app on a phone connected over USB, if asked.
Everything it makes is in the git-ignored out\, mw3\, ppc_mw3*\ and
XenonRecomp\ folders: it contains code made from your copy of the game and is
for your own devices only. Never upload or share it.
Re-runnable: finished steps are skipped. A first run takes one to several
hours and about 40 GB of disk (60 GB with the Windows game).

.PARAMETER Iso
The disc image. Asked for when not given.
.PARAMETER Title
both (default), sp (campaign and Special Ops) or mp (multiplayer): which
Windows executables to build. The Android app always holds both.
.PARAMETER Build
android (default), pc, or both. Asked for when not given.
.PARAMETER Install
Install the built APK on the connected phone (adb) without asking.
.PARAMETER PushIso
Copy the disc image to the phone's Download folder, for the app's installer.
.PARAMETER Instance
2..9 with -Title mp: instead of the app, a copy of the multiplayer alone
(com.mw3.recomp.mp2) to run beside it on one phone, for testing two players
on one device.
.PARAMETER Recomp
Recompile even when a recompiled tree is already there.
.PARAMETER Check
Read-only: report which prerequisites are present and what would be
downloaded or installed, then stop. Nothing is written.
.PARAMETER Yes
Ask nothing: take the defaults (both titles, Android only) and the switches
given. Licence terms of the Android SDK are accepted on your behalf.
.PARAMETER Toolchain
Where the tools go (default: .toolchain\ in the repository).

.EXAMPLE
setup\setup.cmd
.EXAMPLE
setup\setup.cmd -Iso "D:\Games\MW3.iso" -Build both -Install
.EXAMPLE
setup\setup.cmd -Check
#>
[CmdletBinding()]
param(
    [string]$Iso,
    [ValidateSet('both', 'sp', 'mp')][string]$Title,
    [ValidateSet('android', 'pc', 'both')][string]$Build,
    [switch]$Install,
    [switch]$PushIso,
    [ValidateRange(1, 9)][int]$Instance = 1,
    [switch]$Recomp,
    [switch]$Check,
    [switch]$Yes,
    [string]$Toolchain
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

# What differs between the disc's two executables (win_build.ps1's table).
$Titles = @{
    sp = @{ Name = 'mw3'; Label = 'campaign and Special Ops'; Xex = 'default'; Ppc = 'ppc_mw3'; Toml = 'MW3.toml'
            Tables = 'mw3_switch_tables.toml'; Files = 146; Jump = 128 }
    mp = @{ Name = 'mw3mp'; Label = 'multiplayer'; Xex = 'default_mp'; Ppc = 'ppc_mw3_mp'; Toml = 'MW3MP.toml'
            Tables = 'mw3mp_switch_tables.toml'; Files = 153; Jump = 156 }
}

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
        $t = $Titles[$key]; $xex = Join-Path $Repo "mw3\tu0\$($t.Xex).xex"
        $state = 'not extracted yet'
        if (Test-Path $xex) {
            if ($py) {
                $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
                $verdict = & $py (Join-Path $Repo 'tools\check_mw3_xex.py') $xex 2>&1
                $ErrorActionPreference = $old
                $state = "extracted; $verdict"
            } else { $state = 'extracted (no Python to check it)' }
        }
        $tree = if (Test-Path (Join-Path $Repo "$($t.Ppc)\ppc_func_mapping.cpp")) { 'recompiled' } else { 'not recompiled' }
        Note "$($t.Xex).xex ($($t.Label)): $state; $tree"
    }
    if ($Iso) { Note "disc image: $(if (Test-Path -LiteralPath $Iso -PathType Leaf) { "$Iso ($([math]::Round((Get-Item -LiteralPath $Iso).Length / 1GB, 2)) GB)" } else { "$Iso NOT FOUND" })" }
    $devices = Get-Devices
    Note "Android devices (adb): $(if (-not (Test-Path $Adb)) { 'adb not installed yet' } elseif ($devices.Count) { ($devices | ForEach-Object { "$($_.Serial) ($($_.State))" }) -join ', ' } else { 'none connected' })"
    return
}

Write-Host 'Modern Warfare 3 (Xbox 360) recompiled: setup' -ForegroundColor Green
Write-Host 'This builds the game from your own disc image. What it makes contains code'
Write-Host 'from your copy of the game: keep it to your own devices, never share it.'
Write-Host ''
if ($freeGb -lt 40) { Warn "only $freeGb GB free on ${drive}: about 40 GB is wanted (60 GB with the Windows game)." }
if ($memGb -lt 16) { Warn "$memGb GB of memory: the recompiled code is large, and the build may run out below 16 GB." }

# ---- 1. what to build --------------------------------------------------------
if (-not $Title) { $Title = Choose 'Which part of the game for Windows: both, sp (campaign and Special Ops) or mp (multiplayer)? The Android app always holds both.' @('both', 'sp', 'mp') 'both' }
if (-not $Build) { $Build = Choose 'Build for android, pc (Windows) or both?' @('android', 'pc', 'both') 'android' }
if ($Instance -gt 1 -and $Title -ne 'mp') { throw '-Instance is for -Title mp' }
$ForAndroid = $Build -in 'android', 'both'
$ForPc = $Build -in 'pc', 'both'
$TestCopy = $Instance -gt 1
# The Windows game is an executable for each title asked for. The Android app
# carries both, so it needs both recompiled whatever -Title says; the
# multiplayer test copy (-Instance) needs only its own.
$PcKeys = if ($Title -eq 'both') { @('sp', 'mp') } else { @($Title) }
$Keys = if ($ForAndroid -and -not $TestCopy) { @('sp', 'mp') } else { $PcKeys }

$needIso = ($ForPc -and -not (Test-Path (Join-Path $Out 'pc\game\default.xex'))) -or
           @($Keys | Where-Object { -not (Test-Path (Join-Path $Repo "mw3\tu0\$($Titles[$_].Xex).xex")) }).Count -gt 0 -or $PushIso
while ($needIso -and -not ($Iso -and (Test-Path -LiteralPath $Iso -PathType Leaf))) {
    if ($Iso) { Warn "no file at $Iso" }
    if (-not $Interactive) { throw 'Give the disc image with -Iso <path>.' }
    $Iso = (Read-Host 'Path to your Modern Warfare 3 disc image (.iso; you can drag the file into this window)').Trim().Trim('"')
}
if ($Iso) { $Iso = (Resolve-Path -LiteralPath $Iso).Path }

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
foreach ($key in @($Keys)) {
    $t = $Titles[$key]
    $xexRel = "mw3/tu0/$($t.Xex).xex"
    if (-not (Test-Path (Join-Path $Repo $xexRel))) {
        Step "extract $($t.Xex).xex from the disc image (read-only)"
        Exec $Python @('tools/xdvdfs.py', $Iso, 'mw3/tu0', "$($t.Xex).xex") -Cwd $Repo -Log "extract_$($t.Xex).log" -Quiet
        if (-not (Test-Path (Join-Path $Repo $xexRel))) { throw "$($t.Xex).xex is not on that disc image: is it Modern Warfare 3 for the Xbox 360?" }
    }
    try { Exec $Python @('tools/check_mw3_xex.py', $xexRel) -Cwd $Repo }
    catch {
        if ($Keys.Count -gt 1) {
            Warn "$($t.Xex).xex is not a build this port supports; skipping the $($t.Label)."
            $Keys = @($Keys | Where-Object { $_ -ne $key })
        } else { throw "$($t.Xex).xex is not a disc build this port supports (tools/check_mw3_xex.py lists them)." }
    }
}
$PcKeys = @($PcKeys | Where-Object { $_ -in $Keys })
if ($ForAndroid -and -not $TestCopy -and $Keys.Count -lt 2) {
    throw 'The Android app holds the campaign and the multiplayer, and needs both executables to be disc builds this port supports (tools/check_mw3_xex.py lists them).'
}

# ---- 4. recompile ------------------------------------------------------------
$toRecompile = @($Keys | Where-Object { $Recomp -or -not (Test-Path (Join-Path $Repo "$($Titles[$_].Ppc)\ppc_func_mapping.cpp")) })
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
    # MSVC/Windows SDK include and library paths, for this process only.
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^(INCLUDE|LIB|LIBPATH|PATH)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
    if (-not $env:INCLUDE) { throw "vcvars64.bat did not set INCLUDE ($vcvars)" }
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

foreach ($key in @($Keys)) {
    $t = $Titles[$key]
    $xexRel = "mw3/tu0/$($t.Xex).xex"
    $ppcDir = Join-Path $Repo $t.Ppc
    $env:MW2_TITLE = $t.Name; $env:MW2_VERSION = 'tu0'
    if ($key -in $toRecompile) {
        Step "recompile $($t.Xex).xex ($($t.Label)); this takes a while"
        # tools/xexdump.py decrypts with openssl.exe, found on PATH.
        $savedPath = $env:PATH; $env:PATH = "$OpenSslDir;$env:PATH"
        try { Exec $Python @('tools/xexdump.py', $xexRel, "mw3/tu0/$($t.Xex).pe") -Cwd $Repo -Log "$($t.Name)_xexdump.log" -Quiet }
        finally { $env:PATH = $savedPath }
        Exec $AnalyseExe @($xexRel, "config/$($t.Tables)") -Cwd $Repo -Log "$($t.Name)_analyse.log" -Quiet
        # XenonRecomp crashes, rather than complaining, when its output directory is missing.
        if (Test-Path $ppcDir) { Remove-Item -Recurse -Force $ppcDir }
        New-Item -ItemType Directory $ppcDir | Out-Null
        Exec $XenonExe @($t.Toml, '../XenonRecomp/XenonUtils/ppc_context.h') -Cwd (Join-Path $Repo 'config') -Log "$($t.Name)_recomp.log" -Quiet
        $recompLog = Join-Path $Logs "$($t.Name)_recomp.log"
        $count = { param($file, $pattern) @(Select-String -Path $file -SimpleMatch $pattern).Count }
        $files = @(Get-ChildItem $ppcDir -File).Count
        $tables = & $count (Join-Path $Repo "config\$($t.Tables)") '[[switch]]'
        $bad = (& $count $recompLog 'Unrecognized instruction') + (& $count $recompLog 'trying to jump outside function')
        Note "files: $files (expect $($t.Files))   jump tables: $tables (expect $($t.Jump))   recompiler errors: $bad (expect 0)"
        if ($files -ne $t.Files -or $tables -ne $t.Jump -or $bad -ne 0) { Warn 'the recompiled tree differs from the tested one; the build may fail or misbehave.' }
        Step 'generate the shared header'
        Exec $Python @('tools/genshared.py') -Cwd $Repo -Log "$($t.Name)_genshared.log" -Quiet
    }
    # Every run: the runtime implements more imports as it is updated (git
    # pull), and a stub left for one it now has would clash with it at the link.
    Exec $Python @('tools/gen_kernel_stubs.py') -Cwd $Repo -Log "$($t.Name)_stubs.log" -Quiet
}

# ---- 5. the Android apps -----------------------------------------------------
$Apks = @()
if ($ForAndroid) {
    $adreno = Join-Path $Repo 'third_party\libadrenotools'
    if (-not (Test-Path "$adreno\CMakeLists.txt")) {
        Step 'clone libadrenotools'
        # android/fetch_deps.sh names a revision the remote does not serve and
        # falls back to the default branch's head; this is that fallback.
        if (Test-Path $adreno) { Remove-Item -Recurse -Force $adreno }
        Exec $GitTools.Git @('clone', '-q', '--depth', '1', '-c', 'core.autocrlf=false', $AdrenoRemote, $adreno)
        Exec $GitTools.Git @('-C', $adreno, 'submodule', 'update', '-q', '--init', '--recursive', '--depth', '1')
    }
    # The Turnip driver the campaign app bundles (THIRD_PARTY.md): downloaded,
    # hash-checked, never committed. A failure leaves the APK without it
    # rather than stopping the build.
    $turnip = Join-Path $Repo 'android\app\src\mw3\assets\drivers\turnip_v27_r2.zip'
    if (-not ((Test-Path $turnip) -and (Sha256Of $turnip) -eq $TurnipSha)) {
        Step 'bundled Turnip driver'
        try {
            New-Item -ItemType Directory -Force (Split-Path -Parent $turnip) | Out-Null
            Exec 'curl.exe' @('-fsSL', '--retry', '3', '-o', "$turnip.part", $TurnipUrl)
            if ((Sha256Of "$turnip.part") -ne $TurnipSha) { throw 'SHA-256 of the Turnip package does not match' }
            Move-Item -Force "$turnip.part" $turnip
        } catch {
            Warn "no bundled Turnip driver: $($_.Exception.Message)"
            Remove-Item -Force -ErrorAction SilentlyContinue $turnip, "$turnip.part"
        }
    }
    [IO.File]::WriteAllText((Join-Path $Repo 'android\local.properties'), "sdk.dir=$($Sdk -replace '\\', '/')`n")
    $env:MW2_LTO = 'OFF'
    # One app with both titles: the campaign's build makes the multiplayer's
    # library as well (cmake/android.cmake, MW3_BOTH_TITLES).
    $app = if ($TestCopy) {
        @{ Label = "multiplayer test copy $Instance"; Task = 'assembleMw3mpTu0Debug'; Variant = 'mw3mpTu0'; Apk = "mw3mp$Instance-debug.apk" }
    } else {
        @{ Label = 'app (campaign, Special Ops and multiplayer)'; Task = 'assembleMw3Tu0Debug'; Variant = 'mw3Tu0'; Apk = 'mw3-debug.apk' }
    }
    Step "build the $($app.Label) (gradle $($app.Task); the longest step, log: out\logs\gradle_android.log)"
    # A modest daemon: most of the memory goes to the compilers it starts.
    $gradleArgs = @('--no-daemon', '-Dorg.gradle.jvmargs=-Xmx1536m -XX:MaxMetaspaceSize=512m -Dfile.encoding=UTF-8')
    if ($TestCopy) { $gradleArgs += "-PmpInstance=$Instance" }
    Exec (Join-Path $Repo 'android\gradlew.bat') ($gradleArgs + $app.Task) -Cwd (Join-Path $Repo 'android') -Log 'gradle_android.log' -Quiet
    $built = Get-ChildItem (Join-Path $Repo "android\app\build\outputs\apk\$($app.Variant)\debug\*.apk") | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $built) { throw "gradle finished but no APK under android\app\build\outputs\apk\$($app.Variant)\debug" }
    $apk = Join-Path $Out $app.Apk
    Copy-Item $built.FullName $apk -Force
    $Apks += $apk
    Note ("{0} ({1:N1} MB)" -f $apk, ((Get-Item $apk).Length / 1MB))
}

# ---- 6. the Windows game -----------------------------------------------------
if ($ForPc) {
    if (-not (Test-Path "$MingwDir\bin\x86_64-w64-mingw32-clang++.exe")) { Step 'unpack llvm-mingw'; Unpack (Fetch $Pins.LlvmMingw) $MingwDir }
    $pc = Join-Path $Out 'pc'
    $game = Join-Path $pc 'game'
    New-Item -ItemType Directory -Force $game | Out-Null
    # The whole disc, about 7.4 GB: the multiplayer picks its own maps, and a
    # missing zone reads as a dirty disc. Files already there are skipped.
    if ($Iso) {
        Step 'copy the game files from the disc image (read-only; about 7.4 GB)'
        Exec $Python @('tools/xdvdfs.py', $Iso, $game) -Cwd $Repo -Log 'extract_pc_game.log' -Quiet
    }
    $fwd = { param($p) $p -replace '\\', '/' }
    foreach ($key in @($PcKeys)) {
        $t = $Titles[$key]
        $buildDir = Join-Path $Out "build-pc-$($t.Name)"
        Step "build the Windows $($t.Label) (log: out\logs\pc_$($t.Name).log)"
        $env:MW2_TITLE = $t.Name; $env:MW2_VERSION = 'tu0'
        # A diagnostics build, as tested: its log is what a bug report needs.
        Exec $CMake @('-S', $Repo, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$(& $fwd $Ninja)",
                      '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_TOOLCHAIN_FILE=$(& $fwd (Join-Path $Repo 'cmake\mingw-w64.cmake'))",
                      "-DLLVM_MINGW=$(& $fwd $MingwDir)", "-DMW2_TITLE=$($t.Name)", '-DMW3_RECOMP_ONLY=ON', '-DMW3_LINK_PROBE=ON',
                      '-DMW2_DIAGNOSTICS=ON', '-DMW2_PORTABLE=OFF', '-DMW2_LTO=OFF') -Log "pc_$($t.Name)_configure.log" -Quiet
        Exec $CMake @('--build', $buildDir, '--target', 'mw2') -Log "pc_$($t.Name).log" -Quiet
        Copy-Item (Join-Path $buildDir "$($t.Name).exe") (Join-Path $pc "$($t.Name).exe") -Force
        $launcher = if ($key -eq 'mp') { 'Multiplayer.cmd' } else { 'Campaign.cmd' }
        $lines = '@echo off', 'rem Settings are in .env beside this file (docs/switches.md); the log is mw3.log.',
                 'cd /d "%~dp0"', "$($t.Name).exe game\$($t.Xex).xex game"
        [IO.File]::WriteAllText((Join-Path $pc $launcher), (($lines -join "`r`n") + "`r`n"))
    }
    $envFile = Join-Path $pc '.env'
    if (-not (Test-Path $envFile)) {
        $name = if ($Interactive) { (Read-Host 'Your player name online (15 characters at most; empty for "Player")').Trim() } else { '' }
        if (-not $name) { $name = 'Player' }
        if ($name.Length -gt 15) { $name = $name.Substring(0, 15) }
        $settings = "MW2_NETWORK=server`nMW2_SERVER=$DefaultRelay`nMW2_NAME=$name`nMW2_FULLSCREEN=0`nMW2_UNLOCK_ALL=1`nMW2_LOG_FILE=mw3.log`n"
        [IO.File]::WriteAllText($envFile, $settings)
    }
}

# ---- 7. onto the phone -------------------------------------------------------
if ($ForAndroid -and $Apks.Count) {
    $devices = @(Get-Devices)
    $ready = @($devices | Where-Object { $_.State -eq 'device' })
    if (@($devices | Where-Object { $_.State -eq 'unauthorized' }).Count) { Warn 'a phone is connected but has not allowed USB debugging: accept the prompt on its screen.' }
    if ($ready.Count -eq 0) {
        Note 'No phone connected with USB debugging on; install the APKs later (README, "Install on Android").'
    } else {
        $serial = $ready[0].Serial
        if ($ready.Count -gt 1) { Warn "more than one phone connected; using $serial" }
        if ($Install -or (Ask "Install the app on $serial now?" $true)) {
            foreach ($apk in $Apks) {
                Step "adb install $(Split-Path -Leaf $apk)"
                # A copy built elsewhere (another clone, another PC) is signed with
                # another debug key, and Android refuses to update it in place. The
                # build is done by now, so say what to do rather than fail the run.
                try { Exec $Adb @('-s', $serial, 'install', '-r', $apk) }
                catch { Warn "could not install $(Split-Path -Leaf $apk) on ${serial}: an app of that name built on another PC or from another copy of this folder is probably installed. Uninstall it on the phone (its game files go with it) and install $apk again, or copy the APK over and open it there." }
            }
            # The multiplayer used to be an app of its own (com.mw3.recomp.mp).
            # Its profile and rank are that app's saves folder, which the app's
            # multiplayer keeps as mp\saves; an app cannot read another's
            # folder on a current Android, adb can. Never over saves already
            # there, and the old app and its files are left as they are.
            if (-not $TestCopy) {
                $oldSaves = '/sdcard/Android/data/com.mw3.recomp.mp/files/saves'
                $home3 = '/sdcard/Android/data/com.mw3.recomp/files/mp'
                try {
                    $moved = Exec $Adb @('-s', $serial, 'shell', "if [ -d $oldSaves ] && [ ! -e $home3/saves ]; then mkdir -p $home3 && cp -r $oldSaves $home3/saves && echo copied; fi")
                    if ("$moved" -match 'copied') { Note 'the old multiplayer app''s profile and rank were copied into the app.' }
                    if ("$(Exec $Adb @('-s', $serial, 'shell', 'pm', 'list', 'packages', 'com.mw3.recomp.mp'))" -match 'com\.mw3\.recomp\.mp\s*$') {
                        Note 'the old multiplayer app (CoD: MW3 Multiplayer) is no longer needed: uninstalling it on the phone frees its copy of the game files.'
                    }
                } catch { Warn "could not look for the old multiplayer app's saves: $($_.Exception.Message)" }
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
foreach ($apk in $Apks) { Note "APK: $apk" }
if ($ForPc) {
    Note "Windows game: $(Join-Path $Out 'pc') (start Campaign.cmd or Multiplayer.cmd; settings in .env)"
}
Note 'On the phone: open the app, "Install the game files", choose the disc image. See README.md.'
