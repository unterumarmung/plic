#!/usr/bin/env bash

set -euo pipefail

readonly bazel="${BAZEL:-bazel}"
readonly script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly workspace_directory="$(dirname "${script_directory}")"
readonly ci_bazelrc="${script_directory}/ci.bazelrc"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "this CI script requires Linux x86_64; found $(uname -s) $(uname -m)" >&2
  exit 1
fi

run() {
  echo "+ $*"
  "$@"
}

cd "${workspace_directory}"

run uname -a
run "${bazel}" version
run "${bazel}" --bazelrc="${ci_bazelrc}" build //...
run "${bazel}" --bazelrc="${ci_bazelrc}" test //... --test_output=errors
run "${bazel}" --bazelrc="${ci_bazelrc}" test //:quality --test_output=errors
run "${bazel}" --bazelrc="${ci_bazelrc}" \
  test //test/integration:chat_stress_test \
  --runs_per_test=3 \
  --nocache_test_results \
  --test_output=errors

for example in hello arithmetic records collections concurrency transport; do
  run "${bazel}" --bazelrc="${ci_bazelrc}" run "//examples/${example}"
done
