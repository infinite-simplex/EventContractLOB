#!/usr/bin/env bash
# Sustained matching throughput of the engine alone (no gateway, no sockets): see Tools/EngineThroughput/main.cpp.
# Runs each configuration for SECONDS seconds (default 30) and prints the headline line of each.
#
#   Tools/engine_throughput.sh [seconds] [build-dir]
set -uo pipefail

SECONDS_RUN="${1:-30}"
BUILD_DIR="${2:-build}"
BIN="$BUILD_DIR/Tools/EngineThroughput/EngineThroughput"
if [ ! -x "$BIN" ]; then
    echo "Missing binary: $BIN (build EngineThroughput first)" >&2
    exit 1
fi

run() {
    "$BIN" --seconds "$SECONDS_RUN" "$@" | grep -E "^engine:|^queues:|^service:|mix:|SUSTAINED|^latency"
    echo
}

echo "### the matching logic alone, by resting depth"
for depth in 1000 50000 200000; do run --variant engine --depth "$depth"; done
echo "### the same depth, two more runs (run-to-run spread)"
for _ in 1 2; do run --variant engine --depth 50000; done
echo "### the same depth with the threads not pinned"
run --variant engine --depth 50000 --no-pin
echo "### with the real event and trace queues drained by a second thread"
run --variant queues --depth 50000
echo "### through Core's MatchingService, fed by a producer thread"
run --variant service --depth 50000
