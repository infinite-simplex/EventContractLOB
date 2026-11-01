#include "OuchProtocolHandler.hpp"
#include "OUCH.hpp"

#include <iostream>
#include <iterator>
#include <string>


// ---- Wire-format helpers -------------------------------------------------
// ASSUMPTION: all multi-byte integers on the wire are big-endian.

namespace {

    uint16_t readBE16(const char* p) {
        return (static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]);
    }

    uint32_t readBE32(const char* p) {
        return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24)
            | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16)
            | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8)
            | static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
    }

    // wirePrice is OUCH's real fixed-point value (dollars * 10000, e.g.
    // $0.42 -> 4200). This exchange's Price is whole cents, 0-100
    // (WORST_BID..WORST_ASK). Since cents = dollars * 100, and the wire
    // value already is dollars * 10000, dividing by 100 recovers cents
    // exactly — an unmodified real OUCH client needs no special-casing to
    // trade here. A sub-cent remainder (wirePrice % 100 != 0) is truncated,
    // not rejected: this exchange's domain cannot represent it at all, and
    // rejecting a client for using OUCH's full fixed-point precision would
    // defeat the point of accepting real, unmodified OUCH frames.
    bool decodePrice(uint32_t wirePrice, Price& outPrice) {
        uint32_t cents = wirePrice / 100;
        if (cents > WORST_ASK) return false;
        outPrice = static_cast<Price>(cents); // narrows only after the bounds check
        return true;
    }

    // Real OUCH's ENTER_ORDER has no order-type byte — this exchange's own
    // ORDER_TYPE distinctions (LIMIT/IOC/GTD/...) are instead recovered from
    // timeInForce, per the real OUCH 4.2 sentinel table: 0 = Immediate or
    // Cancel, 99998 = "Market Hours" (auto-cancel at that day's market
    // close — functionally this exchange's own GOOD_TILL_DAY). 99999
    // ("System Hours") and 99996 ("Extended Trading Close") have no clean
    // equivalent in this exchange's ORDER_TYPE model and are deliberately
    // left unmapped rather than guessed at; every other value, including
    // those two, falls through to a plain LIMIT order — a safe, harmless
    // default, not a rejection.
    ORDER_TYPE mapTimeInForce(uint32_t timeInForce) {
        switch (timeInForce) {
        case 0:     return ORDER_TYPE::IMMEDIATE_OR_CANCEL;
        case 99998: return ORDER_TYPE::GOOD_TILL_DAY;
        default:    return ORDER_TYPE::LIMIT;
        }
    }

} // namespace

