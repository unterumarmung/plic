#!/usr/bin/env bash

set -euo pipefail

readonly bazelisk_version="${BAZELISK_VERSION:-v1.29.0}"
readonly destination="${1:-${HOME}/.local/bin/bazel}"

case "$(uname -s):$(uname -m)" in
  Linux:x86_64)
    readonly asset="bazelisk-linux-amd64"
    ;;
  Darwin:arm64)
    readonly asset="bazelisk-darwin-arm64"
    ;;
  *)
    echo "unsupported Bazelisk platform: $(uname -s) $(uname -m)" >&2
    exit 1
    ;;
esac

mkdir -p "$(dirname "${destination}")"
curl --fail --location --retry 5 \
  --output "${destination}" \
  "https://github.com/bazelbuild/bazelisk/releases/download/${bazelisk_version}/${asset}"
chmod +x "${destination}"

echo "installed Bazelisk ${bazelisk_version} at ${destination}"
