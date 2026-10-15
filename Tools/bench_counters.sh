#!/usr/bin/env bash
# What the two telemetry counters cost. Runs OrderBookBench (the real OrderBook.hpp) and
# OrderBookBenchNoCounters (a copy of it without the counter increments, made at configure time)
# alternately, N times each, and compares the median of two measurements over the N runs.
#
#   Tools/bench_counters.sh [runs] [build-dir]
set -uo pipefail

RUNS="${1:-10}"
BUILD_DIR="${2:-build}"
WITH="$BUILD_DIR/Tools/OrderBookBench/OrderBookBench"
WITHOUT="$BUILD_DIR/Tools/OrderBookBench/OrderBookBenchNoCounters"

for bin in "$WITH" "$WITHOUT"; do
    if [ ! -x "$bin" ]; then
        echo "Missing binary: $bin (build OrderBookBench and OrderBookBenchNoCounters first)" >&2
        exit 1
    fi
done

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
for i in $(seq 1 "$RUNS"); do
    "$WITH" > "$WORKDIR/with.$i"
    "$WITHOUT" > "$WORKDIR/without.$i"
done

python3 - "$WORKDIR" "$RUNS" <<'PYEOF'
import re, statistics, sys
workdir, runs = sys.argv[1], int(sys.argv[2])
cells = (("sweep", 10000, "a sweep of 10,000 resting orders"), ("single", 1000, "one order against a 1,000-deep level"))

def medians(variant, kind, depth):
    pattern = re.compile(r"^%s\s+sequential\s+depth=%d\s.*median=([0-9.]+)" % (kind, depth))
    values = []
    for i in range(1, runs + 1):
        for line in open("%s/%s.%d" % (workdir, variant, i)):
            m = pattern.match(line)
            if m:
                values.append(float(m.group(1)))
    return values

print("%-38s %12s %12s %12s" % ("ns per order, median of %d runs" % runs, "with", "without", "difference"))
for kind, depth, label in cells:
    a, b = medians("with", kind, depth), medians("without", kind, depth)
    ma, mb = statistics.median(a), statistics.median(b)
    print("%-38s %12.2f %12.2f %+12.2f   (run-to-run spread: with %.2f-%.2f, without %.2f-%.2f)" % (
        label, ma, mb, ma - mb, min(a), max(a), min(b), max(b)))
PYEOF