bool OuchProtocolHandler::registerToken(const char orderToken[14], OrderId id) {
    if (!m_orderTokenRegistry.insert(orderToken, id)) {
        if (!m_fullWarningGiven) {
            m_fullWarningGiven = true;
            // One string per write: the egress thread prints to the same console.
            std::cerr << ("[WARNING] Order token table is full (" + std::to_string(m_orderTokenRegistry.capacity())
                + " tokens): new orders and replaces are rejected with TOKEN_TABLE_FULL.\n");
        }
        return false;
    }

    if (m_fillWarningsGiven < std::size(FILL_WARNING_PERCENT)) {
        const size_t percent = FILL_WARNING_PERCENT[m_fillWarningsGiven];
        if (m_orderTokenRegistry.size() * 100 >= m_orderTokenRegistry.capacity() * percent) {
            ++m_fillWarningsGiven;
            std::cerr << ("[WARNING] Order token table is " + std::to_string(percent) + "% full ("
                + std::to_string(m_orderTokenRegistry.size()) + " of " + std::to_string(m_orderTokenRegistry.capacity())
                + " tokens).\n");
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Real NASDAQ OUCH 4.2 wire structs, from OUCH.hpp. Frame on the wire:
//   [2B BE payloadLen][1B msgType]<rest of that OUCH.hpp struct>
// payloadLen == sizeof() the matching struct (messageType byte included).
// The [2B len] envelope is this project's own transport framing (real OUCH
// runs over SoupBinTCP, not implemented here) — only what follows msgType
// is byte-accurate to the real spec. Each case reinterpret_casts the region
// starting at msgType onto the matching #pragma pack(1) struct, then reads
// multi-byte fields through readBE16/readBE32 for the wire's big-endianness
// (this project's own machines are little-endian); char/char[N] fields need
// no swap.
//
// Error policy, split by whether a command can even be built from the bytes:
//   - Structurally uninterpretable (wrong length, unrecognized msgType):
//     nothing downstream could parse it either. Returns PARSE_FATAL_ERROR;
//     NetworkGateway closes the connection rather than stalling on bytes
//     that will never parse.
//   - Well-formed frame, invalid content (unknown symbol, price outside
//     [WORST_BID, WORST_ASK], shares outside (0, MAX_ORDER_QUANTITY] on an
//     enter/replace, no order id left below ORDER_ID_LIMIT, no room in the token table): a real
//     OuchOrderCommand can still be built.
//     Returns totalFrameLen with type = CommandType::INVALID and flows
//     through the normal OrderBook reject pipeline — same as CANCEL_ORDER's
//     unknown-token case below, which is accepted, not rejected, at this
//     layer.
// ---------------------------------------------------------------------------

// Token look-ahead. Every token hashes to a random bucket of a table far larger than the CPU
// caches, so the lookup a frame needs is normally a trip to main memory. The gateway usually
// holds many complete frames in one read buffer, so while this one is parsed the buckets the
// next few will touch are already known: an enter inserts its token, a cancel resolves its
// token, a replace resolves the existing token and inserts the replacement. Hinting them now
// overlaps those memory trips with the parsing in between.
//
// Hints only. This reads bytes inside [0, availableLen) and changes no result, so it needs
// no validation of its own: a frame it cannot make sense of simply ends the look-ahead, and
// parseFrame() reports it when its turn comes.
void OuchProtocolHandler::prefetchAhead(const char* buf, size_t availableLen, size_t currentLen) {
    if (m_ahead == 0 || m_frontier < currentLen || m_frontier > availableLen) {
        m_ahead = 0;
        m_frontier = currentLen; // nothing prefetched yet: start right after the current frame
    }
    while (m_ahead < m_prefetchDistance) {
        if (m_frontier + 3 > availableLen) break; // need the length and the type byte
        const size_t payloadLen = readBE16(buf + m_frontier);
        const size_t total = 2 + payloadLen;
        if (m_frontier + total > availableLen) break; // that frame has not fully arrived yet
        const char* body = buf + m_frontier + 2;      // the message type byte
        switch (body[0]) {
        case 'O':
            if (payloadLen != sizeof(OUCHEnterOrder)) return;
            m_orderTokenRegistry.prefetch(reinterpret_cast<const OUCHEnterOrder*>(body)->orderToken);
            break;
        case 'X':
            if (payloadLen != sizeof(OUCHCancelOrder)) return;
            m_orderTokenRegistry.prefetch(reinterpret_cast<const OUCHCancelOrder*>(body)->orderToken);            break;
        case 'U': {
            if (payloadLen != sizeof(OUCHReplaceOrder)) return;
            const auto* wire = reinterpret_cast<const OUCHReplaceOrder*>(body);
            m_orderTokenRegistry.prefetch(wire->existingOrderToken);
            m_orderTokenRegistry.prefetch(wire->replacementOrderToken);
            break;
        }
        default:
            return; // not a frame this look-ahead understands: do not look past it
        }
        m_frontier += total;
        ++m_ahead;
    }
}

size_t OuchProtocolHandler::validateAndParse(const char* buf, size_t availableLen, OuchOrderCommand& outCmd) {
    if (m_prefetchDistance != 0 && availableLen >= 2) {
        const size_t currentLen = 2 + readBE16(buf);
        if (currentLen <= availableLen) prefetchAhead(buf, availableLen, currentLen);
    }
    const size_t consumed = parseFrame(buf, availableLen, outCmd);
    if (consumed == 0 || consumed == PARSE_FATAL_ERROR) {
        m_ahead = 0; // no complete frame left in this buffer
    } else if (m_ahead > 0) {
        if (m_frontier >= consumed) {
            m_frontier -= consumed; // the next frame is now the current one
            --m_ahead;
        } else {
            m_ahead = 0;
        }
    }
    return consumed;
}

size_t OuchProtocolHandler::parseFrame(const char* buf, size_t availableLen, OuchOrderCommand& outCmd) {
    constexpr size_t LENGTH_FIELD_SIZE = 2;

    if (availableLen < LENGTH_FIELD_SIZE) {
        return 0; // not even enough to read the length field yet
    }

    uint16_t payloadLen = readBE16(buf);
    size_t totalFrameLen = LENGTH_FIELD_SIZE + payloadLen;

    if (availableLen < totalFrameLen) {
        return 0; // full frame hasn't arrived yet
    }
    if (payloadLen < 1) {
        return PARSE_FATAL_ERROR; // no type byte at all — can't tell what this even is
    }

    char msgType = buf[LENGTH_FIELD_SIZE];

    switch (msgType) {
    case 'O': { // ENTER_ORDER
        if (payloadLen != sizeof(OUCHEnterOrder)) {
            return PARSE_FATAL_ERROR;
        }
        const OUCHEnterOrder* wire = reinterpret_cast<const OUCHEnterOrder*>(buf + LENGTH_FIELD_SIZE);

        std::memcpy(outCmd.orderToken, wire->orderToken, 14);
        outCmd.buySellIndicator = wire->buySellIndicator;
        outCmd.shares = readBE32(reinterpret_cast<const char*>(&wire->shares));

        uint16_t stockLocate = m_symbolRegistry.lookup(wire->stock);
        if (stockLocate == SymbolRegistry::INVALID_SLOT) {
            // Well-formed frame, invalid content — reject downstream rather
            // than disconnect (see error-policy note above). Bail out here
            // rather than parsing further: orderId is never minted and the
            // token is never registered, same as replace_order's own
            // UNKNOWN_ORDER reject when no real order exists.
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::UNKNOWN_SYMBOL;
            return totalFrameLen;
        }
        outCmd.stockLocate = stockLocate; // widens uint16_t -> uint32_t, fine

        uint32_t wirePrice = readBE32(reinterpret_cast<const char*>(&wire->price));
        Price decodedPrice{};
        if (!decodePrice(wirePrice, decodedPrice)) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::PRICE_OUT_OF_RANGE;
            return totalFrameLen;
        }
        outCmd.price = decodedPrice;

        if (!is_valid_order_quantity(outCmd.shares)) { // real OUCH: shares > 0 and < 1,000,000
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::INVALID_QUANTITY;
            return totalFrameLen;
        }

        uint32_t timeInForce = readBE32(reinterpret_cast<const char*>(&wire->timeInForce));
        outCmd.orderType = mapTimeInForce(timeInForce);

        // mpid is a 4-char ASCII firm code, not a numeric id — pack it into
        // a stable integer key the same way SymbolRegistry already packs an
        // 8-byte ticker, rather than standing up a separate firm registry.
        outCmd.firmId = readBE32(wire->mpid);

        // display, capacity, intermarketSweep, minimumQuantity, crossType,
        // customerType are real OUCH fields with no engine feature to
        // consume them yet — intentionally not read. reinterpret_cast means
        // there's no manual offset to "skip past" them either.

        if (m_nextOrderId >= ORDER_ID_LIMIT) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::ORDER_LIMIT_REACHED;
            return totalFrameLen;
        }
        if (!registerToken(outCmd.orderToken, m_nextOrderId)) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::TOKEN_TABLE_FULL;
            return totalFrameLen;
        }
        outCmd.orderId = m_nextOrderId++;

        outCmd.type = CommandType::ENTER_ORDER;
        return totalFrameLen;
    }

    case 'X': { // CANCEL_ORDER
        if (payloadLen != sizeof(OUCHCancelOrder)) {
            return PARSE_FATAL_ERROR;
        }
        const OUCHCancelOrder* wire = reinterpret_cast<const OUCHCancelOrder*>(buf + LENGTH_FIELD_SIZE);

        std::memcpy(outCmd.orderToken, wire->orderToken, 14);
        outCmd.shares = readBE32(reinterpret_cast<const char*>(&wire->shares)); // 0 = cancel all remaining

        // Not found (INVALID_ORDER_ID) is accepted, not rejected: a cancel
        // racing a fill/earlier cancel is routine, not a protocol error.
        // OrderBook's cancel path already no-ops safely on an unknown id.
        outCmd.orderId = m_orderTokenRegistry.resolve(outCmd.orderToken);

        outCmd.price = 0;
        outCmd.stockLocate = 0;
        outCmd.buySellIndicator = '\0';
        outCmd.firmId = 0;
        outCmd.type = CommandType::CANCEL_ORDER;
        return totalFrameLen;
    }

    case 'U': { // REPLACE_ORDER
        if (payloadLen != sizeof(OUCHReplaceOrder)) {
            return PARSE_FATAL_ERROR;
        }
        const OUCHReplaceOrder* wire = reinterpret_cast<const OUCHReplaceOrder*>(buf + LENGTH_FIELD_SIZE);

        // Not found (INVALID_ORDER_ID) is accepted, not rejected here — the
        // same reasoning as CANCEL_ORDER: OrderBook rejects a replace that
        // references an unknown order, this layer just resolves the token.
        outCmd.orderId = m_orderTokenRegistry.resolve(wire->existingOrderToken);

        std::memcpy(outCmd.orderToken, wire->replacementOrderToken, 14); // the NEW token

        outCmd.shares = readBE32(reinterpret_cast<const char*>(&wire->shares));

        uint32_t wirePrice = readBE32(reinterpret_cast<const char*>(&wire->price));
        Price decodedPrice{};
        if (!decodePrice(wirePrice, decodedPrice)) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::PRICE_OUT_OF_RANGE;
            return totalFrameLen;
        }
        outCmd.price = decodedPrice;

        // A replace can't resize an order to nothing (that's a cancel), and
        // is bound by the same upper limit as an enter.
        if (!is_valid_order_quantity(outCmd.shares)) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::INVALID_QUANTITY;
            return totalFrameLen;
        }

        // timeInForce/display/intermarketSweep/minimumQuantity are present
        // in the real struct but not read: OrderBook::replace_order never
        // consults cmd.orderType (it keeps the existing order's own type),
        // so there is nothing to map them to here.

        // Eagerly minted and registered, same as ENTER_ORDER, even though
        // the replace might still be rejected by OrderBook (e.g. the
        // existing order wasn't found) — harmless either way, since an id
        // that was never actually inserted just safely no-ops on any later
        // lookup, same tolerant pattern OrderTokenRegistry already documents.
        if (m_nextOrderId >= ORDER_ID_LIMIT) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::ORDER_LIMIT_REACHED;
            return totalFrameLen;
        }
        if (!registerToken(outCmd.orderToken, m_nextOrderId)) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = RejectReason::TOKEN_TABLE_FULL;
            return totalFrameLen;
        }
        outCmd.replacementOrderId = m_nextOrderId++;

        outCmd.stockLocate = 0;
        outCmd.buySellIndicator = '\0';
        outCmd.firmId = 0;
        outCmd.type = CommandType::REPLACE_ORDER;
        return totalFrameLen;
    }

    default:
        return PARSE_FATAL_ERROR;
    }
}
