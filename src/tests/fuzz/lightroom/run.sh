#!/usr/bin/env bash
# build the harness and fuzz; extra arguments go to libFuzzer, e.g.
#   ./run.sh -max_total_time=300 -jobs=8
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
if [ -z "${IN_NIX_SHELL:-}" ]; then
  exec nix-shell "$HERE/shell.nix" --run "$(printf '%q ' "$0" "$@")"
fi

"$HERE/build.sh"
mkdir -p "$HERE/corpus" "$HERE/artifacts"
cd "$HERE"
# real sidecars are around 10 kB
exec ./fuzz_lightroom -artifact_prefix=artifacts/ -print_final_stats=1 -max_len=32768 \
  -dict=lightroom.dict "$@" corpus
