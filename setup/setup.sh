#!/usr/bin/env bash
# Builds a game's code for Android from your own disc image, on Linux: what
# setup\setup.ps1 does on Windows for the Android app, for Modern Warfare 3
# (the default), Modern Warfare 2 (--game mw2), Call of Duty 4 (--game cod4)
# and Call of Duty: Advanced Warfare (--game aw, docs/AW-BRINGUP.md).
#
#   bash setup/setup.sh --game mw2 --iso ~/Downloads/MW2.iso
#
# It
#   1. fetches the tools it needs into .toolchain/ beside the repository (a
#      JDK and the Android SDK and NDK, each pinned; nothing installed
#      system-wide). What it cannot carry with it are a few programs of the
#      system's own, which it names before doing anything else:
#        sudo apt install git curl unzip python3 openssl clang
#      (clang 17 or later builds the recompiler; Ubuntu 24.04's is 18);
#   2. reads the two executables out of your disc image (opened for reading
#      only), or takes them from a folder (--xex-dir);
#   3. checks each executable's SHA-256 against the disc builds this port
#      supports and refuses any other;
#   4. recompiles them to C++ (XenonRecomp) and builds the game code for
#      Android: a package of its own with no screen, out/<game>-game.apk;
#   5. downloads the published app, and installs both on a phone connected
#      over USB, if asked (docs/game-library.md).
# Everything it makes is in the git-ignored out/, mw3/ (mw2/, cod4/, aw/), ppc_*/
# and XenonRecomp/ folders: it contains code made from your copy of the game
# and is for your own devices only. Never upload or share it.
# Re-runnable: finished steps are skipped. The Windows game is built by
# setup\setup.ps1 only, and other authors' camouflage packs are fetched by
# the app itself (Settings, Multiplayer, Camo packs).
set -euo pipefail

usage() {
    cat <<'EOF'
usage: bash setup/setup.sh [options]

  --game mw3|mw2|cod4|aw  which game (default mw3)
  --iso <file>          your disc image (Advanced Warfare: the game disc's);
                        asked for when it is needed and not given
  --install-iso <file>  Advanced Warfare: the image of its second disc, the
                        install disc. Its two content packages are read out
                        of it (4 GB) and copied to the phone with the app.
                        Without it the app's own installer asks for that
                        image on the phone
  --xex-dir <folder>    take default.xex and default_mp.xex from this folder
                        instead of a disc image (also as <game>-default.xex,
                        the names the app's "Prepare files for building
                        online" writes)
  --install             install on the connected phone without asking
  --no-install          build only: nothing is installed on or copied to
                        any phone, whatever is connected
  --device <serial>     the phone to install on (`adb devices`). Without it
                        you are asked about the one connected, or the first
                        of several; with --yes nothing is installed unless
                        the phone is named here and connected, and the
                        command for each phone is printed
  --push-iso            also copy the disc image to the phone's Download folder
  --game-only           build and install the game code only; leave the app alone
  --dev                 build the app here instead of downloading the published one
  --suffix .dev         with --dev or --game-only: a suffix for the package names,
                        so a development build lives beside the published app
  --recomp              recompile even when a recompiled tree is already there
  --jobs <n>            compilers run at once (default: from the free memory)
  --sdk <folder>        an Android SDK to use and add to, instead of .toolchain/android-sdk
  --toolchain <folder>  where the tools go (default: .toolchain/ in the repository)
  --check               read-only: say what is present and what would be fetched
  --yes                 ask nothing; the Android SDK's licence terms are accepted for you
EOF
}

GAME=mw3; ISO=; INSTALL_ISO=; XEX_DIR=; INSTALL=0; NO_INSTALL=0; DEVICE=; PUSH_ISO=0; GAME_ONLY=0; DEV=0
SUFFIX=; RECOMP=0; JOBS=; SDK=; TC=; CHECK=0; YES=0
# As given, for the command printed when no phone was chosen (step 6).
ARGS=("$@")
# -Device, as setup.ps1 spells it, is taken too.
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME=${2:?--game needs a name}; shift ;;
        --iso) ISO=${2:?--iso needs a file}; shift ;;
        --install-iso) INSTALL_ISO=${2:?--install-iso needs a file}; shift ;;
        --xex-dir) XEX_DIR=${2:?--xex-dir needs a folder}; shift ;;
        --install) INSTALL=1 ;;
        --no-install) NO_INSTALL=1 ;;
        --device|-Device) DEVICE=${2:?--device needs a serial}; shift ;;
        --push-iso) PUSH_ISO=1 ;;
        --game-only) GAME_ONLY=1 ;;
        --dev) DEV=1 ;;
        --suffix) SUFFIX=${2?--suffix needs a value}; shift ;;
        --recomp) RECOMP=1 ;;
        --jobs) JOBS=${2:?--jobs needs a number}; shift ;;
        --sdk) SDK=${2:?--sdk needs a folder}; shift ;;
        --toolchain) TC=${2:?--toolchain needs a folder}; shift ;;
        --check) CHECK=1 ;;
        --yes) YES=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TC=${TC:-$REPO/.toolchain}
DL=$TC/dl
OUT=$REPO/out
LOGS=$OUT/logs
SDK=${SDK:-$TC/android-sdk}
INTERACTIVE=0
if [ "$YES" = 0 ] && [ "$CHECK" = 0 ] && [ -t 0 ]; then INTERACTIVE=1; fi

START=$(date +%s)
step() { local t=$(( $(date +%s) - START )); printf '\033[36m[%02d:%02d:%02d] %s\033[0m\n' $((t / 3600)) $((t / 60 % 60)) $((t % 60)) "$*"; }
note() { echo "    $*"; }
warn() { printf '\033[33m    warning: %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

ask() {   # ask <question> <default y|n>
    [ "$INTERACTIVE" = 1 ] || { [ "$2" = y ]; return; }
    local hint='[y/N]' answer
    [ "$2" = y ] && hint='[Y/n]'
    while true; do
        read -r -p "$1 $hint " answer || answer=
        case "${answer,,}" in
            '') [ "$2" = y ]; return ;;
            y|yes) return 0 ;;
            n|no) return 1 ;;
        esac
    done
}

