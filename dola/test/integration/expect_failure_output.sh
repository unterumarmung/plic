#!/bin/sh
set -eu

output_file="${TEST_TMPDIR:-/tmp}/dola-failure-output.txt"
if "$1" >"$output_file" 2>&1; then
  echo "expected program to fail" >&2
  exit 1
fi
if ! grep -F "$2" "$output_file" >/dev/null; then
  echo "expected output to contain: $2" >&2
  sed -n '1,120p' "$output_file" >&2
  exit 1
fi
