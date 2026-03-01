#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script="$repo_root/scripts/uninstall.sh"

if [ ! -f "$script" ]; then
  echo "FAIL: Missing $script" >&2
  exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

logfile="$tmpdir/ssh.log"
mkdir -p "$tmpdir/bin"

cat >"$tmpdir/bin/ssh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

args=("$@")
argc=${#args[@]}
remote_cmd="${args[$((argc - 1))]}"
printf '%s\n' "$remote_cmd" >>"$SSH_LOG"
exit 0
EOF
chmod +x "$tmpdir/bin/ssh"

PATH="$tmpdir/bin:$PATH" SSH_LOG="$logfile" MOVE_FORCE_UNINSTALL=1 bash "$script" >/dev/null 2>&1

backup_line=$(rg -n "/data/UserData/UserLibrary/Move Everything Backups/Set Pages" "$logfile" | head -n 1 | cut -d: -f1 || true)
remove_line=$(rg -n "rm -rf ~/move-anything ~/move-anything.tar.gz" "$logfile" | head -n 1 | cut -d: -f1 || true)

if [ -z "$backup_line" ]; then
  echo "FAIL: uninstall.sh did not export set-page backups before cleanup" >&2
  exit 1
fi

if [ -z "$remove_line" ]; then
  echo "FAIL: uninstall.sh did not remove the Move Anything payload" >&2
  exit 1
fi

if [ "$backup_line" -ge "$remove_line" ]; then
  echo "FAIL: backup export must happen before payload removal ($backup_line >= $remove_line)" >&2
  exit 1
fi

echo "PASS: uninstall.sh exports set-page backups before removing Move Anything"
