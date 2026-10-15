// OUCH frame builders shared by the standalone clients in Tools/. Deliberately decoupled from
// NetworkService/MatchingService internal types (a client simulates an external party, so it
// does not reuse OuchOrderCommand): the layouts are hand-matched to the byte-accurate NASDAQ
// OUCH 4.2 structs in NetworkService/OUCH_ITCH/OUCH.hpp, wrapped in this project's own
// [2B BE payloadLen] envelope. `price` is in whole cents (this exchange's own Price domain,
// 0-100), sent as OUCH's fixed point (dollars * 10000), as OuchProtocolHandler decodes it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ouchframes {

// [1B 'O'][14B token][1B side][4B BE shares][8B symbol][4B BE price][4B BE timeInForce]
// [4B BE mpid][1B display][1B capacity][1B intermarketSweep][4B BE minimumQuantity]
// [1B crossType][1B customerType]
inline std::vector<char> BuildOuchEnterOrderFrame(const char orderToken[14], char buySellIndicator,
    uint32_t shares, const char symbol[8], uint32_t price, uint32_t timeInForce, uint32_t firmId) {
    constexpr size_t bodyLen = 14 + 1 + 4 + 8 + 4 + 4 + 4 + 1 + 1 + 1 + 4 + 1 + 1;
    constexpr uint16_t payloadLen = static_cast<uint16_t>(1 + bodyLen);
    uint32_t wirePrice = price * 100; // cents -> OUCH's dollars*10000 fixed point

    std::vector<char> frame(2 + payloadLen, '\0');
    size_t off = 0;
    frame[off++] = static_cast<char>((payloadLen >> 8) & 0xFF);
    frame[off++] = static_cast<char>(payloadLen & 0xFF);
    frame[off++] = 'O';
    std::memcpy(frame.data() + off, orderToken, 14); off += 14;
    frame[off++] = buySellIndicator;
    frame[off++] = static_cast<char>((shares >> 24) & 0xFF);
    frame[off++] = static_cast<char>((shares >> 16) & 0xFF);
    frame[off++] = static_cast<char>((shares >> 8) & 0xFF);
    frame[off++] = static_cast<char>(shares & 0xFF);
    std::memcpy(frame.data() + off, symbol, 8); off += 8;
    frame[off++] = static_cast<char>((wirePrice >> 24) & 0xFF);
    frame[off++] = static_cast<char>((wirePrice >> 16) & 0xFF);
    frame[off++] = static_cast<char>((wirePrice >> 8) & 0xFF);
    frame[off++] = static_cast<char>(wirePrice & 0xFF);
    frame[off++] = static_cast<char>((timeInForce >> 24) & 0xFF);
    frame[off++] = static_cast<char>((timeInForce >> 16) & 0xFF);
    frame[off++] = static_cast<char>((timeInForce >> 8) & 0xFF);
    frame[off++] = static_cast<char>(timeInForce & 0xFF);
    frame[off++] = static_cast<char>((firmId >> 24) & 0xFF);
    frame[off++] = static_cast<char>((firmId >> 16) & 0xFF);
    frame[off++] = static_cast<char>((firmId >> 8) & 0xFF);
    frame[off++] = static_cast<char>(firmId & 0xFF);
    // display / capacity / intermarketSweep / minimumQuantity / crossType / customerType: real OUCH
    // fields OuchProtocolHandler parses but doesn't act on yet - neutral placeholder values.
    frame[off++] = 'N'; // display
    frame[off++] = 'O'; // capacity
    frame[off++] = 'N'; // intermarketSweep
    frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; // minimumQuantity
    frame[off++] = 'N'; // crossType
    frame[off++] = ' '; // customerType
    return frame;
}

// [1B 'U'][14B existingToken][14B newToken][4B BE shares][4B BE price][4B BE timeInForce]
// [1B display][1B intermarketSweep][4B BE minimumQuantity]. Two tokens, not one reused: a token
// identifies one specific order instance.
inline std::vector<char> BuildOuchReplaceOrderFrame(const char existingToken[14], const char newToken[14],
    uint32_t shares, uint32_t price) {
    constexpr size_t bodyLen = 14 + 14 + 4 + 4 + 4 + 1 + 1 + 4;
    constexpr uint16_t payloadLen = static_cast<uint16_t>(1 + bodyLen);
    uint32_t wirePrice = price * 100;

    std::vector<char> frame(2 + payloadLen, '\0');
    size_t off = 0;
    frame[off++] = static_cast<char>((payloadLen >> 8) & 0xFF);
    frame[off++] = static_cast<char>(payloadLen & 0xFF);
    frame[off++] = 'U';
    std::memcpy(frame.data() + off, existingToken, 14); off += 14;
    std::memcpy(frame.data() + off, newToken, 14); off += 14;
    frame[off++] = static_cast<char>((shares >> 24) & 0xFF);
    frame[off++] = static_cast<char>((shares >> 16) & 0xFF);
    frame[off++] = static_cast<char>((shares >> 8) & 0xFF);
    frame[off++] = static_cast<char>(shares & 0xFF);
    frame[off++] = static_cast<char>((wirePrice >> 24) & 0xFF);
    frame[off++] = static_cast<char>((wirePrice >> 16) & 0xFF);
    frame[off++] = static_cast<char>((wirePrice >> 8) & 0xFF);
    frame[off++] = static_cast<char>(wirePrice & 0xFF);
    // timeInForce / display / intermarketSweep / minimumQuantity: parsed but discarded for a
    // REPLACE_ORDER - left zero/neutral.
    frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; // timeInForce
    frame[off++] = 'N'; // display
    frame[off++] = 'N'; // intermarketSweep
    frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; frame[off++] = 0; // minimumQuantity
    return frame;
}

// [1B 'X'][14B token][4B BE shares]
inline std::vector<char> BuildOuchCancelOrderFrame(const char orderToken[14], uint32_t shares) {
    constexpr size_t bodyLen = 14 + 4;
    constexpr uint16_t payloadLen = static_cast<uint16_t>(1 + bodyLen);

    std::vector<char> frame(2 + payloadLen);
    size_t off = 0;
    frame[off++] = static_cast<char>((payloadLen >> 8) & 0xFF);
    frame[off++] = static_cast<char>(payloadLen & 0xFF);
    frame[off++] = 'X';
    std::memcpy(frame.data() + off, orderToken, 14); off += 14;
    frame[off++] = static_cast<char>((shares >> 24) & 0xFF);
    frame[off++] = static_cast<char>((shares >> 16) & 0xFF);
    frame[off++] = static_cast<char>((shares >> 8) & 0xFF);
    frame[off++] = static_cast<char>(shares & 0xFF);
    return frame;
}

inline void PadSymbol(char out[8], const std::string& symbol) {
    std::memset(out, ' ', 8);
    std::memcpy(out, symbol.data(), std::min<size_t>(symbol.size(), 8));
}

inline uint16_t ReadBE16(const char* p) {
    return (static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]);
}
inline uint32_t ReadBE32(const char* p) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24)
        | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8)
        | static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}
inline uint64_t ReadBE64(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(p[i]);
    return v;
}

} // namespace ouchframes
