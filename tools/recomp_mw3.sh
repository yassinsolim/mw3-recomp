#!/bin/bash
# MW3: flat image -> jump tables -> recompiled C++ (docs/MW3-BRINGUP.md, steps 1
# and 3). build.sh is MW2's pipeline and has no MW3 title yet; this runs the
# same XenonRecomp steps for one of MW3's executables.
#   tools/recomp_mw3.sh        the campaign (default.xex, config/MW3.toml, ppc_mw3/)
#   tools/recomp_mw3.sh mp     the multiplayer (default_mp.xex, config/MW3MP.toml,
#                              ppc_mw3_mp/; docs/MW3-MP-BRINGUP.md)
#   Input: the executable from your disc, in mw3/tu0/, and its config.
#   Output: the flat image beside it, the switch tables in config/, the tree.
#   Prereqs: cmake ninja-build clang-18 python3 git (as build.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
case "${1:-sp}" in
    sp) NAME=default;    TOML=MW3.toml;   TABLES=mw3_switch_tables.toml;   PPC=ppc_mw3 ;;
    mp) NAME=default_mp; TOML=MW3MP.toml; TABLES=mw3mp_switch_tables.toml; PPC=ppc_mw3_mp ;;
    *)  echo "usage: $0 [sp|mp]"; exit 1 ;;
esac
XEX=mw3/tu0/$NAME.xex
LOG=/tmp/${PPC}_recomp.log
python3 tools/check_mw3_xex.py "$XEX"
[ -f "config/$TOML" ] || { echo "config/$TOML is missing"; exit 1; }

XENON_COMMIT=ddd128bcca99fe8bfbb99bea583c972351fa6ace   # as build.sh
if [ ! -d XenonRecomp ]; then
    git clone --recursive https://github.com/hedge-dev/XenonRecomp.git
    git -C XenonRecomp checkout -q "$XENON_COMMIT"
    git -C XenonRecomp submodule update --init --recursive
    git -C XenonRecomp apply "$ROOT/patches/xenonrecomp-mw2.patch"
fi
cmake -B XenonRecomp/build -S XenonRecomp -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 >/dev/null
cmake --build XenonRecomp/build -j"${JOBS:-$(nproc)}" >/dev/null

python3 tools/xexdump.py "$XEX" "mw3/tu0/$NAME.pe"
./XenonRecomp/build/XenonAnalyse/XenonAnalyse "$XEX" "config/$TABLES" >/dev/null
# XenonRecomp crashes, rather than complaining, when its output directory is missing.
rm -rf "$PPC" && mkdir -p "$PPC"
( cd config && ../XenonRecomp/build/XenonRecomp/XenonRecomp "$TOML" ../XenonRecomp/XenonUtils/ppc_context.h ) > "$LOG"
echo "switch-case errors: $(grep -c 'trying to jump outside function' "$LOG" || true)"
echo "unrecognized instructions: $(grep -c 'Unrecognized instruction' "$LOG" || true)"
echo "files: $(ls "$PPC" | wc -l)  (log: $LOG)"
