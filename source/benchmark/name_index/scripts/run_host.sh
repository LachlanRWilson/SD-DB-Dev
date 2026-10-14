#!/usr/bin/env bash
# Build and run the host (non-authoritative) name index benchmark.
# Usage: scripts/run_host.sh [tag]      (tag defaults to the optimisation level, e.g. O3)
# Writes results/<date>_host_<tag>.csv and results/<date>_host_<tag>_meta.txt
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="$root/build-bench-host"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --target test_name_index name_index_bench_host -j"$(nproc)"
"$build/source/benchmark/name_index/test_name_index" --gtest_brief=1

tag="${1:-O3}"
prefix="$here/results/$(date +%F)_host_${tag}"
"$build/source/benchmark/name_index/name_index_bench_host" "$prefix"
echo "wrote ${prefix}.csv and ${prefix}_meta.txt"
