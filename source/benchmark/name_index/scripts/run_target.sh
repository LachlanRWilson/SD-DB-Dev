#!/usr/bin/env bash
# Build, flash and run the target name index benchmark on the NUCLEO-H723ZG.
# Results come back over semihosting, so OpenOCD stays attached for the run.
#
# Usage: scripts/run_target.sh [Release|Debug]   (Release = -Os, Debug = -O0)
# Writes results/<date>_target_<Os|O0>.csv and results/<date>_target_<Os|O0>_meta.txt
#
# The SD baseline writes to the card's DB region (its contents are disposable).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../../.." && pwd)"
build_type="${1:-Release}"
build="$root/build-bench-$build_type"
tag=$([ "$build_type" = "Release" ] && echo Os || echo O0)

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="firmware/cmake/gcc-arm-none-eabi.cmake" -DBUILD_STM32=ON -DNAME_INDEX_BENCH=ON -DCMAKE_BUILD_TYPE="$build_type" > /dev/null
cmake --build "$build"
elf="$build/firmware/MyProject.elf"

work="$(mktemp -d)"
cd "$work"
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
    -c "init" -c "reset halt" -c "program $elf verify" -c "arm semihosting enable" -c "reset run" > openocd.log 2>&1 &
ocd=$!
python3 "$here/scripts/ocd_resume.py" > resume.log 2>&1 &
wd=$!
trap 'kill $ocd $wd 2>/dev/null || true' EXIT

echo "running (progress lines from the target appear in $work/openocd.log)"
until grep -q '^done,1' target_meta.txt 2>/dev/null; do
    if ! kill -0 $ocd 2>/dev/null; then echo "openocd exited early, see $work/openocd.log"; exit 1; fi
    sleep 5
done

prefix="$here/results/$(date +%F)_target_${tag}"
mv target.csv "${prefix}.csv"
mv target_meta.txt "${prefix}_meta.txt"
[ -s resume.log ] && { echo "spurious halts resumed (outside timed regions):"; cat resume.log; }
echo "wrote ${prefix}.csv and ${prefix}_meta.txt"
