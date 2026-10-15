// Sustained matching throughput of the engine alone: no gateway, no sockets, no UDP. A command stream is
// generated once, then replayed through the real OrderBook for as long as asked, and orders per second are
// counted over whole cycles.
//
// The stream. It runs a shadow book while it generates, so every cancel and replace names an order that is
// resting, and it holds the number of resting orders near --depth. The first --depth commands are passive
// limit orders that build the book; after that the mix is 45% passive limit orders (bids at 30-49, asks at
// 51-70, so they rest), 8% marketable limit orders that cross a few ticks, 7% small IOC orders, 30% cancels
// and 10% replaces, and a passive order that would push the book past --depth becomes a cancel. Ids of orders
// that are gone are reused. A cycle ends by cancelling everything that is left, so the stream can be replayed
// back to back on the same book. Bids use firm ids 1-32 and asks 33-64. (OuchLoadGenerator's mix is different:
// it cannot know which orders are resting, so most of its cancels miss.)
//
// Three ways to run it:
//   engine   the OrderBook alone on one thread, events discarded: the cost of the matching logic
//   queues   the same, with the real event and trace queues drained by a second thread
//   service  Core's MatchingService, fed through its input queue by a producer thread and drained by a
//            consumer thread: the replica's engine path with the network cut off
//
// Every cycle must emit the number of events the generator predicted and leave the book empty, so a rate can
// not come from a run that did different work.
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "MatchingService.hpp"
#include "OrderBook.hpp"
#include "OuchOrderCommand.hpp"
#include "SPSCProducerPolicy.hpp"

namespace {

constexpr std::uint32_t NOT_LIVE = UINT32_MAX;

class LiveSet {
public:
    LiveSet() : pos_(ORDER_ID_LIMIT, NOT_LIVE) {}
    bool contains(OrderId id) const { return pos_[id] != NOT_LIVE; }
    std::size_t size() const { return ids_.size(); }
    bool empty() const { return ids_.empty(); }
    OrderId at(std::size_t i) const { return ids_[i]; }
    void add(OrderId id) {
        pos_[id] = static_cast<std::uint32_t>(ids_.size());
        ids_.push_back(id);
    }
    bool erase(OrderId id) {
        const std::uint32_t p = pos_[id];
        if (p == NOT_LIVE) return false;
        const OrderId last = ids_.back();
        ids_[p] = last;
        pos_[last] = p;
        ids_.pop_back();
        pos_[id] = NOT_LIVE;
        return true;
    }

private:
    std::vector<std::uint32_t> pos_;
    std::vector<OrderId> ids_;
};

// Tracks which ids are resting from the events the book emits, and hands the ids of orders that are gone back.
struct ShadowPolicy {
    LiveSet& live;
    std::vector<OrderId>& freeIds;
    std::uint64_t events{ 0 };
    std::uint64_t rejects{ 0 };

