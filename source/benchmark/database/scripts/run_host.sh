#!/usr/bin/env bash
# Build and run one host (non-authoritative) database benchmark.
# Usage: scripts/run_host.sh <bench> [quick]     (bench: contacts | messages | calls | init)
# Writes results/<date>_host_O3_<bench>.csv and _meta.txt (quick runs go to the scratch dir only)
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="$root/build-bench-host"
bench="${1:?usage: run_host.sh <bench> [quick]}"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --target database_bench_host database_bench_host_quick -j"$(nproc)"
bin="$build/source/benchmark/database"

if [ "${2:-}" = "quick" ]; then
    out="$(mktemp -d)"
    "$bin/database_bench_host_quick" "$bench" "$out/quick"
    echo "quick run ok: $out/quick.csv"
    exit 0
fi

prefix="$here/results/$(date +%F)_host_O3_${bench}"
"$bin/database_bench_host" "$bench" "$prefix"
echo "wrote ${prefix}.csv and ${prefix}_meta.txt"
