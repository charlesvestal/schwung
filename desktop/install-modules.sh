#!/usr/bin/env bash
#
# File every ported module into the desktop module tree and install it.
#
#   desktop/install-modules.sh [dest]
#
# dest defaults to the directory the plugin looks in:
#   ~/Library/Application Support/Schwung/modules
#
# Placement comes from module-catalog.json, NOT from the module's own
# module.json -- plenty of modules (cloudseed, mverb, psxverb, gate, ducker,
# filter, midiverb) declare no component_type at all, and schwung-manager files
# them on the device by the catalog's value. Guessing puts an audio FX under
# sound_generators/, where the chain host's "%s/../audio_fx/%s/%s.so" can never
# find it: installed, looks installed, and unpickable.
#
# Safe to run while port-module.sh is still going: it only copies a module
# whose binary is already a complete Mach-O, and it is idempotent.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$ROOT/build/desktop"
MODULES="$OUT/modules"
DEST="${1:-$HOME/Library/Application Support/Schwung/modules}"

[ -d "$OUT/src" ] || { echo "nothing ported yet (no $OUT/src)"; exit 1; }

kind_of() {
    python3 -c "
import json, sys
mid = sys.argv[1]
for path in sys.argv[2:]:
    try:
        d = json.load(open(path))
    except Exception:
        continue
    if 'modules' in d:
        for m in d['modules']:
            if m.get('id') == mid and m.get('component_type'):
                print(m['component_type']); sys.exit(0)
    elif d.get('component_type'):
        print(d['component_type']); sys.exit(0)
print('sound_generator')
" "$@" 2>/dev/null || echo sound_generator
}

filed=0; skipped=0
for d in "$OUT"/src/*/dist/*/; do
    [ -d "$d" ] || continue
    id=$(basename "$d")

    bin=""
    for cand in "$d/dsp.so" "$d/$id.so"; do
        [ -f "$cand" ] && bin="$cand" && break
    done
    [ -n "$bin" ] || continue
    file "$bin" | grep -q "Mach-O" || { echo "skip  $id (not a Mach-O)"; skipped=$((skipped+1)); continue; }

    kind=$(kind_of "$id" "$ROOT/module-catalog.json" "$d/module.json" )

    case "$kind" in
        sound_generator) sub="sound_generators" ;;
        audio_fx)        sub="audio_fx" ;;
        midi_fx)         sub="midi_fx" ;;
        *)               echo "skip  $id ($kind is not chain-loadable)"; skipped=$((skipped+1)); continue ;;
    esac

    # Remove any WRONG placement of this same id left by an earlier run, or the
    # tree ends up with the module in two subtrees and the stale copy is the
    # one a picker may show.
    for other in sound_generators audio_fx midi_fx; do
        [ "$other" = "$sub" ] && continue
        [ -d "$MODULES/$other/$id" ] && rm -rf "${MODULES:?}/$other/$id" && echo "moved $id out of $other/"
    done

    mkdir -p "$MODULES/$sub/$id"
    rsync -a --exclude '*.dSYM' "$d" "$MODULES/$sub/$id/"
    filed=$((filed+1))
done

echo
echo "filed $filed module(s), skipped $skipped"

# ---- reconcile the WHOLE tree, not just what was filed this pass ----------
#
# Filing only corrects modules whose dist/ is present right now. Anything an
# earlier run misfiled -- or filed that should not be here at all -- stays put
# otherwise, and a tool or overtake module left under sound_generators/ shows
# up in the plugin's synth picker where choosing it can only fail. So the tree
# itself is checked against the catalog, independently of what was just built.
pruned=0; moved=0
for sub in sound_generators audio_fx midi_fx; do
    for dir in "$MODULES/$sub"/*/; do
        [ -d "$dir" ] || continue
        id=$(basename "$dir")
        [ "$id" = "chain" ] && continue          # never the chain host itself

        kind=$(kind_of "$id" "$ROOT/module-catalog.json" "$dir/module.json")
        case "$kind" in
            sound_generator) want="sound_generators" ;;
            audio_fx)        want="audio_fx" ;;
            midi_fx)         want="midi_fx" ;;
            *)  rm -rf "${dir%/}"
                echo "prune $id ($kind) from $sub/"
                pruned=$((pruned+1)); continue ;;
        esac

        if [ "$want" != "$sub" ]; then
            mkdir -p "$MODULES/$want"
            rm -rf "${MODULES:?}/$want/$id"
            mv "${dir%/}" "$MODULES/$want/$id"
            echo "move  $id  $sub/ -> $want/"
            moved=$((moved+1))
        fi
    done
done
[ "$pruned$moved" = "00" ] || echo "reconciled: $moved moved, $pruned pruned"

mkdir -p "$DEST"
rsync -a --delete-excluded --exclude '*.dSYM' "$MODULES/" "$DEST/"

echo "installed to: $DEST"
echo
for sub in sound_generators audio_fx midi_fx; do
    n=$(find "$DEST/$sub" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ')
    [ "$n" = "0" ] && continue
    echo "$sub ($n):"
    find "$DEST/$sub" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | sort | paste -sd' ' - | fold -sw 76 | sed 's/^/  /'
done
