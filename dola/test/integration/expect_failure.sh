#!/bin/sh
set -eu

if "$1"; then
  echo "expected program to fail" >&2
  exit 1
fi
