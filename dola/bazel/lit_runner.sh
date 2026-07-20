#!/bin/sh
set -eu

lit="$1"
shift
exec "$lit" "$@"