    void on_order_event(const OrderEvent& e) {
        ++events;
        switch (e.type) {
        case OrderEventType::ACCEPTED:
        case OrderEventType::REPLACED: live.add(e.order_id); break;
        case OrderEventType::CANCELED: release(e.order_id); break;
        case OrderEventType::EXECUTED:
            if (e.leaves_quantity == 0) release(e.order_id);
            break;
        case OrderEventType::REJECTED: ++rejects; break;
        default: break;
        }
    }
    void on_trace_complete(const OrderTrace&) noexcept {}

private:
    void release(OrderId id) {
        if (live.erase(id)) freeIds.push_back(id);
    }
};

struct Stream {
    std::vector<OuchOrderCommand> commands;
    std::uint64_t events{ 0 };
    std::uint64_t passives{ 0 }, marketables{ 0 }, iocs{ 0 }, cancels{ 0 }, replaces{ 0 }, drains{ 0 };
    double meanResting{ 0 };
    std::size_t atDepth{ 0 }; // commands issued with the book within 10% of its target depth
};

Stream generate(std::size_t count, std::size_t depth, std::uint64_t seed) {
    Stream s;
    s.commands.reserve(count + depth + 16);
    LiveSet live;
    std::vector<OrderId> freeIds;
    ShadowPolicy policy{ live, freeIds };
    auto book = std::make_unique<OrderBook<ShadowPolicy>>(policy);
    std::mt19937_64 rng(seed);
    auto below = [&](std::uint64_t n) { return rng() % n; };
    OrderId nextFresh = 1;
    auto allocId = [&]() -> OrderId {
        if (!freeIds.empty()) {
            const OrderId id = freeIds.back();
            freeIds.pop_back();
            return id;
        }
        if (nextFresh >= ORDER_ID_LIMIT) {
            std::fprintf(stderr, "the stream needs more order ids than the book has\n");
            std::exit(1);
        }
        return nextFresh++;
    };
    auto submit = [&](OuchOrderCommand cmd) {
        cmd.trace_id = static_cast<TraceId>(s.commands.size());
        s.commands.push_back(cmd);
        book->submit_order(cmd);
    };

    enum class Action { PASSIVE, MARKETABLE, IOC, CANCEL, REPLACE };
    double restingSum = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const std::uint64_t draw = below(100);
        Action action = draw < 45   ? Action::PASSIVE
                        : draw < 53 ? Action::MARKETABLE
                        : draw < 60 ? Action::IOC
                        : draw < 90 ? Action::CANCEL
                                    : Action::REPLACE;
        if (k < depth) action = Action::PASSIVE; // build the book first
        if (action == Action::PASSIVE && live.size() >= depth) action = Action::CANCEL;
        if ((action == Action::CANCEL || action == Action::REPLACE) && live.empty()) action = Action::PASSIVE;
        restingSum += static_cast<double>(live.size());
        if (live.size() * 10 >= depth * 9) ++s.atDepth;

        OuchOrderCommand cmd{};
        if (action == Action::PASSIVE || action == Action::MARKETABLE || action == Action::IOC) {
            const char side = below(2) ? 'B' : 'S';
            cmd.type = CommandType::ENTER_ORDER;
            cmd.orderId = allocId();
            cmd.firmId = static_cast<FirmId>(side == 'B' ? 1 + below(32) : 33 + below(32));
            cmd.buySellIndicator = side;
            if (action == Action::PASSIVE) {
                cmd.price = static_cast<Price>(side == 'B' ? 49 - below(20) : 51 + below(20));
                cmd.shares = static_cast<Quantity>(1 + below(100));
                cmd.orderType = ORDER_TYPE::LIMIT;
                ++s.passives;
            } else if (action == Action::MARKETABLE) {
                cmd.price = static_cast<Price>(side == 'B' ? 51 + below(3) : 49 - below(3));
                cmd.shares = static_cast<Quantity>(1 + below(50));
                cmd.orderType = ORDER_TYPE::LIMIT;
                ++s.marketables;
            } else {
                cmd.price = side == 'B' ? Price(60) : Price(40);
                cmd.shares = static_cast<Quantity>(1 + below(30));
                cmd.orderType = ORDER_TYPE::IMMEDIATE_OR_CANCEL;
                ++s.iocs;
            }
            submit(cmd);
            if (!live.contains(cmd.orderId)) freeIds.push_back(cmd.orderId);
        } else if (action == Action::CANCEL) {
            cmd.type = CommandType::CANCEL_ORDER;
            cmd.orderId = live.at(below(live.size()));
            ++s.cancels;
            submit(cmd);
        } else {
            cmd.type = CommandType::REPLACE_ORDER;
            cmd.orderId = live.at(below(live.size()));
            cmd.replacementOrderId = allocId();
            cmd.price = static_cast<Price>(30 + below(41));
            cmd.shares = static_cast<Quantity>(1 + below(100));
            cmd.orderType = ORDER_TYPE::LIMIT;
            ++s.replaces;
            submit(cmd);
            if (!live.contains(cmd.replacementOrderId)) freeIds.push_back(cmd.replacementOrderId);
        }
    }
    s.meanResting = restingSum / static_cast<double>(count);

    while (live.size() > 0) { // the cycle ends with an empty book
        OuchOrderCommand cmd{};
        cmd.type = CommandType::CANCEL_ORDER;
        cmd.orderId = live.at(live.size() - 1);
        ++s.drains;
        submit(cmd);
    }
    if (book->get_bid_quantity() != 0 || book->get_ask_quantity() != 0 || policy.rejects != 0) {
        std::fprintf(stderr, "the generated stream is not clean: %llu rejects, resting %u/%u\n",
            static_cast<unsigned long long>(policy.rejects), book->get_bid_quantity(), book->get_ask_quantity());
        std::exit(1);
    }
    s.events = policy.events;
    return s;
}

