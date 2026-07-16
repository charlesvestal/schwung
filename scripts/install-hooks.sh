#!/usr/bin/env bash
# Enable Schwung's in-repo git hooks (pre-commit runs the fast native checks:
# host-tests + go; see scripts/hooks/pre-commit).
set -euo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

chmod +x scripts/hooks/pre-commit
git config core.hooksPath scripts/hooks

echo "Enabled: core.hooksPath = scripts/hooks"
echo "Pre-commit now runs: host-tests (unit + contract) + go (schwung-manager)."
echo "Bypass a commit with:  git commit --no-verify"
