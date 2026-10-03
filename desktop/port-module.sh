#!/usr/bin/env bash
#
# Build a Schwung module repo for the DESKTOP, by running its own build.sh.
#
#   desktop/port-module.sh <repo-dir> [<repo-dir> ...]
#   desktop/port-module.sh --all
#
# NOTHING IN THE MODULE REPO IS EDITED. Every module's build.sh already reads
#
#     CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
#     ${CROSS_PREFIX}g++ -O3 -shared -fPIC ... -o build/dsp.so
#
# so the whole port is: point CROSS_PREFIX at a pair of shim compilers that
# translate the handful of GNU-only flags to their clang/ld64 spellings, and
# let the repo build itself. That is why this is one script rather than 100
# patches -- and why a module that gains a source file keeps working with no
# change here.
#
# CROSS_PREFIX IS "host-", NOT EMPTY. Every build.sh tests `[ -z "$CROSS_PREFIX" ]`
# to decide whether to re-enter Docker, so an empty value sends the build into
# an aarch64 container and produces a Linux .so that looks like success.
#
# The output keeps the name dsp.so even though it is a Mach-O: the chain host
# builds sub-module paths with a ".so" suffix and dlopen on macOS does not care
# what a file is called. See desktop/README.md.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$ROOT/build/desktop"
MODULES="$OUT/modules"
SHIM="$OUT/toolchain"
SIBLINGS="${SCHWUNG_MODULE_REPOS:-$(cd "$(dirname "$(git rev-parse --git-common-dir)")/.." && pwd)}"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "port-module.sh: macOS only for now" >&2
    exit 1
fi

# ---------------------------------------------------------------- shim ----
# Translate the GNU flags that ld64 and clang spell differently, and DROP the
# ones that have no meaning here. Dropping is safe for all of these: they are
# hardening and symbol-visibility options, not behaviour.
#
#   -shared                 -> -dynamiclib
#   -Wl,--no-undefined      -> -Wl,-undefined,error      (same guarantee)
#   -Wl,--exclude-libs,...  -> dropped; ld64 has no equivalent
#   -Wl,--version-script=.. -> dropped; would need an -exported_symbols_list
#   -Wl,-soname,...         -> dropped; install_name is not needed for dlopen
#   -fno-gnu-unique         -> dropped; clang has no STB_GNU_UNIQUE to suppress
#   -Wl,--gc-sections       -> -Wl,-dead_strip
#   -static-libgcc/stdc++   -> dropped; not a thing on macOS
mkdir -p "$SHIM"
cat > "$SHIM/host-cc-impl" <<'SHIMEOF'
#!/usr/bin/env bash
real="$1"; shift
args=()
for a in "$@"; do
    case "$a" in
        -shared)                  args+=(-dynamiclib) ;;
        -Wl,--no-undefined)       args+=(-Wl,-undefined,error) ;;
        -Wl,--gc-sections)        args+=(-Wl,-dead_strip) ;;
        -Wl,--exclude-libs,*)     ;;
        -Wl,--version-script=*)   ;;
        -Wl,-soname*)             ;;
        -Wl,--as-needed)          ;;
        -Wl,-z,*)                 ;;
        -fno-gnu-unique)          ;;
        -static-libgcc)           ;;
        -static-libstdc++)        ;;
        -Wl,-rpath,\$ORIGIN*)     ;;
        *)                        args+=("$a") ;;
    esac
done
exec "$real" "${args[@]}"
SHIMEOF
chmod +x "$SHIM/host-cc-impl"

printf '#!/usr/bin/env bash\nexec "%s/host-cc-impl" /usr/bin/clang "$@"\n'   "$SHIM" > "$SHIM/host-gcc"
printf '#!/usr/bin/env bash\nexec "%s/host-cc-impl" /usr/bin/clang++ "$@"\n' "$SHIM" > "$SHIM/host-g++"
printf '#!/usr/bin/env bash\nexec /usr/bin/ar "$@"\n'                        "$SHIM" > "$SHIM/host-ar"
printf '#!/usr/bin/env bash\nexec /usr/bin/strip "$@"\n'                     "$SHIM" > "$SHIM/host-strip"
chmod +x "$SHIM"/host-*