struct CountingPolicy {
    std::uint64_t events{ 0 };
    void on_order_event(const OrderEvent&) noexcept { ++events; }
    void on_trace_complete(const OrderTrace&) noexcept {}
};

struct Pins {
    bool enabled{ true };
    int match{ 2 }, consumer{ 4 }, producer{ 6 }, main{ 0 };
};

void pinThisThread(const Pins& pins, int cpu) {
    if (!pins.enabled) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

double seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

double percentile(std::vector<double>& values, double fraction) {
    if (values.empty()) return 0;
    const std::size_t at = std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

struct Outcome {
    std::size_t cycles{ 0 };
    std::uint64_t commands{ 0 };
    double elapsed{ 0 };
    std::vector<double> windowRates; // orders per second, one entry per full second
    std::vector<double> cycleRates;
    std::vector<double> latenciesNs; // sampled, engine variant only
};

double clockOverheadNs() {
    std::vector<double> samples;
    for (int i = 0; i < 100'000; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(b - a).count());
    }
    return percentile(samples, 0.5);
}

// The engine runs on the calling thread. `eventsSoFar`, when given, reads how many events it has emitted, for
// the per-cycle check.
template <typename Book>
Outcome runInline(Book& book, const Stream& stream, double runSeconds, bool sampleLatency, double overheadNs,
    const std::function<std::uint64_t()>& eventsSoFar) {
    Outcome out;
    const std::size_t n = stream.commands.size();
    const auto begin = Clock::now();
    auto windowStart = begin;
    std::uint64_t inWindow = 0;
    for (;;) {
        const auto cycleStart = Clock::now();
        const std::uint64_t eventsBefore = eventsSoFar ? eventsSoFar() : 0;
        for (std::size_t i = 0; i < n; ++i) {
            OuchOrderCommand cmd = stream.commands[i];
            if (sampleLatency && (i & 127) == 0) {
                const auto a = Clock::now();
                book.submit_order(cmd);
                const auto b = Clock::now();
                out.latenciesNs.push_back(std::chrono::duration<double, std::nano>(b - a).count() - overheadNs);
            } else {
                book.submit_order(cmd);
            }
            if ((i & 4095) == 4095) {
                inWindow += 4096;
                const auto now = Clock::now();
                if (seconds(windowStart, now) >= 1.0) {
                    out.windowRates.push_back(static_cast<double>(inWindow) / seconds(windowStart, now));
                    windowStart = now;
                    inWindow = 0;
                }
            }
        }
        const auto cycleEnd = Clock::now();
        out.cycleRates.push_back(static_cast<double>(n) / seconds(cycleStart, cycleEnd));
        ++out.cycles;
        out.commands += n;
        if (book.get_bid_quantity() != 0 || book.get_ask_quantity() != 0) {
            std::fprintf(stderr, "cycle %zu did not end with an empty book\n", out.cycles);
            std::exit(1);
        }
        if (eventsSoFar && eventsSoFar() - eventsBefore != stream.events) {
            std::fprintf(stderr, "cycle %zu emitted %llu events, the generator predicted %llu\n", out.cycles,
                static_cast<unsigned long long>(eventsSoFar() - eventsBefore), static_cast<unsigned long long>(stream.events));
            std::exit(1);
        }
        out.elapsed = seconds(begin, cycleEnd);
        if (out.elapsed >= runSeconds) break;
    }
    return out;
}

// Drains the event and trace queues on its own thread, as the egress publisher and telemetry forwarder do in a
// replica, but only counts what it takes.
class Consumer {
public:
    Consumer(SPSCQueue<OrderEvent, 16384>& events, SPSCQueue<OrderTrace, 16384>& traces, const Pins& pins)
        : thread_([this, &events, &traces, pins] {
              pinThisThread(pins, pins.consumer);
              OrderEvent e;
              OrderTrace t;
              std::uint64_t ne = 0;
              for (;;) {
                  bool any = false;
                  while (events.try_pop(e)) { ++ne; any = true; eventsSeen_.store(ne, std::memory_order_relaxed); }
                  while (traces.try_pop(t)) any = true;
                  if (!any) {
                      if (stop_.load(std::memory_order_acquire)) break;
                      __builtin_ia32_pause();
                  }
              }
          }) {}
    ~Consumer() { finish(); }
    std::uint64_t eventsSeen() const { return eventsSeen_.load(std::memory_order_relaxed); }
    void finish() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

private:
    std::atomic<bool> stop_{ false };
    std::atomic<std::uint64_t> eventsSeen_{ 0 };
    std::thread thread_;
};

Outcome runEngine(const Stream& stream, double runSeconds, const Pins& pins) {
    pinThisThread(pins, pins.match);
    CountingPolicy policy;
    auto book = std::make_unique<OrderBook<CountingPolicy>>(policy);
    return runInline(*book, stream, runSeconds, true, clockOverheadNs(), [&] { return policy.events; });
}

Outcome runQueues(const Stream& stream, double runSeconds, const Pins& pins) {
    auto eventQ = std::make_unique<SPSCQueue<OrderEvent, 16384>>();
    auto traceQ = std::make_unique<SPSCQueue<OrderTrace, 16384>>();
    SPSCProducerPolicy<OrderEvent> policy{ *eventQ, *traceQ };
    auto book = std::make_unique<OrderBook<SPSCProducerPolicy<OrderEvent>>>(policy);
    Consumer consumer(*eventQ, *traceQ, pins);
    pinThisThread(pins, pins.match);
    // The consumer counts asynchronously, so the events are checked once, after it has drained everything.
    Outcome out = runInline(*book, stream, runSeconds, false, 0, nullptr);
    consumer.finish();
    const std::uint64_t expected = out.cycles * stream.events;
    if (consumer.eventsSeen() != expected) {
        std::fprintf(stderr, "the consumer saw %llu events, expected %llu\n",
            static_cast<unsigned long long>(consumer.eventsSeen()), static_cast<unsigned long long>(expected));
        std::exit(1);
    }
    return out;
}

Outcome runService(const Stream& stream, double runSeconds, const Pins& pins) {
    using Policy = SPSCProducerPolicy<OrderEvent>;
    using Engine = OrderBook<Policy>;
    using Matcher = MatchingService<OuchOrderCommand, Engine>;
    auto inbound = std::make_unique<SPSCQueue<OuchOrderCommand>>();
    auto eventQ = std::make_unique<SPSCQueue<OrderEvent, 16384>>();
    auto traceQ = std::make_unique<SPSCQueue<OrderTrace, 16384>>();
    Policy policy{ *eventQ, *traceQ };
    auto book = std::make_unique<Engine>(policy);
    Matcher matcher(*inbound, *book);
    Consumer consumer(*eventQ, *traceQ, pins);

    pinThisThread(pins, pins.match);
    matcher.start(); // the matching thread inherits this thread's CPU
    pinThisThread(pins, pins.main);

    const std::size_t n = stream.commands.size();
    std::atomic<std::uint64_t> pushed{ 0 };
    std::atomic<bool> stopProducing{ false };
    std::atomic<std::size_t> cyclesDone{ 0 };
    const auto begin = Clock::now();
    std::thread producer([&] {
        pinThisThread(pins, pins.producer);
        std::uint64_t total = 0;
        while (!stopProducing.load(std::memory_order_relaxed)) {
            for (std::size_t i = 0; i < n; ++i) {
                OuchOrderCommand cmd = stream.commands[i];
                cmd.trace_id = static_cast<TraceId>(total + i);
                while (!inbound->try_push(cmd)) {}
                if ((i & 255) == 255) pushed.store(total + i + 1, std::memory_order_relaxed);
            }
            total += n;
            pushed.store(total, std::memory_order_relaxed);
            cyclesDone.fetch_add(1, std::memory_order_relaxed);
        }
    });

    Outcome out;
    std::uint64_t last = 0;
    auto lastTime = begin;
    while (seconds(begin, Clock::now()) < runSeconds) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = Clock::now();
        const std::uint64_t p = pushed.load(std::memory_order_relaxed);
        out.windowRates.push_back(static_cast<double>(p - last) / seconds(lastTime, now));
        last = p;
        lastTime = now;
    }
    stopProducing.store(true, std::memory_order_relaxed);
    producer.join();
    const std::size_t cycles = cyclesDone.load();
    const std::uint64_t total = static_cast<std::uint64_t>(cycles) * n;
    const TraceId lastTrace = static_cast<TraceId>(total - 1);
    while (!(inbound->empty() && matcher.lastAppliedSeq() == lastTrace)) { __builtin_ia32_pause(); }
    out.elapsed = seconds(begin, Clock::now());
    matcher.stop();
    consumer.finish();
    out.cycles = cycles;
    out.commands = total;
    if (book->get_bid_quantity() != 0 || book->get_ask_quantity() != 0 || consumer.eventsSeen() != cycles * stream.events) {
        std::fprintf(stderr, "service run: book not empty, or %llu events seen against %llu expected\n",
            static_cast<unsigned long long>(consumer.eventsSeen()), static_cast<unsigned long long>(cycles * stream.events));
        std::exit(1);
    }
    out.cycleRates.push_back(static_cast<double>(total) / out.elapsed);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string variant = "engine";
    std::size_t depth = 50'000;
    std::size_t commands = 4'000'000;
    double runSeconds = 30;
    Pins pins;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--variant") variant = next();
        else if (arg == "--depth") depth = std::stoul(next());
        else if (arg == "--commands") commands = std::stoul(next());
        else if (arg == "--seconds") runSeconds = std::stod(next());
        else if (arg == "--no-pin") pins.enabled = false;
        else {
            std::fprintf(stderr, "usage: EngineThroughput [--variant engine|queues|service] [--depth N] [--commands N per cycle] [--seconds S] [--no-pin]\n");
            return arg == "--help" ? 0 : 1;
        }
    }

    commands = std::max(commands, 6 * depth); // enough after the ramp for the book to sit at its depth
    const Stream stream = generate(commands, depth, 1337);
    const double total = static_cast<double>(stream.commands.size());
    auto pct = [&](std::uint64_t n) { return 100.0 * static_cast<double>(n) / total; };
    std::printf("%s: target depth %zu, mean resting %.0f, %.0f%% of commands within 10%% of it; %zu commands per cycle, %.2f events per command\n"
                "  mix: %.0f%% passive %.0f%% marketable %.0f%% ioc %.0f%% cancel %.0f%% replace (+%llu cancels to empty the book), %s\n",
        variant.c_str(), depth, stream.meanResting, pct(stream.atDepth), stream.commands.size(),
        static_cast<double>(stream.events) / total, pct(stream.passives), pct(stream.marketables), pct(stream.iocs),
        pct(stream.cancels), pct(stream.replaces), static_cast<unsigned long long>(stream.drains),
        pins.enabled ? "threads pinned" : "threads not pinned");

    Outcome out;
    if (variant == "engine") out = runEngine(stream, runSeconds, pins);
    else if (variant == "queues") out = runQueues(stream, runSeconds, pins);
    else if (variant == "service") out = runService(stream, runSeconds, pins);
    else {
        std::fprintf(stderr, "unknown variant %s\n", variant.c_str());
        return 1;
    }

    std::vector<double> windows = out.windowRates;
    std::sort(windows.begin(), windows.end());
    std::printf("cycle rates (M orders/s):");
    for (const double r : out.cycleRates) std::printf(" %.2f", r / 1e6);
    std::printf("\n");
    std::printf("SUSTAINED %-7s depth=%-7zu %zu cycles, %llu commands in %.1f s = %.2f M orders/s", variant.c_str(), depth, out.cycles,
        static_cast<unsigned long long>(out.commands), out.elapsed, static_cast<double>(out.commands) / out.elapsed / 1e6);
    if (!windows.empty()) {
        std::printf("   one-second windows: min %.2f median %.2f max %.2f", windows.front() / 1e6, windows[windows.size() / 2] / 1e6,
            windows.back() / 1e6);
    }
    std::printf("\n");
    if (!out.latenciesNs.empty()) {
        std::printf("latency per command, 1 in 128 sampled, ns: p50 %.0f p99 %.0f p99.9 %.0f p99.99 %.0f max %.0f\n",
            percentile(out.latenciesNs, 0.5), percentile(out.latenciesNs, 0.99), percentile(out.latenciesNs, 0.999),
            percentile(out.latenciesNs, 0.9999), *std::max_element(out.latenciesNs.begin(), out.latenciesNs.end()));
    }
    return 0;
}
