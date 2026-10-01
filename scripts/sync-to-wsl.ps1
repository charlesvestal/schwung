# Push the current Windows working tree to a WSL checkout that can build it.
#
# The build runs in WSL (Docker cross-compile), so the tree has to live on the
# Linux side. Three things this handles that a plain copy does not:
#
#   - SUBMODULES STAY PUT. libs/openevv and libs/link are excluded from the
#     sync and from its --delete, because they are large, they do not change
#     with the working tree, and build.sh HARD-FAILS without either one. If the
#     destination has no copy yet, one is seeded from a known-good checkout.
#     They must be real directories, never symlinks: build.sh runs inside
#     Docker, which mounts only the repo directory, so a link out of the tree
#     dangles in the container and reads as "submodule absent".
#
#   - LINE ENDINGS. core.autocrlf=true on Windows, so most of the tree is CRLF
#     on disk while the shell scripts are LF. CRLF in a .sh is a "bad
#     interpreter" failure inside Docker, so text files are normalised to LF
#     after the copy. libs/ is skipped (already LF, and 180 MB of it).
#
#   - THE BUILD DIRECTORY IS WIPED, since that is what is being rebuilt there.
#
# Usage:
#   ./scripts/sync-to-wsl.ps1
#   ./scripts/sync-to-wsl.ps1 -Destination '~/schwung-other' -KeepBuild

[CmdletBinding()]
param(
    # Destination inside WSL. A leading ~ is expanded by the shell there.
    [string] $Destination = '~/schwung-testing',

    # Keep the destination's build/ instead of wiping it.
    [switch] $KeepBuild
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $repoRoot 'scripts/build.sh'))) {
    throw "Not a Schwung checkout: $repoRoot"
}

# D:\programming\schwung -> /mnt/d/programming/schwung. Done here rather than
# by wslpath: wsl.exe eats backslashes in its arguments, so the path arrives
# as "D:programmingschwung".
function ConvertTo-WslPath([string] $winPath) {
    $full = [System.IO.Path]::GetFullPath($winPath)
    $drive = $full.Substring(0, 1).ToLower()
    '/mnt/' + $drive + ($full.Substring(2) -replace '\\', '/')
}

$srcWsl = ConvertTo-WslPath $repoRoot

Write-Host "Source:      $repoRoot"
Write-Host "             $srcWsl (in WSL)"
Write-Host "Destination: $Destination"
Write-Host ''

$wipeBuild = if ($KeepBuild) { '0' } else { '1' }

# Everything below runs in WSL. Single-quoted here-string: PowerShell must not
# touch $VAR or the backticks, so the two paths are passed as arguments ($1,$2)
# rather than interpolated.
$bash = @'
set -eu
SRC="$1"; DEST="$2"; WIPE_BUILD="$3"

command -v rsync >/dev/null || { echo "ERROR: rsync not installed in WSL (apt install rsync)" >&2; exit 1; }
[ -d "$SRC" ] || { echo "ERROR: source $SRC not visible from WSL" >&2; exit 1; }

# ~ arrives literal from PowerShell; expand it here.
case "$DEST" in "~"|"~/"*) DEST="$HOME${DEST#\~}" ;; esac
mkdir -p "$DEST"

echo "==> Syncing working tree"
# --delete makes the destination match, and an --exclude is also protected
# FROM that delete (which is what keeps the submodules and the destination's
# own build/ alive). .git is excluded: the destination keeps its own.
rsync -a --delete \
    --exclude '.git/' \
    --exclude 'build/' \
    --exclude 'libs/openevv/' \
    --exclude 'libs/link/' \
    --exclude 'node_modules/' \
    --exclude '*.tar.gz' \
    --exclude 'debug.log' \
    --exclude 'crash_maps.txt' \
    "$SRC/" "$DEST/"

echo "==> Normalising line endings (CRLF -> LF)"
# By extension, and never under libs/: the submodules are already LF, and
# rewriting 180 MB of somebody else's checkout is not this script's business.
find "$DEST" \
    \( -path "$DEST/libs" -o -path "$DEST/.git" -o -path "$DEST/build" \) -prune -o \
    -type f \( -name '*.sh' -o -name '*.c' -o -name '*.h' -o -name '*.cpp' \
               -o -name '*.js' -o -name '*.mjs' -o -name '*.json' -o -name '*.py' \
               -o -name '*.md' -o -name '*.txt' -o -name '*.yml' -o -name '*.go' \
               -o -name 'Makefile' \) -print0 \
    | xargs -0 -r sed -i 's/\r$//'
find "$DEST/scripts" -name '*.sh' -exec chmod +x {} +

echo "==> Submodules"
# build.sh gates on these two files per submodule; anything less is "absent".
seed_submodule() {
    name="$1"; shift
    target="$DEST/libs/$name"
    if [ -L "$target" ]; then
        echo "    libs/$name is a SYMLINK - replacing (Docker cannot follow it)"
        rm -f "$target"
    fi
    for probe in "$@"; do
        [ "$probe" = "$target" ] && continue
        if [ -f "$probe/Makefile" ] || [ -f "$probe/CMakeLists.txt" ]; then
            echo "    seeding libs/$name from $probe"
            mkdir -p "$DEST/libs"
            cp -a "$probe" "$target"
            return 0
        fi
    done
    echo "    ERROR: no source found for libs/$name - build.sh will fail." >&2
    echo "           Put a checkout at one of: $*" >&2
    return 1
}

check_submodule() {
    name="$1"; marker="$2"; shift 2
    target="$DEST/libs/$name"
    if [ -e "$target" ] && [ ! -L "$target" ] && [ -f "$target/$marker" ]; then
        echo "    libs/$name present ($(find "$target" -type f | wc -l) files)"
        return 0
    fi
    seed_submodule "$name" "$@"
}

rc=0
check_submodule openevv Makefile \
    "$HOME/openevv-lf" "$HOME/schwung-test/libs/openevv" "$SRC/libs/openevv" || rc=1
check_submodule link CMakeLists.txt \
    "$HOME/schwung-test/libs/link" "$SRC/libs/link" || rc=1

if [ "$WIPE_BUILD" = "1" ]; then
    echo "==> Wiping $DEST/build"
    rm -rf "$DEST/build"
else
    echo "==> Keeping $DEST/build"
fi

echo ""
echo "Done. Build with:"
echo "    cd $DEST && ./scripts/build.sh"
exit $rc
'@

# Hand the payload to WSL as a file so nothing is re-quoted on the way. It must
# be LF itself, or bash reads the CR as part of the last argument.
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "schwung-sync-$PID.sh"
[System.IO.File]::WriteAllText($tmp, ($bash -replace "`r`n", "`n"))
try {
    $tmpWsl = ConvertTo-WslPath $tmp
    wsl.exe -e bash "$tmpWsl" "$srcWsl" "$Destination" $wipeBuild
    $code = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
}

if ($code -ne 0) { throw "Sync failed (exit $code)" }