# Runs a program with its output in a log file, and says where the log is
# when it fails: a build's output is thousands of lines.
logged() {   # logged <log name> <command...>
    local log=$LOGS/$1; shift
    if ! "$@" > "$log" 2>&1; then
        tail -n 25 "$log" >&2
        die "$1 failed (the whole log: $log)"
    fi
}

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
# A file's SHA-256 as cmake/game_abi.cmake takes it: without the carriage
# returns, so a checkout with Windows line endings says the same.
abi_hash() { tr -d '\r' < "$1" | sha256sum | cut -d' ' -f1; }

case "$GAME" in mw3|mw2|cod4|aw) ;; *) die "--game must be mw3, mw2, cod4 or aw, not '$GAME'" ;; esac
[ "$NO_INSTALL" = 1 ] && { [ "$INSTALL" = 1 ] || [ "$PUSH_ISO" = 1 ]; } && die '--no-install installs and copies nothing; --install and --push-iso do. Choose one.'
[ -n "$INSTALL_ISO" ] && [ "$GAME" != aw ] && die '--install-iso is for --game aw: the other games have the one disc.'
[ -n "$INSTALL_ISO" ] && [ ! -f "$INSTALL_ISO" ] && die "--install-iso: no file at $INSTALL_ISO"
[[ "$SUFFIX" =~ ^(\.[a-z][a-z0-9]*)?$ ]] || die "--suffix must look like .dev"
[ "$GAME_ONLY" = 1 ] && [ "$DEV" = 1 ] && die '--game-only leaves the app alone; --dev builds it. Choose one.'
[ -n "$SUFFIX" ] && [ "$GAME_ONLY" = 0 ] && [ "$DEV" = 0 ] && die '--suffix goes with --dev or --game-only: the published app has the one name.'
[ -n "$ISO" ] && [ -n "$XEX_DIR" ] && die '--iso and --xex-dir are two sources for the same executables. Choose one.'
[ -n "$JOBS" ] && ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] && die '--jobs needs a number'
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] || die 'A 64-bit (x86-64) Linux is needed: the Android NDK has no other.'

# Every download is pinned to a version and its SHA-256, as setup.ps1's are,
# so a friend's build uses the same tools as the one that was tested.
JDK_FILE=jdk17.tar.gz
JDK_URL='https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.21%2B9/OpenJDK17U-jdk_x64_linux_hotspot_17.0.21_9.tar.gz'
JDK_SHA=8b9b611d917e3dfd8419d3566b5949226f69b052fed7e109c1368add1dfe8711
CMDTOOLS_FILE=cmdline-tools.zip
CMDTOOLS_URL='https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip'
CMDTOOLS_SHA=2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258
XENON_COMMIT=$(sed -n 's/^XENON_COMMIT=//p' "$REPO/build.sh")   # one place for it on Linux, as android/fetch_deps.sh reads it
RELEASE_REPO=yassinsolim/$GAME-recomp   # where the Android app is published (android/app/build.gradle.kts, UPDATE_REPO)
NDK_VERSION=28.2.13676358
SDK_PACKAGES=("ndk;$NDK_VERSION" 'platforms;android-35' 'build-tools;35.0.0' 'cmake;3.22.1' 'platform-tools')

JDK=$TC/jdk17
SDKMANAGER=$SDK/cmdline-tools/latest/bin/sdkmanager
ADB=$SDK/platform-tools/adb
# The SDK's own CMake and Ninja build the recompiler too: one download fewer.
CMAKE=$SDK/cmake/3.22.1/bin/cmake
NINJA=$SDK/cmake/3.22.1/bin/ninja
XENON=$REPO/XenonRecomp
XENON_EXE=$XENON/build/XenonRecomp/XenonRecomp
ANALYSE_EXE=$XENON/build/XenonAnalyse/XenonAnalyse
PATCH=$REPO/patches/xenonrecomp-mw2.patch

# What differs between the disc's two executables, for each game
# (setup.ps1's table): the build's name for the title, its library, the
# executable, the recompiled tree, its configuration and jump tables, and
# what a recompilation of the tested disc gives (files, jump tables, the
# instructions the configuration steps over).
DISC=$GAME/tu0
case "$GAME" in
    mw2) GAME_NAME='Modern Warfare 2'; GUIDE=docs/MW2-ANDROID.md
         T_sp=(sp mw2 'campaign and Special Ops' default ppc_tu0 MW2.tu0.toml mw2_switch_tables.tu0.toml 113 100 0)
         T_mp=(mp mw2mp 'multiplayer' default_mp ppc_mp_tu0 MW2MP.tu0.toml mw2mp_switch_tables.tu0.toml 117 121 0) ;;
    cod4) GAME_NAME='Call of Duty 4: Modern Warfare'; GUIDE=docs/COD4-BRINGUP.md
         T_sp=(cod4 cod4 'campaign' default ppc_cod4 COD4.toml cod4_switch_tables.toml 76 181 9)
         T_mp=(cod4mp cod4mp 'multiplayer' default_mp ppc_cod4_mp COD4MP.toml cod4mp_switch_tables.toml 77 179 9) ;;
    # The last number: 415 instructions of twelve kinds in each executable,
    # which the configuration steps over (config/AW.toml, tools/extra_instructions.py).
    aw)  GAME_NAME='Call of Duty: Advanced Warfare'; GUIDE=README-AW.md
         T_sp=(aw aw 'campaign' default ppc_aw AW.toml aw_switch_tables.toml 207 210 415)
         T_mp=(awmp awmp 'multiplayer' default_mp ppc_aw_mp AWMP.toml awmp_switch_tables.toml 204 230 415) ;;
    *)   GAME_NAME='Modern Warfare 3'; GUIDE=README.md
         T_sp=(mw3 mw3 'campaign and Special Ops' default ppc_mw3 MW3.toml mw3_switch_tables.toml 146 128 0)
         T_mp=(mw3mp mw3mp 'multiplayer' default_mp ppc_mw3_mp MW3MP.toml mw3mp_switch_tables.toml 153 156 0) ;;
