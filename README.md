# EventContractLOB

[![CI](https://github.com/infinite-simplex/EventContractLOB/actions/workflows/ci.yml/badge.svg)](https://github.com/infinite-simplex/EventContractLOB/actions/workflows/ci.yml)

A limit order book for binary-outcome event contracts (priced 0-100),
speaking real NASDAQ OUCH 4.2 on ingress. Built on top of
[DisruptorExchange](https://github.com/infinite-simplex/DisruptorExchange)'s
Disruptor-style pipeline (Core v0.1.0, pinned in `CMakeLists.txt`) —
replicated, leader-elected matching with real failover, a sequenced
memory-mapped log with gap-fill retransmit — via CMake `FetchContent` rather
than a copy or a submodule.

This is a portfolio project, not a production exchange.

## Architecture

Three processes: **NetworkServiceApp** (gateway — OUCH ingress, sequencing,
memory-mapped log with gap-fill retransmit, leader arbitration, egress),
**MatchingServiceApp** (N replicas of the matching engine, only the elected
leader publishes), and optionally **PerformanceServiceApp** (from DisruptorExchange —
HdrHistogram latency percentiles aggregated across replicas; build it with
`-DBUILD_PERFORMANCE_SERVICE=ON`, which fetches HdrHistogram_c). A custom compact
frame on egress (no public ITCH feed yet).

Design rationale and tradeoffs are written up in the
[EventContractLOB design write-up](https://infinite-simplex.github.io/InfiniteLabs/#EventContractLOB).

## Limits

- **Price:** whole cents, 0–100 inclusive (a contract that settles at $0.00 or
  $1.00). On the wire this is OUCH's real fixed point (dollars × 10,000, so
  $0.42 is `4200`); a sub-cent remainder is truncated, and anything above 100
  cents is rejected with `PRICE_OUT_OF_RANGE`.
- **Quantity:** unsigned, 1–999,999 shares, matching OUCH's "> 0 and
  < 1,000,000". An enter or replace outside that range is rejected with
  `INVALID_QUANTITY`. A cancel's `shares = 0` still means "cancel all
  remaining".

- **Book depth:** 262,144 resting orders per side. Past that, the part of an order
  that would rest is rejected with `BOOK_FULL`; fills it already made stand.
- **Order ids:** 10,000,000 per run (one counter, never reset). After that every
  enter and replace is rejected with `ORDER_LIMIT_REACHED`.
- **Order tokens:** the gateway remembers 1,835,008 distinct client order tokens per run,
  one per enter and one per replace. Past that every new enter and replace is rejected with
  `TOKEN_TABLE_FULL`; orders already registered can still be canceled or replaced. It logs a
  warning at 50% and 80% full.

The price, quantity and order-id checks run in the OUCH handler (a bad frame becomes a rejected
command without dropping the connection) and again in the book, so nothing that
bypasses the handler can rest an out-of-range order.

## Building and running

Linux only (uses `epoll`, `mmap`, multicast sockets and a calibrated TSC clock
that falls back to `CLOCK_TAI` — on Windows, build under WSL).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./run_exchange.sh 2 build
```

Core asks for 4 MB UDP receive buffers. If `net.core.rmem_max` is lower, the receivers and the
gateway print a warning at start-up and a burst can drop datagrams (retransmit recovers the
replicas' ingress ones, at a cost; a lost egress datagram is not recovered, and the gateway
counts and warns about the events that never arrived);
`sudo sysctl -w net.core.rmem_max=4194304` removes it.

For load, Core advises two settings: `--ingress-spin-us 100` on each `MatchingServiceApp`
(the receiver polls for 100 µs after a datagram before it sleeps) and `--no-egress-echo` on
`NetworkServiceApp` (no `[egress]` console line per event). `--egress-spin-us` is the
gateway's equivalent of the first. The replicas and the gateway must run the same Core version,
because the egress wire format can change between Core versions.

Then, from another shell:

```bash
build/Tools/OuchTestClient/OuchTestClient --side B --price 50 --qty 10 --firm-id 1
build/Tools/OuchTestClient/OuchTestClient --side S --price 50 --qty 10 --firm-id 2 --listen-ms 500
```

`./kill_leader.sh run` kills only the current leader replica, to watch
failover happen live. With the performance service built, `tail -f
run/performance.log` shows latency percentiles.

## Measuring

Configure with `-DBUILD_PERFORMANCE_SERVICE=ON`, then:

```bash
Tools/load_test.sh 10000 30 build   # whole stack under OuchLoadGenerator: orders/s, seconds
Tools/load_test.sh 200000 10 build tuned   # the same with the two load settings above
build/Tools/OrderBookBench/OrderBookBench   # the book alone: cost per trade against level depth
Tools/bench_counters.sh 12 build    # what the two telemetry counters cost
Tools/engine_throughput.sh 30 build # the engine alone: sustained orders/s, no gateway or sockets
```

The results, with the machine and the caveats, are on the design write-up.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

Matching engine, full network ingress pipeline, GTD auto-cancel, telemetry
arena collision detection. CI runs this on every push. DisruptorExchange carries
its own separate test suite for the pipeline primitives this repo consumes.

## License

[MIT](LICENSE).
