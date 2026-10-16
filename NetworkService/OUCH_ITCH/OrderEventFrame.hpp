#pragma once
#include "OrderEvent.hpp"

#include <array>
#include <cstdint>
#include <ostream>

// Generic egress wire frame: one shared encoding for both the private OUCH
// ack (unicast, over the originating client's TCP connection) and the public
// ITCH broadcast (multicast) — see EgressGateway.hpp, which sends the exact
// same bytes to both destinations. Mirrors the [2B BE len][1B type]<body>
// style OuchProtocolHandler uses for ingress, rather than real OUCH-style
// wire structs — those describe NASDAQ's inbound format (see
// OuchProtocolHandler.cpp), not what this project needs for its own egress
// frame.
//
// Layout: [2B BE payloadLen][1B type][8B BE order_id][1B side]
//         [1B price][4B BE quantity][4B BE leaves_quantity]
//         [8B BE match_id][1B reject_reason]
inline char EncodeOrderEventTypeByte(OrderEventType type) {
    switch (type) {
    case OrderEventType::ACCEPTED: return 'A';
    case OrderEventType::REJECTED: return 'J';
    case OrderEventType::EXECUTED: return 'E';
    case OrderEventType::CANCELED: return 'C';
    case OrderEventType::DELETED:  return 'D';
    case OrderEventType::REPLACED: return 'U';
    }
    return '?';
}

// One encoded frame, held inline so encoding never allocates. EgressGateway
// only needs data() and size().
struct OrderEventFrame {
    static constexpr size_t BODY_LEN = 8 + 1 + 1 + 4 + 4 + 8 + 1;
    static constexpr size_t SIZE = 2 + 1 + BODY_LEN;

    [[nodiscard]] const char* data() const noexcept { return bytes.data(); }
    [[nodiscard]] static constexpr size_t size() noexcept { return SIZE; }

    std::array<char, SIZE> bytes{};
};

inline OrderEventFrame EncodeOrderEventFrame(const OrderEvent& evt) {
    constexpr uint16_t payloadLen = static_cast<uint16_t>(1 + OrderEventFrame::BODY_LEN);

    OrderEventFrame frame;
    size_t off = 0;

    auto putBE16 = [&](uint16_t v) {
        frame.bytes[off++] = static_cast<char>((v >> 8) & 0xFF);
        frame.bytes[off++] = static_cast<char>(v & 0xFF);
    };
    auto putBE32 = [&](uint32_t v) {
        frame.bytes[off++] = static_cast<char>((v >> 24) & 0xFF);
        frame.bytes[off++] = static_cast<char>((v >> 16) & 0xFF);
        frame.bytes[off++] = static_cast<char>((v >> 8) & 0xFF);
        frame.bytes[off++] = static_cast<char>(v & 0xFF);
    };
    auto putBE64 = [&](uint64_t v) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.bytes[off++] = static_cast<char>((v >> shift) & 0xFF);
        }
    };

    putBE16(payloadLen);
    frame.bytes[off++] = EncodeOrderEventTypeByte(evt.type);
    putBE64(static_cast<uint64_t>(evt.order_id));
    frame.bytes[off++] = (evt.side == SIDE::BID) ? 'B' : (evt.side == SIDE::ASK) ? 'S' : ' ';
    frame.bytes[off++] = static_cast<char>(evt.price);
    putBE32(static_cast<uint32_t>(evt.quantity));
    putBE32(static_cast<uint32_t>(evt.leaves_quantity));
    putBE64(static_cast<uint64_t>(evt.match_id));
    frame.bytes[off++] = static_cast<char>(evt.reject_reason);

    return frame;
}

// Adapts the two free functions above to the TEncoder contract
// EgressGateway<TGateway, TEvent, TEncoder> requires: static encode() for
// wire bytes, static print() for the console debug line. Keeps every
// OUCH-specific field name out of EgressGateway itself.
struct OuchEventEncoder {
    static OrderEventFrame encode(const OrderEvent& evt) {
        return EncodeOrderEventFrame(evt);
    }

    static void print(std::ostream& os, const OrderEvent& evt) {
        os << "type=" << EncodeOrderEventTypeByte(evt.type)
           << " order_id=" << evt.order_id
           << " session_id=" << evt.session_id
           << " side=" << (evt.side == SIDE::BID ? 'B' : evt.side == SIDE::ASK ? 'S' : '-')
           << " price=" << static_cast<int>(evt.price)
           << " qty=" << evt.quantity
           << " leaves=" << evt.leaves_quantity
           << " match_id=" << evt.match_id
           << " reject_reason=" << static_cast<int>(evt.reject_reason);
    }
};
