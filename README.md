# EventContractLOB

[![CI](https://github.com/infinite-simplex/EventContractLOB/actions/workflows/ci.yml/badge.svg)](https://github.com/infinite-simplex/EventContractLOB/actions/workflows/ci.yml)

A limit order book for binary-outcome event contracts (priced 0-100),
speaking real NASDAQ OUCH 4.2 on ingress. Built on top of
[ToyExchange](https://github.com/infinite-simplex/ToyExchange)'s
Disruptor-style pipeline — replicated, leader-elected matching with real
failover, a durable sequenced log with gap-fill retransmit — via CMake
`FetchContent` rather than a copy or a submodule.

This is a portfolio project, not a production exchange.

## Architecture

Three processes: **NetworkServiceApp** (gateway — OUCH ingress, sequencing,
durable log with gap-fill retransmit, leader arbitration, egress),
**MatchingServiceApp** (N replicas of the matching engine, only the elected
leader publishes), and optionally **PerformanceServiceApp** (from ToyExchange —
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

Every check above runs in the OUCH handler (a bad frame becomes a rejected command
without dropping the connection) and again in the book, so nothing that
bypasses the handler can rest an out-of-range order.

## Building and running

Linux only (uses `epoll`, `mmap`, multicast sockets, `CLOCK_TAI` directly —
on Windows, build under WSL).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./run_exchange.sh 2 build
```

Core asks for 4 MB UDP receive buffers. If `net.core.rmem_max` is lower, the receivers print a
warning at start-up and a burst can drop datagrams (retransmit recovers them, at a cost);
`sudo sysctl -w net.core.rmem_max=4194304` removes it.

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
arena collision detection. CI runs this on every push. ToyExchange carries
its own separate test suite for the pipeline primitives this repo consumes.

## License

[MIT](LICENSE).