esac
# Advanced Warfare's README is the front page of its own repository, where it
# is README.md; here it lies beside the other games'.
[ "$GAME" = aw ] && [ ! -f "$REPO/$GUIDE" ] && GUIDE=README.md
# Modern Warfare 2 has the one disc (CMakeLists.txt, MW2_XEX_SHA256_SP/MP).
MW2_SHA_default=990e640d3a92544f9d585e71faf1ab65082d37ec6f5865b2599335fa123bf6ff
MW2_SHA_default_mp=aacc47aff3221f3bd9129e80827fd3bcd74aa43bf74bb5c5ddbd0597c0d5fcde

title() {   # title sp|mp: the row above, as T_NAME T_LIB T_LABEL T_XEX T_PPC T_TOML T_TABLES T_FILES T_JUMP T_STEPPED
    local -n row=T_$1
    T_NAME=${row[0]}; T_LIB=${row[1]}; T_LABEL=${row[2]}; T_XEX=${row[3]}; T_PPC=${row[4]}
    T_TOML=${row[5]}; T_TABLES=${row[6]}; T_FILES=${row[7]}; T_JUMP=${row[8]}; T_STEPPED=${row[9]}
}

# What a tree recompiled now is made with, as setup.ps1 notes it in the tree
# and as cmake/game_abi.cmake builds the game library's statement from it.
recompiled_with() {
    printf 'recompiler %s\nrecompiler-patch %s\nconfiguration %s\nshared-header %s\n' \
        "$XENON_COMMIT" "$(abi_hash "$PATCH")" "$(abi_hash "$REPO/config/$T_TOML")" "$(abi_hash "$REPO/tools/genshared.py")"
}

# Whether the title's tree is there and was made with what this checkout
# recompiles with (docs/game-library.md).
tree_current() {
    [ -f "$REPO/$T_PPC/ppc_func_mapping.cpp" ] && [ -f "$REPO/$T_PPC/recompiled_with.txt" ] &&
        [ "$(tr -d '\r' < "$REPO/$T_PPC/recompiled_with.txt")" = "$(recompiled_with)" ]
}

# Whether the title's extracted executable is a disc build this port
# supports, as one line of text; fails when it is not.
check_xex() {
    local rel=$DISC/$T_XEX.xex sha want
    case "$GAME" in
        cod4) (cd "$REPO" && python3 tools/check_cod4_xex.py "$rel") ;;
        aw)   (cd "$REPO" && python3 tools/check_aw_xex.py "$rel") ;;
        mw3)  (cd "$REPO" && python3 tools/check_mw3_xex.py "$rel") ;;
        mw2)  sha=$(sha256_of "$REPO/$rel"); want=MW2_SHA_$T_XEX
              [ "$sha" = "${!want}" ] || { echo "$rel: its SHA-256 is not the Modern Warfare 2 disc's"; return 1; }
              echo "$rel: the Modern Warfare 2 disc's (1.0.557)" ;;
    esac
}

# The compiler the recompiler is built with: the system's clang, 17 or later.
find_clang() {
    local c major
    for c in clang++-20 clang++-19 clang++-18 clang++-17 clang++; do
        command -v "$c" > /dev/null 2>&1 || continue
        major=$("$c" -dumpversion | cut -d. -f1)
        if [ "$major" -ge 17 ]; then echo "$c"; return 0; fi
    done
    return 1
}

# A Java 17 or later already here is used; otherwise the pinned one is fetched.
find_java() {
    if [ -x "$JDK/bin/java" ]; then echo "$JDK"; return 0; fi
    local home=${JAVA_HOME:-} major
    if [ -z "$home" ] && command -v javac > /dev/null 2>&1; then home=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")"); fi
    [ -n "$home" ] && [ -x "$home/bin/javac" ] || return 1
    major=$("$home/bin/javac" -version 2>&1 | sed -n 's/^javac \([0-9]*\).*/\1/p')
    [ "${major:-0}" -ge 17 ] && echo "$home"
}

missing_sdk_packages() {
    local p
    for p in "${SDK_PACKAGES[@]}"; do [ -e "$SDK/${p//;//}" ] || echo "$p"; done
}

# Downloads a pinned file into .toolchain/dl (once) and checks its hash.
fetch() {   # fetch <file> <url> <sha256>
    local dest=$DL/$1
    if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$3" ]; then return 0; fi
    step "download $1"
    curl -fL --retry 3 --progress-bar -o "$dest.part" "$2"
    if [ "$(sha256_of "$dest.part")" != "$3" ]; then
        rm -f "$dest.part"
        die "$1: the download does not match its SHA-256; not using it"
    fi
    mv -f "$dest.part" "$dest"
}

devices() {   # the connected phones, a line each: <serial> <device|unauthorized>
    [ -x "$ADB" ] || return 0
    "$ADB" devices 2> /dev/null | awk '$2 == "device" || $2 == "unauthorized" { print $1, $2 }'
}

# Which of the connected phones the APKs go on: the one --device names, when
# it is connected, and never another in its place. With none named,
# somebody at the keyboard is asked about the only one, or warned and asked
# about the first of several; a run that asks nothing (--yes, or no
# terminal) gets none, because it would be a phone nobody chose: such runs
# put an app and 4 GB on the one adb listed first of two, and on the only
# one connected, which was somebody else's at the time.
install_device() {   # install_device <ready serial...>: prints the serial, or nothing
    local candidate
    for candidate in "$@"; do
        if [ "$candidate" = "$DEVICE" ]; then echo "$candidate"; return 0; fi
    done
    if [ -n "$DEVICE" ] || [ "$INTERACTIVE" = 0 ]; then return 0; fi
    if [ $# -gt 1 ]; then warn "more than one phone connected; using $1 (--device <serial> chooses)"; fi
    echo "$1"
}

# ---- 0. preflight ------------------------------------------------------------
# The programs of the system's own, said before anything is fetched.
NEEDED=()
for tool in git curl unzip python3 openssl; do command -v "$tool" > /dev/null 2>&1 || NEEDED+=("$tool"); done
CLANG=$(find_clang) || NEEDED+=(clang)
if command -v python3 > /dev/null 2>&1 && ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
    die "python3 is $(python3 -V 2>&1 | cut -d' ' -f2); the tools need Python 3.10 or later."
fi
FREE_GB=$(( $(df -Pk "$REPO" | awk 'NR == 2 { print $4 }') / 1048576 ))
MEM_GB=$(awk '/^MemTotal:/ { printf "%d", ($2 + 524288) / 1048576 }' /proc/meminfo)
AVAILABLE_MB=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo)

