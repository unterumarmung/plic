#!/bin/sh
set -eu

program="$1"
expected="$2"
actual="$("$program")"
case "$actual" in
  *"$expected"*) ;;
  *)
    echo "expected output to contain: $expected" >&2
    echo "actual: $actual" >&2
    exit 1
    ;;
esac
