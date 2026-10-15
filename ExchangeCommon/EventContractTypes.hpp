#pragma once
#include <chrono>
#include <cstdint>

// Event-contract vocabulary. These used to live in ToyExchange's Core/Alias.hpp;
// Core is protocol- and product-agnostic, so the domain choices sit here.

using Price = std::uint8_t;
// This exchange's whole price domain: event-contract cents, 0-100 inclusive.
// Shared here since OuchProtocolHandler (NetworkService) needs the same
// bounds to validate a real OUCH price on ingress without NetworkService
// linking MatchingService; OrderBook.hpp's m_price_levels array is indexed
// directly by Price using these same bounds.
constexpr Price WORST_BID = 0;
constexpr Price WORST_ASK = 100;

// Unsigned: direction is carried by SIDE, and OUCH `shares` is uint32_t on the wire.
using Quantity = std::uint32_t;

// OUCH's OUCHEnterOrder documents shares as "> 0 and < 1,000,000". Applies to
// enter and replace; a cancel's shares == 0 ("cancel all remaining") is exempt.
constexpr Quantity MAX_ORDER_QUANTITY = 999'999;
constexpr bool is_valid_order_quantity(Quantity quantity) noexcept {
    return quantity > 0 && quantity <= MAX_ORDER_QUANTITY;
}

using OrderId = std::uint64_t;
constexpr OrderId INVALID_ORDER_ID = 0; // real ids are assigned starting at 1
// Ids are minted in [1, ORDER_ID_LIMIT). The book's lookup table has one slot per id,
// so an order with an id at or past this is rejected before it can reach the book.
constexpr OrderId ORDER_ID_LIMIT = 10'000'000;
using ExecutionId = std::uint64_t;
using FirmId = std::uint32_t;
using PoolIdx = std::uint32_t;
using Clock = std::chrono::steady_clock;

// Nanoseconds since the TAI epoch (see get_synced_time_ns() in Core's
// Telemetry.hpp). Deliberately not a std::chrono::steady_clock::time_point:
// steady_clock's epoch is implementation-defined and arbitrary per machine
// (typically boot time), so it has exactly the same cross-machine
// incomparability problem as a raw TSC value — an OrderEvent stamped on one
// machine and read/compared on another needs a real, synchronized wall-clock
// reading. (std::chrono::tai_clock, the typed C++20 equivalent, isn't
// implemented on this toolchain, hence the plain integer.)
using Timestamp = std::uint64_t;
