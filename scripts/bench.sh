#!/usr/bin/env bash
# Builds the release configuration and runs every benchmark. Usage: scripts/bench.sh [mde_bench args]
# Writes the markdown report to bench/results/<date>-<os>-<arch>.md and prints it.
# For meaningful numbers, run on an otherwise idle machine (no builds, tests or VMs busy).
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset release >/dev/null
cmake --build --preset release --target mde_bench >/dev/null
mkdir -p bench/results
out="bench/results/$(date +%Y-%m-%d)-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m).md"
./build/release/bench/mde_bench "$@" | tee "$out"
echo "wrote $out" >&2
