#!/usr/bin/env bash
set -euo pipefail

# Regression test: sampler and skipback saves should use dated subfolders:
#   Samples/Move Everything/Resampler/YYYY-MM-DD/
#   Samples/Move Everything/Skipback/YYYY-MM-DD/

file_h="src/host/shadow_sampler.h"
file_c="src/host/shadow_sampler.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q '^#define SAMPLER_RECORDINGS_DIR ".*/Resampler"$' "$file_h"; then
  echo "FAIL: sampler recordings dir is not scoped to Resampler/" >&2
  exit 1
fi

if ! rg -q '^#define SKIPBACK_DIR ".*/Skipback"$' "$file_h"; then
  echo "FAIL: skipback dir constant missing or changed unexpectedly" >&2
  exit 1
fi

if ! rg -q 'strftime\(date_subdir, sizeof\(date_subdir\), "%Y-%m-%d",' "$file_c"; then
  echo "FAIL: dated folder format (%Y-%m-%d) not generated in sampler logic" >&2
  exit 1
fi

if ! rg -q 'snprintf\(recording_dir, sizeof\(recording_dir\), "%s/%s",\s*SAMPLER_RECORDINGS_DIR,\s*date_subdir\);' "$file_c"; then
  echo "FAIL: sampler recording dir is not built with date subfolder" >&2
  exit 1
fi

if ! rg -q 'snprintf\(skipback_dir, sizeof\(skipback_dir\), "%s/%s",\s*SKIPBACK_DIR,\s*date_subdir\);' "$file_c"; then
  echo "FAIL: skipback dir is not built with date subfolder" >&2
  exit 1
fi

if ! rg -q 'snprintf\(sampler_current_recording, sizeof\(sampler_current_recording\),\s*"%s/sample_' "$file_c"; then
  echo "FAIL: sampler filename is not emitted under dated recording dir" >&2
  exit 1
fi

if ! rg -q 'snprintf\(path, sizeof\(path\),\s*"%s/skipback_' "$file_c"; then
  echo "FAIL: skipback filename is not emitted under dated skipback dir" >&2
  exit 1
fi

echo "PASS: sampler and skipback save into dated feature subfolders"
exit 0
