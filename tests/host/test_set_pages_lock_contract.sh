#!/usr/bin/env bash
# The device's page switch and schwung-manager's Set Pages editor must lock the
# SAME file. If the names drift, the manager can save during the window where
# Move has changed page in memory but has not yet written current_page.txt, and
# the page's Sets end up in the wrong folder (invisible on the device).
set -euo pipefail
cd "$(dirname "$0")/../.."

c_path=$(grep -oE '#define SET_PAGES_LOCK_PATH "[^"]+"' src/host/shadow_set_pages.h |
  head -1 | sed -E 's/.*"(.*)"/\1/')
go_name=$(grep -oE 'setPagesLockName = "[^"]+"' schwung-manager/set_pages.go |
  head -1 | sed -E 's/.*"(.*)"/\1/')

[ -n "$c_path" ] || { echo "FAIL: SET_PAGES_LOCK_PATH missing from shadow_set_pages.h"; exit 1; }
[ -n "$go_name" ] || { echo "FAIL: setPagesLockName missing from set_pages.go"; exit 1; }

want="/data/UserData/schwung/$go_name"
if [ "$c_path" != "$want" ]; then
  echo "FAIL: device locks $c_path, manager locks $want"
  exit 1
fi

echo "test_set_pages_lock_contract: $c_path"
