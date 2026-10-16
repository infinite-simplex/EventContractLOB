#!/usr/bin/env bash
# Load test: one NetworkServiceApp gateway, two MatchingServiceApp replicas and a PerformanceServiceApp,
# driven by OuchLoadGenerator, then a summary of the one-second windows the performance service printed.
# Needs a build configured with -DBUILD_PERFORMANCE_SERVICE=ON. Run it under WSL/Linux.
#
#   Tools/load_test.sh [orders-per-second] [seconds] [build-dir] [tuned]
#
# "tuned" is Core's advice for load: the replicas spin 100 us after each datagram, and the gateway does not
# print an "[egress]" line per event. Without it Core's defaults run.
#
# Each window holds the commands whose traces completed in that second. The first three windows (the
# book is still empty) and the last (partial) are left out. For every metric the summary gives, over
# the remaining windows of both replicas, the median of each window's p50, p99 and p99.99 (with the
# worst window in brackets) and the largest max.
set -uo pipefail

RATE="${1:-10000}"
SECONDS_RUN="${2:-30}"
BUILD_DIR="${3:-build}"
GATEWAY_FLAGS=()
REPLICA_FLAGS=()
if [ "${4:-}" = "tuned" ]; then
    GATEWAY_FLAGS=(--no-egress-echo)
    REPLICA_FLAGS=(--ingress-spin-us 100)
fi

NET_BIN="$BUILD_DIR/NetworkService/NetworkServiceApp"
MATCH_BIN="$BUILD_DIR/MatchingService/MatchingServiceApp"
PERF_BIN="$BUILD_DIR/PerformanceService/PerformanceServiceApp"
GEN_BIN="$BUILD_DIR/Tools/OuchLoadGenerator/OuchLoadGenerator"

for bin in "$NET_BIN" "$MATCH_BIN" "$PERF_BIN" "$GEN_BIN"; do
    if [ ! -x "$bin" ]; then
        echo "Missing binary: $bin (configure with -DBUILD_PERFORMANCE_SERVICE=ON and build everything)" >&2
        exit 1
    fi
done

WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    for pid in "${PIDS[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

"$NET_BIN" --sequence-store "$WORKDIR/sequence_store.dat" --symbol TEST "${GATEWAY_FLAGS[@]}" > "$WORKDIR/network.log" 2>&1 &
PIDS+=("$!")
GATEWAY_PID="$!"
sleep 0.3
REPLICA_PIDS=()
for i in 1 2; do
    "$MATCH_BIN" --id "replica-$i" --replica-id "$i" --stats-interval-ms 500 "${REPLICA_FLAGS[@]}" > "$WORKDIR/replica-$i.log" 2>&1 &
    PIDS+=("$!")
    REPLICA_PIDS+=("$!")
done
"$PERF_BIN" --window-ms 1000 > "$WORKDIR/performance.log" 2>&1 &
PIDS+=("$!")
sleep 1.0

for pid in "${PIDS[@]}"; do
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "FAIL: a process exited immediately, see $WORKDIR" >&2
        exit 1
    fi
done

echo "Workdir: $WORKDIR"
"$GEN_BIN" --rate "$RATE" --duration-s "$SECONDS_RUN" --seed 1337 | tee "$WORKDIR/generator.txt"

# Wait until both replicas have applied the same sequence number and it has stopped moving for 3 s
# (they catch up after the load, through the gateway's retransmit server), at most 3 minutes.
applied() { grep -o 'applied=[0-9]*' "$WORKDIR/replica-$1.log" | tail -1 | cut -d= -f2; }
stable=0
for _ in $(seq 1 360); do
    sleep 0.5
    now="$(applied 1)/$(applied 2)"
    if [ "$now" = "${last:-}" ] && [ "$(applied 1)" = "$(applied 2)" ]; then stable=$((stable + 1)); else stable=0; fi
    last="$now"
    [ "$stable" -ge 6 ] && break
done

# A graceful stop makes each replica print its final sequence number and a digest of its whole book.
kill -TERM "${REPLICA_PIDS[@]}" 2>/dev/null
wait "${REPLICA_PIDS[@]}" 2>/dev/null
# Stopping the gateway makes it print what its egress side received.
kill -TERM "$GATEWAY_PID" 2>/dev/null
wait "$GATEWAY_PID" 2>/dev/null
echo
for i in 1 2; do
    echo "replica-$i $(grep -o 'final applied=.*' "$WORKDIR/replica-$i.log" | tail -1)   (sequence gaps recovered: $(grep -c 'Sequence gap' "$WORKDIR/replica-$i.log"), leader changes: $(( $(grep -o 'epoch=[0-9]*' "$WORKDIR/replica-$i.log" | cut -d= -f2 | sort -n | tail -1) - 1 )))"
done
if [ "$(grep -o 'final applied=.*' "$WORKDIR/replica-1.log" | tail -1)" = "$(grep -o 'final applied=.*' "$WORKDIR/replica-2.log" | tail -1)" ] \
    && grep -q 'final applied=' "$WORKDIR/replica-1.log"; then
    echo "replicas: IDENTICAL books"
else
    echo "replicas: DIFFERENT books (or a replica did not stop cleanly)"
fi
echo "gateway $(grep -a 'egress stats:' "$WORKDIR/network.log" | tail -1)   (egress loss warnings: $(grep -a -c 'did not receive' "$WORKDIR/network.log"))"
echo

python3 - "$WORKDIR/performance.log" "$WORKDIR/generator.txt" <<'PYEOF'
import re, statistics, sys
perf, generator = sys.argv[1], sys.argv[2]
line = re.compile(r"\[perf\] replica=(\d+) metric=(\w+) n=(\d+) min=(\d+) max=(\d+) p50=(\d+) p99=(\d+) p99\.99=(\d+)")
windows = {}  # (replica, metric) -> [(n, max, p50, p99, p9999)] in time order
for text in open(perf):
    m = line.match(text)
    if m:
        windows.setdefault((m.group(1), m.group(2)), []).append(tuple(int(m.group(i)) for i in range(3, 9)))
sent = 0
for text in open(generator):
    m = re.match(r"sent (\d+) orders", text)
    if m:
        sent = int(m.group(1))

print("%-16s %7s %10s %12s %20s %22s %12s" % ("metric", "windows", "samples", "p50", "p99 (worst)", "p99.99 (worst)", "max"))
for metric in ("engine_exec_ns", "ext_counter_0", "ext_counter_1"):
    kept = []
    samples = {}
    for (replica, name), rows in sorted(windows.items()):
        if name != metric:
            continue
        samples[replica] = sum(r[0] for r in rows)
        kept += rows[3:-1]
    if not kept:
        print("%-16s no windows" % metric)
        continue
    p50 = statistics.median(r[3] for r in kept)
    p99 = statistics.median(r[4] for r in kept)
    p9999 = statistics.median(r[5] for r in kept)
    print("%-16s %7d %10d %12g %12g (%g) %14g (%g) %12d" % (
        metric, len(kept), sum(r[0] for r in kept), p50, p99, max(r[4] for r in kept),
        p9999, max(r[5] for r in kept), max(r[2] for r in kept)))
    if metric == "engine_exec_ns":
        print("  traces reported per replica over the whole run: %s, orders sent: %d" % (
            ", ".join("replica %s: %d" % (k, v) for k, v in sorted(samples.items())), sent))
PYEOF
