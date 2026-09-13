#!/usr/bin/env bash
#
# Build the desktop Schwung host and a module tree for it, natively.
#
#   desktop/build.sh                 -> build/desktop/{schwung-render,modules/}
#
# The module tree MIRRORS THE DEVICE LAYOUT exactly:
#
#   modules/chain/dsp.so
#   modules/sound_generators/<id>/dsp.so
#   modules/audio_fx/<id>/<id>.so
#
# because the chain host builds every sub-module path relative to its own
# module_dir and those paths are written with a ".so" suffix in its source.
# dlopen on macOS does not care what a Mach-O file is called, so the dylibs
# are simply named .so rather than teaching chain_host.c and chain_bus.c a
# platform suffix. The two platforms never share an install tree.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$ROOT/build/desktop"
MODULES="$OUT/modules"
# Where the module repos live (schwung-braids, schwung-dx7, ...). Derived from
# git rather than by counting "..": in a worktree $ROOT is
# schwung/.claude/worktrees/<name>, which is three levels deeper than a normal
# checkout, and a hardcoded count silently resolves to the wrong directory and
# reports every module as "not checked out".
SIBLINGS="${SCHWUNG_MODULE_REPOS:-$(cd "$(dirname "$(git rev-parse --git-common-dir)")/.." && pwd)}"

case "$(uname -s)" in
    Darwin) SHARED="-dynamiclib" ;;
    *)      SHARED="-shared -fPIC" ;;
esac

mkdir -p "$MODULES/chain" "$MODULES/sound_generators" "$MODULES/audio_fx"

echo "=== chain host ==="
# shellcheck disable=SC2086
cc -g -O2 $SHARED \
    src/modules/chain/dsp/chain_host.c \
    src/modules/chain/dsp/chain_json.c \
    src/modules/chain/dsp/chain_params.c \
    src/modules/chain/dsp/chain_mod.c \
    src/modules/chain/dsp/chain_midi.c \
    src/modules/chain/dsp/chain_patch.c \
    src/modules/chain/dsp/chain_reorder.c \
    src/modules/chain/dsp/chain_bus.c \
    src/host/unified_log.c \
    -o "$MODULES/chain/dsp.so" \
    -Isrc -lm -lpthread
cp src/modules/chain/module.json "$MODULES/chain/" 2>/dev/null || true

# ---------------------------------------------------------------- freeverb --
# Bundled, and the only bundled audio FX -- so it is the one FX that needs no
# sibling repo checked out.
echo "=== freeverb ==="
mkdir -p "$MODULES/audio_fx/freeverb"
# shellcheck disable=SC2086
cc -g -O2 $SHARED \
    src/modules/audio_fx/freeverb/freeverb.c \
    -o "$MODULES/audio_fx/freeverb/freeverb.so" \
    -Isrc -lm
cp src/modules/audio_fx/freeverb/module.json "$MODULES/audio_fx/freeverb/" 2>/dev/null || true

# ------------------------------------------------------------------ braids --
# First external module ported. Pure C++14, no submodules, and its build.sh
# already honours CROSS_PREFIX -- which is what makes a native build a flag
# change rather than a port.
BRAIDS="$SIBLINGS/schwung-braids"
if [ -d "$BRAIDS" ]; then
    echo "=== braids ==="
    mkdir -p "$MODULES/sound_generators/braids" "$OUT/obj/braids"
    for src in braids/macro_oscillator.cc braids/analog_oscillator.cc \
               braids/digital_oscillator.cc braids/resources.cc \
               braids/quantizer.cc stmlib/utils/random.cc braids_plugin.cpp; do
        obj="$OUT/obj/braids/$(basename "${src%.*}").o"
        c++ -O3 -fPIC -std=c++14 -DTEST -I"$BRAIDS/src/dsp" -c "$BRAIDS/src/dsp/$src" -o "$obj"
    done
    # shellcheck disable=SC2086
    c++ $SHARED -o "$MODULES/sound_generators/braids/dsp.so" "$OUT/obj/braids"/*.o -lm
    cp "$BRAIDS/src/module.json" "$MODULES/sound_generators/braids/" 2>/dev/null || true
else
    echo "=== braids: SKIPPED ($BRAIDS not checked out) ==="
fi

echo "=== render CLI ==="
cc -g -O2 -std=c11 \
    desktop/src/render_cli.c desktop/src/schwung_desktop.c \
    -o "$OUT/schwung-render" \
    -Isrc -Idesktop/src -lm

echo
echo "built:"
find "$OUT" -name '*.so' -o -name 'schwung-render' | sed "s|$ROOT/||" | sort
