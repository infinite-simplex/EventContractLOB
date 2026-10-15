// In-process OrderBook benchmark: the matching engine alone, with no network, no event queue and no
// telemetry transport. Each measurement is one line of text; Tools/bench_counters.sh reads them.
//
//   sweep   one order trades through every order of a price level (depth orders of one share each)
//   single  one-share orders that each trade against the head of a level that stays about depth deep
//
// "sequential" builds the level in a fresh book, so its orders sit next to each other in the pool.
// "scattered" first fills and empties the pool in random order, so the level's orders are spread
// over the whole pool and walking it chases links through memory.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

#include "OrderBook.hpp"

namespace {

struct CountingPolicy {
    std::uint64_t events{ 0 };
    void on_order_event(const OrderEvent&) noexcept { ++events; }
    void on_trace_complete(const OrderTrace&) noexcept {}
};
using Book = OrderBook<CountingPolicy>;

constexpr FirmId BIDDER = 1;
constexpr FirmId ASKER = 2;

struct Harness {
    CountingPolicy policy;
    std::unique_ptr<Book> book{ std::make_unique<Book>(policy) };
    OrderId nextId{ 1 };

    OrderId id() {
        if (nextId >= ORDER_ID_LIMIT) nextId = 1; // only orders that are still resting need distinct ids
        return nextId++;
    }
    OrderId restAsk(Price price, Quantity quantity) {
        const OrderId orderId = id();
        Order ask(orderId, ASKER, SIDE::ASK, ORDER_TYPE::LIMIT, price, quantity);
        book->submit_order(ask);
        return orderId;
    }
    void cancelAsk(OrderId orderId) {
        Order cancel(orderId, ASKER, SIDE::ASK, ORDER_TYPE::CANCEL, 0, 0);
        book->submit_order(cancel);
    }
};

double nsSince(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

double clockOverheadNs() {
    std::vector<double> samples;
    for (int i = 0; i < 100'000; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        samples.push_back(nsSince(a, b));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

double at(std::vector<double>& sorted, double fraction) {
    return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())))];
}

void report(const char* kind, const char* layout, std::size_t depth, std::size_t samples, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    std::printf("%-6s %-10s depth=%-7zu samples=%-6zu ns/order median=%.1f p90=%.1f p99=%.1f min=%.1f max=%.1f\n", kind, layout,
        depth, samples, at(values, 0.5), at(values, 0.9), at(values, 0.99), values.front(), values.back());
}

// Fills one side's pool and empties it in random order, so the free list hands slots out in a
// random order afterwards.
void scatterPool(Harness& h) {
    std::vector<OrderId> ids;
    ids.reserve(BOOK_POOL_SIZE);
    for (std::size_t i = 0; i < BOOK_POOL_SIZE; ++i) ids.push_back(h.restAsk(100, 1));
    std::mt19937_64 rng(1337);
    std::shuffle(ids.begin(), ids.end(), rng);
    for (const OrderId orderId : ids) h.cancelAsk(orderId);
}

void sweep(const char* layout, Harness& h, std::size_t depth, std::size_t reps, double overheadNs) {
    std::vector<double> perFill;
    for (std::size_t r = 0; r < reps; ++r) {
        for (std::size_t i = 0; i < depth; ++i) h.restAsk(50, 1);
        Order bid(h.id(), BIDDER, SIDE::BID, ORDER_TYPE::LIMIT, 50, static_cast<Quantity>(depth));
        const std::uint64_t before = h.policy.events;
        const auto start = Clock::now();
        h.book->submit_order(bid);
        const auto end = Clock::now();
        if (h.policy.events - before != 2 * depth) {
            std::fprintf(stderr, "sweep depth %zu: expected %zu events, got %llu\n", depth, 2 * depth,
                static_cast<unsigned long long>(h.policy.events - before));
            std::exit(1);
        }
        perFill.push_back((nsSince(start, end) - overheadNs) / static_cast<double>(depth));
    }
    report("sweep", layout, depth, reps, std::move(perFill));
}

// One-share orders that each trade with the head of a level that stays about `depth` deep: after each
// timed batch the batch's worth of resting orders is added again, untimed. Timing a whole batch keeps the
// clock's own cost (tens of nanoseconds) out of a per-order figure of the same size.
void single(const char* layout, Harness& h, std::size_t depth, std::size_t batches, double overheadNs) {
    const std::size_t batch = std::min<std::size_t>(100, depth);
    for (std::size_t i = 0; i < depth; ++i) h.restAsk(50, 1);
    std::vector<double> perOrder;
    for (std::size_t b = 0; b < batches; ++b) {
        std::vector<Order> bids;
        bids.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) bids.emplace_back(h.id(), BIDDER, SIDE::BID, ORDER_TYPE::LIMIT, 50, 1);
        const std::uint64_t before = h.policy.events;
        const auto start = Clock::now();
        for (Order& bid : bids) h.book->submit_order(bid);
        const auto end = Clock::now();
        if (h.policy.events - before != 2 * batch) {
            std::fprintf(stderr, "single depth %zu: expected %zu events, got %llu\n", depth, 2 * batch,
                static_cast<unsigned long long>(h.policy.events - before));
            std::exit(1);
        }
        perOrder.push_back((nsSince(start, end) - overheadNs) / static_cast<double>(batch));
        for (std::size_t i = 0; i < batch; ++i) h.restAsk(50, 1);
    }
    Order rest(h.id(), BIDDER, SIDE::BID, ORDER_TYPE::LIMIT, 50, static_cast<Quantity>(depth));
    h.book->submit_order(rest); // empties the level for the next measurement
    report("single", layout, depth, batches, std::move(perOrder));
}

} // namespace

int main() {
    const double overheadNs = clockOverheadNs();
    std::printf("clock overhead subtracted from every sample: %.1f ns\n", overheadNs);

    struct Case {
        std::size_t depth;
        std::size_t reps;
    };
    const Case sweeps[] = { { 100, 2000 }, { 1000, 500 }, { 10'000, 100 }, { 100'000, 25 } };
    const Case singles[] = { { 10, 2000 }, { 100, 2000 }, { 1000, 2000 }, { 10'000, 2000 }, { 100'000, 2000 } };

    for (const bool scattered : { false, true }) {
        const char* layout = scattered ? "scattered" : "sequential";
        Harness h;
        if (scattered) scatterPool(h);
        for (const Case& c : sweeps) sweep(layout, h, c.depth, c.reps, overheadNs);
        for (const Case& c : singles) single(layout, h, c.depth, c.reps, overheadNs);
    }
    return 0;
}
