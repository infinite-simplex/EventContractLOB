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
leader publishes), **PerformanceServiceApp** (from ToyExchange — HdrHistogram
latency percentiles aggregated across replicas). A custom compact frame on
egress (no public ITCH feed yet).

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

Both checks run in the OUCH handler (a bad frame becomes a rejected command
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

Then, from another shell:

```bash
build/Tools/OuchTestClient/OuchTestClient --side B --price 50 --qty 10 --firm-id 1
build/Tools/OuchTestClient/OuchTestClient --side S --price 50 --qty 10 --firm-id 2 --listen-ms 500
```

`./kill_leader.sh run` kills only the current leader replica, to watch
failover happen live. `tail -f run/performance.log` shows latency
percentiles.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

Matching engine, full network ingress pipeline, GTD auto-cancel, telemetry
arena collision detection. CI runs this on every push. ToyExchange carries
its own separate test suite for the pipeline primitives this repo consumes.

## License

[MIT](LICENSE).
