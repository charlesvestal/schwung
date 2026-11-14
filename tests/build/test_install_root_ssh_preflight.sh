#!/usr/bin/env bash
set -euo pipefail

# When SSH works as ableton but not as root (e.g. /data/authorized_keys still
# holds an old key), install.sh must say so up front. It used to carry on,
# read the root partition's free space as "0KB" from the failed SSH call, and
# then die with exit 255 from an unguarded root command.

cd "$(dirname "$0")/../.."

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/repo/scripts" "$tmp/bin" "$tmp/home"
cp scripts/install.sh "$tmp/repo/scripts/install.sh"
: > "$tmp/repo/schwung.tar.gz"
printf '#!/usr/bin/env bash\nexit 0\n' > "$tmp/bin/scp"
chmod +x "$tmp/bin/scp"

run_install() {
  set +e
  out=$(cd "$tmp/repo/scripts" && HOME="$tmp/home" PATH="$tmp/bin:$PATH" \
    bash ./install.sh local --skip-confirmation --skip-modules </dev/null 2>&1)
  rc=$?
  set -e
}

expect() {
  if ! echo "$out" | grep -q "$1"; then
    echo "FAIL ($2): expected output containing: $1" >&2
    echo "$out" >&2
    exit 1
  fi
}

reject() {
  if echo "$out" | grep -q "$1"; then
    echo "FAIL ($2): unexpected output: $1" >&2
    echo "$out" >&2
    exit 1
  fi
}

# Case 1: root refuses the key. Preflight must stop with a clear message.
cat > "$tmp/bin/ssh" <<'EOF'
#!/usr/bin/env bash
for a in "$@"; do
  case "$a" in
    root@*) echo "$a: Permission denied (publickey)." >&2; exit 255 ;;
  esac
done
exit 0
EOF
chmod +x "$tmp/bin/ssh"
run_install
if [ "$rc" -ne 1 ]; then
  echo "FAIL (root refused): expected exit 1, got $rc" >&2
  echo "$out" >&2
  exit 1
fi
expect "SSH as 'ableton' works, but SSH as 'root' failed" "root refused"
expect "/development/ssh" "root refused"
reject "Root partition has less than" "root refused"

# Case 2: root login works, but the free-space read fails. Must not be
# reported as a full disk.
cat > "$tmp/bin/ssh" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *"df /"*) exit 255 ;;
esac
exit 0
EOF
chmod +x "$tmp/bin/ssh"
run_install
if [ "$rc" -ne 1 ]; then
  echo "FAIL (df read fails): expected exit 1, got $rc" >&2
  echo "$out" >&2
  exit 1
fi
expect "Could not read free space on the root partition" "df read fails"
reject "Root partition has less than" "df read fails"
reject "0KB" "df read fails"

echo "PASS: install.sh reports root SSH failures clearly instead of a fake full disk"
exit 0