# ---------------------------------------------------------------- build ---
port_one() {
    local src="$1"
    local name; name=$(basename "$src")

    [ -x "$src/scripts/build.sh" ] || { echo "SKIP  $name  (no scripts/build.sh)"; return 1; }

    local log="$OUT/logs/$name.log"
    mkdir -p "$OUT/logs"

    # BUILD IN A COPY, NEVER IN THE MODULE REPO.
    #
    # These build.sh scripts write objects to build/ and artifacts to dist/,
    # and a desktop run would leave Mach-O files exactly where the device
    # build leaves ELFs -- including dist/<id>/dsp.so, which is the file that
    # gets packaged and released. A repo left in that state ships a macOS
    # dylib to a Move. dist/ and build/ are excluded from the copy so that
    # whatever is in dist/ afterwards is from THIS run: several repos publish
    # more than one module, and some carry stale artifacts from months ago.
    local repo="$OUT/src/$name"
    rm -rf "$repo"
    mkdir -p "$(dirname "$repo")"
    rsync -a --exclude '.git' --exclude 'dist' --exclude 'build' "$src/" "$repo/"

    if ! ( cd "$repo" && \
           PATH="$SHIM:$PATH" CROSS_PREFIX="host-" \
           ./scripts/build.sh ) > "$log" 2>&1
    then
        echo "FAIL  $name  ($(sed -n 's/.*error: //p' "$log" | head -1 | cut -c1-70))"
        return 1
    fi

    # Check it is actually a Mach-O -- a build.sh that quietly fell through to
    # Docker would leave an ELF here and report success.
    local produced=0
    for d in "$repo"/dist/*/; do
        [ -d "$d" ] || continue
        local id; id=$(basename "$d")
        # WHICH SUBTREE A MODULE BELONGS IN COMES FROM THE CATALOG, NOT FROM
        # module.json. Plenty of modules -- cloudseed, mverb, psxverb, gate,
        # ducker, filter, midiverb -- declare no component_type of their own,
        # and schwung-manager files them on the device by the catalog's value.
        # Guessing here puts an audio FX under sound_generators/, where the
        # chain host's "%s/../audio_fx/%s/%s.so" will never find it: the module
        # is installed, looks installed, and simply cannot be picked.
        local kind; kind=$(python3 -c "
import json, sys
mid = sys.argv[1]
for path in sys.argv[2:]:
    try:
        d = json.load(open(path))
    except Exception:
        continue
    if 'modules' in d:                       # the catalog
        for m in d['modules']:
            if m.get('id') == mid and m.get('component_type'):
                print(m['component_type']); sys.exit(0)
    elif d.get('component_type'):            # the module's own manifest
        print(d['component_type']); sys.exit(0)
print('sound_generator')
" "$id" "$ROOT/module-catalog.json" "$d/module.json" "$repo/src/module.json" \
        2>/dev/null || echo sound_generator)

        # Only the three CHAIN component kinds have a home here. A tool,
        # overtake or utility module is not something a chain slot can load,
        # and filing one under sound_generators/ would put it in the plugin's
        # synth picker where choosing it can only fail.
        local dest
        case "$kind" in
            sound_generator) dest="$MODULES/sound_generators/$id" ;;
            audio_fx)        dest="$MODULES/audio_fx/$id" ;;
            midi_fx)         dest="$MODULES/midi_fx/$id" ;;
            *)               echo "skip  $id  (component_type $kind is not chain-loadable)"; continue ;;
        esac

        local bin=""
        for cand in "$d/dsp.so" "$d/$id.so"; do
            [ -f "$cand" ] && bin="$cand" && break
        done
        [ -n "$bin" ] || continue

        if ! file "$bin" | grep -q "Mach-O"; then
            echo "FAIL  $name  (built an ELF -- build.sh fell through to Docker)"
            return 1
        fi

        mkdir -p "$dest"
        rsync -a --exclude '*.dSYM' "$d" "$dest/"
        produced=1
        echo "ok    $name -> ${dest#"$MODULES/"}"
    done

    [ "$produced" = 1 ] || { echo "FAIL  $name  (built nothing into dist/)"; return 1; }
    return 0
}

targets=()
if [ "${1:-}" = "--all" ]; then
    for d in "$SIBLINGS"/schwung-* "$SIBLINGS"/move-anything-*; do
        [ -d "$d/src/dsp" ] && [ -x "$d/scripts/build.sh" ] && targets+=("$d")
    done
else
    for a in "$@"; do
        if [ -d "$a" ]; then targets+=("$a")
        elif [ -d "$SIBLINGS/schwung-$a" ]; then targets+=("$SIBLINGS/schwung-$a")
        elif [ -d "$SIBLINGS/$a" ]; then targets+=("$SIBLINGS/$a")
        else echo "SKIP  $a  (no such repo)"; fi
    done
fi

[ ${#targets[@]} -gt 0 ] || { echo "nothing to build"; exit 1; }

okc=0; failc=0
for t in "${targets[@]}"; do
    if port_one "$t"; then okc=$((okc+1)); else failc=$((failc+1)); fi
done

echo
echo "ported $okc, failed $failc   (logs in ${OUT#"$ROOT/"}/logs/)"
