#pragma once
#include <cstring>
#include <cstdint>
#include "SymbolRegistry.hpp"
#include "OrderTokenRegistry.hpp"
#include "OuchOrderCommand.hpp"

class OuchProtocolHandler {
public:
    // Sentinel return from validateAndParse, distinct from 0 ("not enough
    // bytes yet — wait for more") and any real byte count: the buffer holds
    // a COMPLETE frame that is structurally uninterpretable (bad body
    // length for its type, or an unrecognized message type byte) — no
    // OuchOrderCommand can be built from it at all, and nothing about
    // future bytes on this connection can be trusted either. The caller
    // (NetworkGateway) is expected to close the connection, not retry.
    static constexpr size_t PARSE_FATAL_ERROR = static_cast<size_t>(-1);

    // How many complete frames behind the one being parsed have their token-table buckets prefetched.
    // 8 is what measured best end to end; see prefetchAhead() in OuchProtocolHandler.cpp.
    static constexpr unsigned DEFAULT_PREFETCH_DISTANCE = 8;
    static constexpr unsigned MAX_PREFETCH_DISTANCE = 32;

    // firstOrderId is only ever not 1 in a test that needs to reach ORDER_ID_LIMIT.
    // prefetchDistance 0 turns the look-ahead off; a test uses that to check it changes nothing.
    OuchProtocolHandler(const SymbolRegistry& symbolRegistry, OrderTokenRegistry& orderTokenRegistry,
        OrderId firstOrderId = 1, unsigned prefetchDistance = DEFAULT_PREFETCH_DISTANCE)
        : m_symbolRegistry(symbolRegistry), m_orderTokenRegistry(orderTokenRegistry), m_nextOrderId(firstOrderId),
          m_prefetchDistance(prefetchDistance < MAX_PREFETCH_DISTANCE ? prefetchDistance : MAX_PREFETCH_DISTANCE) {}
    size_t validateAndParse(const char* buf, size_t availableLen, OuchOrderCommand& outCmd);
private:
    // The parse itself. validateAndParse() runs the token look-ahead around it.
    size_t parseFrame(const char* buf, size_t availableLen, OuchOrderCommand& outCmd);

    // Prefetches the token-table buckets of the complete frames queued behind the current one.
    void prefetchAhead(const char* buf, size_t availableLen, size_t currentLen);

    // Registers the token. False, with nothing registered, when the token table is full.
    bool registerToken(const char orderToken[14], OrderId id);

    static constexpr uint8_t FILL_WARNING_PERCENT[2] = { 50, 80 };

    const SymbolRegistry& m_symbolRegistry;
    OrderTokenRegistry& m_orderTokenRegistry; // single ingress thread only — see OrderTokenRegistry.hpp
    OrderId m_nextOrderId;                    // incremented only for ENTER_ORDER and REPLACE_ORDER frames
    size_t m_fillWarningsGiven{ 0 };
    bool m_fullWarningGiven{ false };

    // Look-ahead state, relative to the CURRENT frame (the one at buf[0]) so it survives the gateway
    // shifting the buffer down after every frame. m_ahead frames after the current one have been
    // prefetched; m_frontier is the byte offset, from the current frame's start, of the first frame
    // not yet prefetched. Everything here is a hint: stale values can only waste a prefetch.
    unsigned m_prefetchDistance;
    unsigned m_ahead{ 0 };
    size_t m_frontier{ 0 };
};