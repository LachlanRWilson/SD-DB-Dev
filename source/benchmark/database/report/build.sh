#!/usr/bin/env bash
# Rebuild report/dbms-benchmark.html (the shareable results page) from results/.
# Usage: report/build.sh
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
bench="$(cd "$here/.." && pwd)"

python3 "$bench/scripts/plot.py" --inline --out "$here" > /dev/null
python3 "$here/gen.py" "$bench/results" "$here"
echo "wrote $here/dbms-benchmark.html"
