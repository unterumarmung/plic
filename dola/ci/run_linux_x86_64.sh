#!/usr/bin/env bash

set -euo pipefail

readonly bazel="${BAZEL:-bazel}"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "this CI script requires Linux x86_64; found $(uname -s) $(uname -m)" >&2
  exit 1
fi

run() {
  echo "+ $*"
  "$@"
}

run uname -a
run "${bazel}" version
run "${bazel}" build //...
run "${bazel}" test //... --test_output=errors
run "${bazel}" test //:quality --test_output=errors
run "${bazel}" test //test/integration:chat_stress_test \
  --runs_per_test=3 \
  --nocache_test_results \
  --test_output=errors

for example in hello arithmetic records collections concurrency transport; do
  run "${bazel}" run "//examples/${example}"
done
