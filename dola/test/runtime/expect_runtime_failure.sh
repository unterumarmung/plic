#!/bin/sh
set -eu

program="$1"
scenario="$2"
expected="$3"
output="$TEST_TMPDIR/$scenario.txt"

if "$program" "$scenario" >"$output" 2>&1; then
  echo "expected $scenario to fail" >&2
  exit 1
fi

if ! grep -F "$expected" "$output" >/dev/null; then
  echo "missing expected diagnostic: $expected" >&2
  sed -n '1,80p' "$output" >&2
  exit 1
fi