# The compilers run at once. The recompiled code is a few hundred very large
# files; a compiler on one of Modern Warfare 2's was measured at 320 MB at
# most, and each is counted as 768 MB here, for the other games' and for
# what else the PC is doing. More of them than the free memory holds and
# the PC swaps or the build is killed.
if [ -z "$JOBS" ]; then
    JOBS=$(( AVAILABLE_MB / 768 ))
    [ "$JOBS" -lt 1 ] && JOBS=1
    [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
fi

if [ "$CHECK" = 1 ]; then
    step 'prerequisites (read-only check; nothing is downloaded or written)'
    note "repository: $REPO"
    note "toolchain folder: $TC$([ -d "$TC" ] || echo ' (not created yet)')"
    note "free space: $FREE_GB GB (about 8 GB wanted)"
    note "memory: $MEM_GB GB, $AVAILABLE_MB MB of it free now: $JOBS compiler(s) at once on $(nproc) processor(s)"
    note "system programs: $([ ${#NEEDED[@]} = 0 ] && echo "present (compiler: $CLANG)" || echo "missing; sudo apt install ${NEEDED[*]}")"
    note "JDK 17: $(find_java || echo "would download $JDK_FILE")"
    note "Android cmdline-tools: $([ -x "$SDKMANAGER" ] && echo present || echo "would download $CMDTOOLS_FILE")"
    missing=$(missing_sdk_packages | paste -sd' ')
    note "Android SDK packages: $([ -z "$missing" ] && echo present || echo "would install $missing")"
    note "XenonRecomp: $([ -x "$XENON_EXE" ] && echo built || echo "would clone hedge-dev/XenonRecomp at ${XENON_COMMIT:0:12}, patch and build it")"
    for key in sp mp; do
        title $key
        state='not extracted yet'
        if [ -f "$REPO/$DISC/$T_XEX.xex" ]; then state="extracted; $(check_xex 2>&1 || true)"; fi
        if tree_current; then tree=recompiled
        elif [ -f "$REPO/$T_PPC/ppc_func_mapping.cpp" ]; then tree='recompiled with an older recompiler or configuration; would be recompiled'
        else tree='not recompiled'; fi
        note "$T_XEX.xex ($T_LABEL): $state; $tree"
    done
    [ -n "$ISO" ] && note "disc image: $([ -f "$ISO" ] && echo "$ISO ($(( $(stat -c %s "$ISO") / 1048576 )) MB)" || echo "$ISO NOT FOUND")"
    if [ ! -x "$ADB" ]; then found='adb not installed yet'; else found=$(devices | paste -sd,); fi
    note "Android devices (adb): ${found:-none connected}"
    exit 0
fi

printf '\033[32m%s (Xbox 360) recompiled: setup\033[0m\n' "$GAME_NAME"
echo 'This builds the game from your own disc image. What it makes contains code'
echo 'from your copy of the game: keep it to your own devices, never share it.'
echo
if [ ${#NEEDED[@]} -gt 0 ]; then
    echo "These programs are needed and are the system's to install: ${NEEDED[*]}"
    [[ " ${NEEDED[*]} " == *' clang '* ]] && echo '(clang 17 or later: Ubuntu 24.04, Debian 13 and Fedora have one; on an older system, https://apt.llvm.org)'
    if command -v apt-get > /dev/null 2>&1 && ask "Install them now with: sudo apt-get install -y ${NEEDED[*]} ?" y; then
        # Root, as in a container, needs no sudo and often has none.
        as_root=sudo
        [ "$(id -u)" = 0 ] && as_root=
        $as_root apt-get update
        $as_root apt-get install -y "${NEEDED[@]}"
        CLANG=$(find_clang) || die "the clang that apt installed is older than 17. Install a newer one (https://apt.llvm.org), then run setup again."
    else
        die "Install them (sudo apt install ${NEEDED[*]}, or your system's equivalent), then run setup again."
    fi
fi
[ "$FREE_GB" -lt 8 ] && warn "only $FREE_GB GB free here: about 8 GB is wanted."
note "memory: $MEM_GB GB, $AVAILABLE_MB MB of it free now: $JOBS compiler(s) at once on $(nproc) processor(s) (--jobs <n> chooses)"
if [ "$AVAILABLE_MB" -lt 2048 ]; then
    warn 'under 2 GB of memory is free. Close the browser and other programs first; if the build is still killed, add swap (a swap file of 4 GB or more) and run setup again: it carries on where it stopped.'
fi

# ---- 1. what is needed -------------------------------------------------------
APP_PACKAGE=com.$GAME.recomp$SUFFIX
KEYS=(sp mp)
need_source=0
for key in "${KEYS[@]}"; do title $key; [ -f "$REPO/$DISC/$T_XEX.xex" ] || need_source=1; done
if [ -n "$XEX_DIR" ]; then
    [ -d "$XEX_DIR" ] || die "--xex-dir: there is no folder $XEX_DIR"
elif [ "$need_source" = 1 ] || [ "$PUSH_ISO" = 1 ]; then
    while [ -z "$ISO" ] || [ ! -f "$ISO" ]; do
        [ -n "$ISO" ] && warn "no file at $ISO"
        [ "$INTERACTIVE" = 1 ] || die 'Give the disc image with --iso <file>.'
        read -r -p "Path to your $GAME_NAME disc image (.iso): " ISO
        # A path dragged into a terminal comes quoted.
        ISO=${ISO#[\'\"]}; ISO=${ISO%[\'\"]}
    done
fi
[ -n "$ISO" ] && ISO=$(readlink -f "$ISO")
[ -n "$INSTALL_ISO" ] && INSTALL_ISO=$(readlink -f "$INSTALL_ISO")
# Advanced Warfare's install disc holds two content packages, which a console
# copies to its hard disk before the game disc will start; they are read out
# of the image once, into the git-ignored aw/install, and go to the phone as
# they are (docs/AW-BRINGUP.md).
INSTALL_FOLDER=$REPO/aw/install
INSTALL_PACKAGES=(41560914051BF3C0 4156091407406280)
missing_install_packages() {
    local p
    for p in "${INSTALL_PACKAGES[@]}"; do [ -n "$(find "$INSTALL_FOLDER" -type f -name "$p" 2> /dev/null)" ] || echo "$p"; done
}

mkdir -p "$TC" "$DL" "$OUT" "$LOGS"

# The most memory in use at once while this runs, for the last line: what a
# PC with less should expect. Sampled, as no one process holds it.
MEM_PEAK=$OUT/logs/.memory-peak
used_mb() { awk '/^MemTotal:/ { t = $2 } /^MemAvailable:/ { a = $2 } END { print int((t - a) / 1024) }' /proc/meminfo; }
MEM_BEFORE=$(used_mb)
echo "$MEM_BEFORE" > "$MEM_PEAK"
( while sleep 2; do now=$(used_mb); [ "$now" -gt "$(cat "$MEM_PEAK")" ] && echo "$now" > "$MEM_PEAK"; done ) &
SAMPLER=$!
trap 'kill "$SAMPLER" 2> /dev/null || true' EXIT

# ---- 2. tools ----------------------------------------------------------------
step 'tools'
export PYTHONUTF8=1
JAVA_HOME=$(find_java) || {
    fetch "$JDK_FILE" "$JDK_URL" "$JDK_SHA"
    step 'unpack JDK 17'
    rm -rf "$JDK" && mkdir -p "$JDK"
    tar -xzf "$DL/$JDK_FILE" -C "$JDK" --strip-components=1
    JAVA_HOME=$JDK
}
# For this process and what it starts only.
export JAVA_HOME
export GRADLE_USER_HOME=$TC/gradle-home
export ANDROID_USER_HOME=$TC/android-user-home
note "java: $JAVA_HOME"
if [ ! -x "$SDKMANAGER" ]; then
    fetch "$CMDTOOLS_FILE" "$CMDTOOLS_URL" "$CMDTOOLS_SHA"
    step 'unpack Android cmdline-tools'
    mkdir -p "$SDK/cmdline-tools"
    rm -rf "$SDK/cmdline-tools/latest" "$SDK/cmdline-tools/cmdline-tools"
    unzip -q "$DL/$CMDTOOLS_FILE" -d "$SDK/cmdline-tools"
    mv "$SDK/cmdline-tools/cmdline-tools" "$SDK/cmdline-tools/latest"
fi
mapfile -t missing < <(missing_sdk_packages)
if [ ${#missing[@]} -gt 0 ]; then
    ask "The Android SDK and NDK come under Google's licence terms (https://developer.android.com/studio/terms). Accept them?" y ||
        die 'The Android build needs the Android SDK; stopped.'
    step "sdkmanager: ${missing[*]} (about 2 GB)"
    # `yes` ends with a broken pipe once sdkmanager has read enough, which is no failure.
    (yes 2> /dev/null || true) | "$SDKMANAGER" "--sdk_root=$SDK" --licenses > /dev/null 2>&1 || true
    logged sdkmanager.log "$SDKMANAGER" "--sdk_root=$SDK" "${missing[@]}"
fi

# ---- 3. the executables, out of the disc image or the folder ------------------
for key in sp mp; do
    title $key
    rel=$DISC/$T_XEX.xex
    if [ -n "$XEX_DIR" ]; then
        # Under the app's name for it, or the disc's.
        for candidate in "$XEX_DIR/$GAME-$T_XEX.xex" "$XEX_DIR/$T_XEX.xex"; do
            if [ -f "$candidate" ]; then mkdir -p "$REPO/$DISC" && cp -f "$candidate" "$REPO/$rel"; break; fi
        done
        [ -f "$REPO/$rel" ] || die "neither $GAME-$T_XEX.xex nor $T_XEX.xex is in $XEX_DIR"
    elif [ ! -f "$REPO/$rel" ]; then
        step "extract $T_XEX.xex from the disc image (read-only)"
        (cd "$REPO" && logged "extract_$T_XEX.log" python3 tools/xdvdfs.py "$ISO" "$DISC" "$T_XEX.xex")
        [ -f "$REPO/$rel" ] || die "$T_XEX.xex is not on that disc image: is it $GAME_NAME for the Xbox 360?"
    fi
    check_xex || die "$T_XEX.xex is not a disc build this port supports. The app holds the campaign and the multiplayer, and needs both executables to be."
done

if [ -n "$INSTALL_ISO" ] && [ -n "$(missing_install_packages)" ]; then
    step "copy the two content packages from the install disc's image (read-only; 4 GB)"
    (cd "$REPO" && logged extract_install_packages.log python3 tools/xdvdfs.py "$INSTALL_ISO" "$INSTALL_FOLDER" content/0000000000000000/41560914/00000002/)
    missing=$(missing_install_packages | paste -sd' ')
    [ -z "$missing" ] || die "that image has no package $missing: is it $GAME_NAME's install disc?"
fi

# ---- 4. recompile ------------------------------------------------------------
to_recompile=()
for key in "${KEYS[@]}"; do
    title $key
    if [ "$RECOMP" = 1 ] || ! tree_current; then to_recompile+=("$key"); fi
done
# The recompiler itself, when this checkout names another commit of it or
# patches it differently than the one built here: made again from nothing.
if [ ${#to_recompile[@]} -gt 0 ] && [ -f "$XENON/CMakeLists.txt" ]; then
    head=$(git -C "$XENON" rev-parse HEAD 2> /dev/null || true)
    if [ "$head" != "$XENON_COMMIT" ] || ! git -C "$XENON" apply --reverse --check "$PATCH" 2> /dev/null; then
        [ -L "$XENON" ] && die "XenonRecomp/ is a link to a checkout that is not this repository's recompiler ($XENON_COMMIT with patches/xenonrecomp-mw2.patch); update what it points at."
        step 'the recompiler changed with this update: fetching and building it again'
        rm -rf "$XENON"
    fi
fi
if [ ${#to_recompile[@]} -gt 0 ] && ! { [ -x "$XENON_EXE" ] && [ -x "$ANALYSE_EXE" ]; }; then
    if [ ! -f "$XENON/CMakeLists.txt" ]; then
        step 'clone XenonRecomp'
        rm -rf "$XENON"
        # autocrlf off: the patch is LF and must apply to LF files.
        git clone -q -c core.autocrlf=false https://github.com/hedge-dev/XenonRecomp.git "$XENON"
        git -C "$XENON" checkout -q "$XENON_COMMIT"
        git -C "$XENON" submodule update -q --init --recursive
        git -C "$XENON" apply "$PATCH"
    fi
    step "build XenonRecomp ($CLANG)"
    logged xenon_configure.log "$CMAKE" -S "$XENON" -B "$XENON/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_MAKE_PROGRAM=$NINJA" "-DCMAKE_C_COMPILER=${CLANG/++/}" "-DCMAKE_CXX_COMPILER=$CLANG"
    logged xenon_build.log "$CMAKE" --build "$XENON/build" --target XenonAnalyse XenonRecomp -j "$JOBS"
fi

cd "$REPO"
for key in "${KEYS[@]}"; do
    title $key
    rel=$DISC/$T_XEX.xex
    export MW2_TITLE=$T_NAME MW2_VERSION=tu0
    if [[ " ${to_recompile[*]} " == *" $key "* ]]; then
        step "recompile $T_XEX.xex ($T_LABEL); this takes a few minutes"
        logged "${T_NAME}_xexdump.log" python3 tools/xexdump.py "$rel" "$DISC/$T_XEX.pe"
        logged "${T_NAME}_analyse.log" "$ANALYSE_EXE" "$rel" "config/$T_TABLES"
        # Call of Duty 4 and Advanced Warfare have a kind of jump table the
        # analyser does not recognise. Theirs alone: the other games' tables
        # are part of what their game code is checked against, and stay as
        # the analyser writes them.
        case "$GAME" in cod4|aw) logged "${T_NAME}_intext_tables.log" python3 tools/intext_switch_tables.py ;; esac
        # XenonRecomp crashes, rather than complaining, when its output directory is missing.
        rm -rf "$T_PPC" && mkdir "$T_PPC"
        (cd config && logged "${T_NAME}_recomp.log" "$XENON_EXE" "$T_TOML" ../XenonRecomp/XenonUtils/ppc_context.h)
        files=$(find "$T_PPC" -maxdepth 1 -type f | wc -l)
        tables=$(grep -cF '[[switch]]' "config/$T_TABLES" || true)
        bad=$(grep -cE 'Unrecognized instruction|trying to jump outside function' "$LOGS/${T_NAME}_recomp.log" || true)
        note "files: $files (expect $T_FILES)   jump tables: $tables (expect $T_JUMP)   recompiler errors: $bad (expect $T_STEPPED)"
        if [ "$files" != "$T_FILES" ] || [ "$tables" != "$T_JUMP" ] || [ "$bad" != "$T_STEPPED" ]; then
            warn 'the recompiled tree differs from the tested one; the build may fail or misbehave.'
        fi
        step 'generate the shared header'
        logged "${T_NAME}_genshared.log" python3 tools/genshared.py
        # What the tree was made with, for the next run to know whether it
        # still is what this checkout recompiles to (tree_current).
        recompiled_with > "$T_PPC/recompiled_with.txt"
    fi
    # Every run: the runtime implements more imports as it is updated (git
    # pull), and a stub left for one it now has would clash with it at the link.
    logged "${T_NAME}_stubs.log" python3 tools/gen_kernel_stubs.py
done

# ---- 5. the game code's package, and the app ---------------------------------
# What is installed in step 6, in order, a line each: <apk> <package> <data>;
# data says the package has a game folder and saves of its own on the phone.
APKS=()
echo "sdk.dir=$SDK" > android/local.properties
export MW2_LTO=OFF
# A modest daemon: most of the memory goes to the compilers it starts.
GRADLE=(./gradlew --no-daemon '-Dorg.gradle.jvmargs=-Xmx1536m -XX:MaxMetaspaceSize=512m -Dfile.encoding=UTF-8')
[ -n "$SUFFIX" ] && GRADLE+=("-PappSuffix=$SUFFIX")

# A folder for each game: the package takes every library in the folder it
# is given (android/game/build.gradle.kts).
LIBRARIES=$OUT/game-libraries/$GAME/arm64-v8a
mkdir -p "$LIBRARIES"
BUILT=()
for key in "${KEYS[@]}"; do
    title $key
    build=$OUT/build-game-$T_LIB
    step "build the game code: $T_LABEL, $JOBS compiler(s) at once (log: out/logs/game_$T_LIB.log)"
    # Modern Warfare 3's and Call of Duty 4's titles configure only with these two.
    title_args=()
    [ "$GAME" = mw2 ] || title_args=(-DMW3_RECOMP_ONLY=ON -DMW3_LINK_PROBE=ON)
    logged "game_${T_LIB}_configure.log" "$CMAKE" -S "$REPO" -B "$build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$NINJA" \
        "-DCMAKE_TOOLCHAIN_FILE=$SDK/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-O3 "-DMW2_TITLE=$T_NAME" \
        -DMW2_SPLIT=game -DMW2_LTO=OFF "${title_args[@]}"
    logged "game_$T_LIB.log" "$CMAKE" --build "$build" --target ppc_recomp -j "$JOBS"
    cp -f "$build/lib${T_LIB}game.so" "$LIBRARIES/"
    BUILT+=("$(tr -d '\r' < "$build/game_abi.txt" | sed 's/[[:space:]]*$//')")
done
step 'package the game code (gradle :game:assembleDebug)'
(cd android && logged gradle_game.log "${GRADLE[@]}" "-Pgame=$GAME" "-PgameLibraries=$(dirname "$LIBRARIES")" :game:assembleDebug)
GAME_APK=$OUT/$GAME-game$SUFFIX.apk
cp -f android/game/build/outputs/apk/debug/game-debug.apk "$GAME_APK"
APKS+=("$GAME_APK $APP_PACKAGE.game 0")
note "$GAME_APK ($(( $(stat -c %s "$GAME_APK") / 1048576 )) MB; built against $(IFS=,; echo "${BUILT[*]}"))"

# The latest release's assets, a line each: <name> <size> <url>, after one
# with its tag; nothing when there is no release or no network.
# With GitHub's token when there is one (the builder workflow's): a shared
# runner's address is often over the limit for anonymous questions.
RELEASE=$(curl -fsSL --retry 3 ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} "https://api.github.com/repos/$RELEASE_REPO/releases/latest" 2> /dev/null | python3 -c '
import json, sys
release = json.load(sys.stdin)
print(release["tag_name"])
for asset in release["assets"]:
    print(asset["name"], asset["size"], asset["browser_download_url"])' 2> /dev/null || true)
RELEASE_TAG=$(sed -n 1p <<< "$RELEASE")
# What the published app was built against, published beside it: a game
# library built from another state of this folder is one the app will
# refuse, and better said here than there.
abi_url=$(awk -v name="$GAME-recomp-$RELEASE_TAG-game-abi.txt" '$1 == name { print $3 }' <<< "$RELEASE")
if [ -n "$abi_url" ] && wanted=$(curl -fsSL --retry 3 "$abi_url" | tr -d '\r' | sed 's/[[:space:]]*$//'); then
    for line in "${BUILT[@]}"; do
        grep -qxF "$line" <<< "$wanted" && continue
        warn "the game code just built ($line) is not what the published app $RELEASE_TAG was built for (this folder is newer or older than that release). The app will say so and not start the game. Either wait for the next release, or build the game code of that one: git checkout $RELEASE_TAG, then bash setup/setup.sh --game $GAME --game-only."
        ABI_DIFFERS=1
    done
    [ -z "${ABI_DIFFERS:-}" ] && note "it fits the published app $RELEASE_TAG."
fi

BUILD_APP=$DEV
if [ "$GAME_ONLY" = 0 ] && [ "$BUILD_APP" = 0 ]; then
    step "the published app (github.com/$RELEASE_REPO, latest release)"
    read -r app_name app_size app_url < <(awk -v name="$GAME-recomp-$RELEASE_TAG.apk" '$1 == name' <<< "$RELEASE") || true
    if [ -n "${app_url:-}" ]; then
        APP_APK=$OUT/$app_name
        if [ ! -f "$APP_APK" ] || [ "$(stat -c %s "$APP_APK")" != "$app_size" ]; then
            curl -fL --retry 3 --progress-bar -o "$APP_APK.part" "$app_url"
            mv -f "$APP_APK.part" "$APP_APK"
        fi
        APKS+=("$APP_APK $APP_PACKAGE 1")
        note "$APP_APK ($RELEASE_TAG)"
    else
        warn "no published app could be downloaded (no release yet, or no network): building the app here instead. It is signed with this PC's key, so it will not update itself; run setup again once there is a release."
        BUILD_APP=1
    fi
fi
if [ "$BUILD_APP" = 1 ]; then
    step 'libadrenotools, the bundled Turnip driver and the camouflage pack (android/fetch_deps.sh)'
    logged fetch_deps.log bash android/fetch_deps.sh
    case "$GAME" in mw2) variant=mw2Tu0 ;; cod4) variant=cod4Tu0 ;; aw) variant=awTu0 ;; *) variant=mw3Tu0 ;; esac
    step "build the app, without the game code (gradle assemble${variant^}Debug; log: out/logs/gradle_$variant.log)"
    (cd android && logged "gradle_$variant.log" "${GRADLE[@]}" ":app:assemble${variant^}Debug")
    made=$(ls -t "android/app/build/outputs/apk/$variant/debug/"*.apk 2> /dev/null | head -n 1)
    [ -n "$made" ] || die "gradle finished but there is no APK under android/app/build/outputs/apk/$variant/debug"
    APP_APK=$OUT/$GAME$SUFFIX-debug.apk
    cp -f "$made" "$APP_APK"
    APKS+=("$APP_APK $APP_PACKAGE 1")
    note "$APP_APK ($(( $(stat -c %s "$APP_APK") / 1048576 )) MB)"
fi

# ---- 6. onto the phone -------------------------------------------------------
# Installs one package. Android replaces an app only with one signed by the
# same key, and the copies of this one are signed three ways: the published
# app with the project's release key, an app built here with this PC's debug
# key, one built elsewhere with that machine's. Going from one to another
# means uninstalling, which takes the app's folder on the phone with it: the
# game files and the saves. So that folder is moved aside first and back
# afterwards (a rename, not a copy), with the player's leave. The game
# code's package holds nothing of the player's and is simply replaced. Never
# fatal: the build is done by now, and what is wrong is said.
install_package() {   # install_package <serial> <apk> <package> <data 0|1>
    local serial=$1 apk=$2 package=$3 data=$4 text name
    name=$(basename "$apk")
    step "adb install $name"
    text=$("$ADB" -s "$serial" install -r "$apk" 2>&1) && return 0
    # Another key, or (a build made here over the published app) an older version number.
    if ! grep -qE 'INSTALL_FAILED_UPDATE_INCOMPATIBLE|INSTALL_FAILED_VERSION_DOWNGRADE' <<< "$text"; then
        warn "could not install $name on $serial: $text"; return 0
    fi
    if [ "$data" = 0 ]; then
        "$ADB" -s "$serial" uninstall "$package" > /dev/null 2>&1 || true
        text=$("$ADB" -s "$serial" install -r "$apk" 2>&1) || warn "could not install $name on $serial: $text"
        return 0
    fi
    warn "the app on the phone ($package) is not one Android will put $name over: it is signed with another key (one was built on a PC, the other is the published app) or is a newer version."
    if ! ask 'Replace the app on the phone? Its game files and saves are kept; its settings (graphics driver, controls, graphics) start afresh.' y; then
        note "left as it is; $apk is not installed."; return 0
    fi
    local files=/sdcard/Android/data/$package
    # Beside the app's own folder, not in /sdcard itself: Android keeps
    # Android/data apart from the rest of the shared storage, so a move out
    # of it is a copy of every game file (6 to 8 GB, minutes), and one
    # within it a rename. Uninstalling takes the package's own folder alone.
    local keep=/sdcard/Android/data/mw3-recomp-keep-$package moved back
    moved=$("$ADB" -s "$serial" shell "if [ -e $keep ]; then echo busy; elif [ -d $files/files ]; then mv $files/files $keep && echo moved; else echo none; fi" 2>&1 || true)
    if ! grep -qE 'moved|none' <<< "$moved"; then
        warn "$keep is on the phone from a replacement that did not finish. Move it back to $files/files (or delete it), then run setup again."; return 0
    fi
    "$ADB" -s "$serial" uninstall "$package" > /dev/null 2>&1 || true
    text=$("$ADB" -s "$serial" install -r "$apk" 2>&1) || warn "could not install $name on $serial: $text"
    if grep -q moved <<< "$moved"; then
        back=$("$ADB" -s "$serial" shell "mkdir -p $files && mv $keep $files/files && echo back" 2>&1 || true)
        if grep -q back <<< "$back"; then note "the game files and saves are back in the app's folder."
        else warn "the game files and saves could not be moved back: they are in $keep on the phone; move that folder to $files/files."; fi
    fi
}

mapfile -t found < <(devices)
ready=()
for line in "${found[@]}"; do
    case "$line" in
        *' unauthorized') warn 'a phone is connected but has not allowed USB debugging: accept the prompt on its screen.' ;;
        *) ready+=("${line% *}") ;;
    esac
done
serial=
[ ${#ready[@]} = 0 ] || [ "$NO_INSTALL" = 1 ] || serial=$(install_device "${ready[@]}")
if [ "$NO_INSTALL" = 1 ]; then
    note 'Nothing is installed (--no-install): the APKs are in out/.'
elif [ ${#ready[@]} = 0 ]; then
    note 'No phone connected with USB debugging on; install the APKs later (copy them to the phone and open each, or run setup again with it connected).'
elif [ -z "$serial" ]; then
    # The command this run was, with the choice it lacked: what is built is
    # skipped the second time, so it goes straight to the phone.
    again=(); skip=0
    for arg in "${ARGS[@]}"; do
        if [ "$skip" = 1 ]; then skip=0; continue; fi
        case "$arg" in
            --install) ;;
            --device|-Device) skip=1 ;;
            *) again+=("$(printf '%q' "$arg")") ;;
        esac
    done
    warn "the build is ready and was NOT installed: $(if [ -n "$DEVICE" ]; then echo "no phone with the serial $DEVICE is connected"; else echo 'this run asks nothing and no phone was named'; fi). To install it, run$([ ${#ready[@]} -gt 1 ] && echo ' one of'):"
    for candidate in "${ready[@]}"; do note "bash setup/setup.sh ${again[*]} --install --device $candidate"; done
else
    if [ "$INSTALL" = 1 ] || ask "Install on $serial now?" y; then
        for item in "${APKS[@]}"; do
            read -r apk package data <<< "$item"
            install_package "$serial" "$apk" "$package" "$data"
        done
        # Advanced Warfare's install disc: its packages go beside the game
        # folder, where the runtime looks for a title's installed content
        # (runtime/kernel/content.cpp). Files of the same size are skipped.
        if [ "$GAME" = aw ] && [ "$GAME_ONLY" = 0 ] && [ -z "$(missing_install_packages)" ]; then
            step "adb push the install disc's packages (4 GB) to the app's dlc folder"
            dlc=/sdcard/Android/data/$APP_PACKAGE/files/dlc
            "$ADB" -s "$serial" shell "mkdir -p $dlc"
            # adb reads its standard input to the end, and in this loop that
            # is the list of packages: the first one's adb took the second's
            # name and one package of the two reached the phone.
            while IFS= read -r package; do
                there=$("$ADB" -s "$serial" shell "stat -c %s $dlc/$(basename "$package") 2>/dev/null || echo 0" < /dev/null | tr -d '\r')
                [ "$there" = "$(stat -c %s "$package")" ] || "$ADB" -s "$serial" push "$package" "$dlc/$(basename "$package")" < /dev/null > /dev/null
            done < <(find "$INSTALL_FOLDER" -type f)
        fi
        if [ -n "$ISO" ] && { [ "$PUSH_ISO" = 1 ] || ask "Copy the disc image to the phone's Download folder for the app's installer (7-8 GB, several minutes)?" n; }; then
            step 'adb push the disc image to /sdcard/Download/'
            "$ADB" -s "$serial" push "$ISO" /sdcard/Download/
        fi
    fi
fi

# ---- 7. done -----------------------------------------------------------------
took=$(( $(date +%s) - START ))
step "done in $((took / 3600)) h $((took / 60 % 60)) min; at its fullest the PC had $(( $(cat "$MEM_PEAK") )) MB of memory in use ($MEM_BEFORE MB before it began)"
for item in "${APKS[@]}"; do note "APK: ${item%% *}"; done
if [ "$GAME_ONLY" = 1 ]; then
    note 'The game code is built for this state of the folder. If the app on the phone still refuses it, the app is older or newer than this folder: update the app (it offers to) or the folder (git pull).'
else
    if [ "$GAME" = aw ]; then
        note "On the phone: open the app, \"Install the game files\", choose the game disc's image$([ -z "$(missing_install_packages)" ] || echo ", then the install disc's when it asks for the second disc"). See $GUIDE."
    else
        note "On the phone: open the app, \"Install the game files\", choose the disc image. See $GUIDE."
    fi
fi
