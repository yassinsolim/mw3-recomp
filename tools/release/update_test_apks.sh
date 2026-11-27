#!/usr/bin/env bash
# The apps of gate 6 (docs/android-releases.md, "The update as a player does
# it"): a commit's release variant under a package suffix, signed with the
# release key, so that it can stand beside the owner's app on a device.
#
#   tools/release/update_test_apks.sh <worktree> <outdir> <task:versioncode:versionname:apk>...
#
#   update_test_apks.sh ../old out assembleAwTu0Release:103:0.1.3:aw-old.apk
#
# <worktree> is a checkout of the commit the published release was tagged
# from (or of the draft's), with XenonRecomp/, third_party/libadrenotools and
# the Turnip driver in it (android/fetch_deps.sh). RELEASE_WORK is the folder
# docs/android-releases.md names: keys/mw3-release.properties and the SDK and
# JDK in mw3-android/.toolchain are taken from it. PACKAGE_SUFFIX is .ug unless
# set. The environment is the release workflows' (.github/workflows), which is
# why Modern Warfare 3 gets its disc's executable hash: without it the app
# refuses the player's own disc, as it did the first time this was tried.
set -euo pipefail
worktree=$1 out=$2
shift 2
: "${RELEASE_WORK:?RELEASE_WORK has to name the folder the release tools use}"
keys="$RELEASE_WORK/keys/mw3-release.properties"
# Read as text, not sourced: the keystore path has backslashes.
while IFS='=' read -r name value; do
    case "$name" in MW2_*) export "$name=${value%$'\r'}" ;; esac
done < "$keys"
toolchain="$RELEASE_WORK/mw3-android/.toolchain"
export JAVA_HOME="$toolchain/jdk17" ANDROID_HOME="$toolchain/android-sdk" ANDROID_SDK_ROOT="$toolchain/android-sdk"
export GRADLE_USER_HOME="$toolchain/gradle-home"
export MW2_LTO=OFF MW3_XEX_SHA256=e94de720373d668a2bd592dfc74339bfbbc98cca993176fcd055b1449633bcd1
export MW2_BUILD_STAMP="$(date -u +%Y-%m-%d)" MW2_BUILD_SHA="$(git -C "$worktree" rev-parse --short=7 HEAD)"
mkdir -p "$out"
cd "$worktree/android"
for spec in "$@"; do
    IFS=: read -r task code name apk <<< "$spec"
    MW2_VERSION_CODE=$code MW2_VERSION_NAME=$name ./gradlew --no-daemon "-PappSuffix=${PACKAGE_SUFFIX:-.ug}" ":app:$task" --offline -q
    flavour=${task#assemble}
    flavour=${flavour%Release}
    flavour=$(tr '[:upper:]' '[:lower:]' <<< "${flavour:0:1}")${flavour:1}
    cp app/build/outputs/apk/"$flavour"/release/*.apk "$out/$apk"
    echo "built $out/$apk"
done
