#!/usr/bin/env bash
# Build, flash and run one database benchmark on the NUCLEO-H723ZG.
# Results come back over semihosting, so OpenOCD stays attached for the run.
#
# Usage: scripts/run_target.sh <bench> [Release|Debug] [quick]   (Release = -Os, Debug = -O0)
# Writes results/<date>_target_<Os|O0>_<bench>[_$DB_BENCH_TAG].csv and _meta.txt
#
# The benchmark formats the database on the SD card (its contents are lost).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../../.." && pwd)"
bench="${1:?usage: run_target.sh <bench> [Release|Debug] [quick]}"
build_type="${2:-Release}"
quick=$([ "${3:-}" = "quick" ] && echo ON || echo OFF)
build="$root/build-dbbench-$build_type"
tag=$([ "$build_type" = "Release" ] && echo Os || echo O0)

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="firmware/cmake/gcc-arm-none-eabi.cmake" -DBUILD_STM32=ON \
    -DDATABASE_BENCH=ON -DDATABASE_BENCH_NAME="$bench" -DDATABASE_BENCH_QUICK="$quick" -DCMAKE_BUILD_TYPE="$build_type" > /dev/null
cmake --build "$build"
elf="$build/firmware/MyProject.elf"

work="$(mktemp -d)"
cd "$work"
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
    -c "init" -c "reset halt" -c "program $elf verify" -c "arm semihosting enable" -c "reset run" > openocd.log 2>&1 &
ocd=$!
python3 "$root/source/benchmark/name_index/scripts/ocd_resume.py" > resume.log 2>&1 &
wd=$!
trap 'kill $ocd $wd 2>/dev/null || true' EXIT

echo "running (progress lines from the target appear in $work/openocd.log)"
until grep -q '^done,' target_meta.txt 2>/dev/null; do
    if ! kill -0 $ocd 2>/dev/null; then echo "openocd exited early, see $work/openocd.log"; exit 1; fi
    sleep 5
done
[ -s resume.log ] && { echo "spurious halts resumed:"; cat resume.log; }
if ! grep -q '^done,1' target_meta.txt; then
    echo "benchmark FAILED, see $work/target_meta.txt and $work/openocd.log"; cat target_meta.txt; exit 1
fi
if [ "$quick" = "ON" ]; then echo "quick run ok: $work/target.csv"; cat target.csv; exit 0; fi

prefix="$here/results/$(date +%F)_target_${tag}_${bench}${DB_BENCH_TAG:+_$DB_BENCH_TAG}"
mv target.csv "${prefix}.csv"
mv target_meta.txt "${prefix}_meta.txt"
echo "wrote ${prefix}.csv and ${prefix}_meta.txt"
