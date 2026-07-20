#!/bin/sh
set -eu

program="$1"
shift
expected="$*"
actual="$("$program")"
if [ "$actual" != "$expected" ]; then
  echo "expected: $expected" >&2
  echo "actual:   $actual" >&2
  exit 1
fi
