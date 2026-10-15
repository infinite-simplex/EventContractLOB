// Standalone OUCH test client: connects to a running NetworkServiceApp's
// OUCH TCP listener and sends one or more ENTER_ORDER frames. Deliberately
// decoupled from NetworkService/MatchingService internal types (it's meant
// to simulate an external client, not reuse OuchOrderCommand) — the frame
// builders are in ../OuchFrames.hpp, hand-matched to the real, byte-accurate
// NASDAQ OUCH 4.2 structs in NetworkService/OUCH_ITCH/OUCH.hpp (see
// OuchProtocolHandler.cpp for the parser that decodes these same bytes).
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../OuchFrames.hpp"

namespace {
using namespace ouchframes;

// 14-byte order token, unique per call within a process run.
void MakeToken(char out[14], uint32_t counter) {
    std::memset(out, ' ', 14);
    std::string text = "T" + std::to_string(counter);
    std::memcpy(out, text.data(), std::min<size_t>(text.size(), 14));
}

// Decodes the egress frame NetworkService/OrderEventFrame.hpp encodes:
// [2B BE len][1B type][8B BE order_id][1B side][1B price]
// [4B BE quantity][4B BE leaves_quantity][8B BE match_id][1B reject_reason]
// Duplicated here rather than shared, matching this tool's "deliberately
// decoupled from NetworkService" philosophy (see file header comment).
void PrintOuchResponse(const char* body, size_t len) {
    if (len != 8 + 1 + 1 + 4 + 4 + 8 + 1) {
        std::cout << "  (unrecognized response body length " << len << ")\n";
        return;
    }
    size_t off = 0;
    uint64_t orderId = ReadBE64(body + off); off += 8;
    char side = body[off++];
    uint8_t price = static_cast<uint8_t>(body[off++]);
    uint32_t qty = ReadBE32(body + off); off += 4;
    uint32_t leaves = ReadBE32(body + off); off += 4;
    uint64_t matchId = ReadBE64(body + off); off += 8;
    uint8_t rejectReason = static_cast<uint8_t>(body[off++]);

    std::cout << "  order_id=" << orderId << " side=" << side
              << " price=" << static_cast<int>(price) << " qty=" << qty
              << " leaves=" << leaves << " match_id=" << matchId
              << " reject_reason=" << static_cast<int>(rejectReason) << "\n";
}

// Reads and decodes any OUCH-ack frames that arrive on `sock` within the
// next `listenMs` milliseconds. Best-effort: prints raw bytes if a frame
// doesn't parse cleanly rather than failing.
void ListenForResponses(int sock, int listenMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(listenMs);
    std::vector<char> buf;
    char chunk[512];

    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(remaining.count() / 1'000'000);
        tv.tv_usec = static_cast<suseconds_t>(remaining.count() % 1'000'000);

        int ready = select(sock + 1, &readfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) break; // server closed the connection
        buf.insert(buf.end(), chunk, chunk + n);

        while (buf.size() >= 2) {
            uint16_t payloadLen = ReadBE16(buf.data());
            size_t totalFrameLen = 2 + payloadLen;
            if (buf.size() < totalFrameLen) break;

            char type = buf[2];
            std::cout << "Received OUCH response: type=" << type << "\n";
            PrintOuchResponse(buf.data() + 3, payloadLen - 1);

            buf.erase(buf.begin(), buf.begin() + static_cast<long>(totalFrameLen));
        }
    }
}

void PrintUsage() {
    std::cerr <<
        "Usage: OuchTestClient --side B|S --price N --qty N [options]\n"
        "  --host <ip>            default 127.0.0.1\n"
        "  --port <port>          default 10001 (NetworkConfig::ingressListenPort)\n"
        "  --symbol <name>        default TEST, space-padded/truncated to 8 bytes\n"
        "  --side B|S             required for a new order (not for --replace-token)\n"
        "  --price <0-100>        required, whole cents (this exchange's Price\n"
        "                         domain) — sent on the wire as real OUCH's\n"
        "                         fixed-point price (dollars * 10000)\n"
        "  --qty <shares>         required\n"
        "  --time-in-force <n>    real OUCH field, default 1 (plain LIMIT).\n"
        "                         0 = Immediate or Cancel, 99998 = Market Hours\n"
        "                         (auto-cancel at close — this exchange's own\n"
        "                         GOOD_TILL_DAY); any other value is a plain\n"
        "                         LIMIT order — see OuchProtocolHandler.cpp.\n"
        "  --firm-id <n>          default 0. Self-trade prevention rejects two\n"
        "                         orders sharing a firm id that would otherwise\n"
        "                         cross each other — use different --firm-id\n"
        "                         values across invocations to let them match.\n"
        "  --token <text>         default: auto-generated, unique per order sent.\n"
        "                         With --replace-token, this is the NEW token.\n"
        "  --replace-token <text> send a REPLACE_ORDER instead of ENTER_ORDER,\n"
        "                         replacing the order currently identified by\n"
        "                         this existing token with one at --price/--qty\n"
        "                         (identified afterward by --token, or an\n"
        "                         auto-generated one if --token isn't given).\n"
        "                         Ignores --side/--time-in-force/--firm-id/--count,\n"
        "                         which don't apply to a replace.\n"
        "  --cancel-token <text>  send a CANCEL_ORDER instead of ENTER_ORDER,\n"
        "                         canceling the order currently identified by\n"
        "                         this token. Ignores --side/--price/--time-in-force/\n"
        "                         --firm-id/--count/--replace-token.\n"
        "  --count <n>            send n orders over one connection, default 1\n"
        "  --interval-ms <ms>     delay between sends when --count > 1, default 0\n"
        "  --listen-ms <ms>       after sending, keep the connection open and\n"
        "                         print any OUCH ack frames received for this\n"
        "                         long (default 0 = close immediately)\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 10001;
    std::string symbolStr = "TEST";
    char side = 0;
    long price = -1;
    long qty = -1;
    uint32_t timeInForce = 1; // real OUCH field; 1 falls through to a plain LIMIT order
    uint32_t firmId = 0;
    std::string tokenOverride;
    std::string replaceToken;
    std::string cancelToken;
    int count = 1;
    int intervalMs = 0;
    int listenMs = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
            return argv[++i];
        };

        if (arg == "--host") host = next();
        else if (arg == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--symbol") symbolStr = next();
        else if (arg == "--side") side = next()[0];
        else if (arg == "--price") price = std::stol(next());
        else if (arg == "--qty") qty = std::stol(next());
        else if (arg == "--time-in-force") timeInForce = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--firm-id") firmId = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--token") tokenOverride = next();
        else if (arg == "--replace-token") replaceToken = next();
        else if (arg == "--cancel-token") cancelToken = next();
        else if (arg == "--count") count = std::stoi(next());
        else if (arg == "--interval-ms") intervalMs = std::stoi(next());
        else if (arg == "--listen-ms") listenMs = std::stoi(next());
        else if (arg == "--help") { PrintUsage(); return 0; }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage();
            return 1;
        }
    }

    bool isReplace = !replaceToken.empty();
    bool isCancel = !cancelToken.empty();
    if (isReplace && isCancel) {
        std::cerr << "--replace-token and --cancel-token are mutually exclusive.\n";
        return 1;
    }
    bool needsPriceAndSide = !isReplace && !isCancel;
    if ((needsPriceAndSide && side != 'B' && side != 'S') || (!isCancel && (price < 0 || price > 100))
        || (!isCancel && qty <= 0)) {
        std::cerr << "Missing/invalid required arguments.\n";
        PrintUsage();
        return 1;
    }
    if (isReplace && replaceToken.size() > 14) {
        std::cerr << "--replace-token must be at most 14 bytes.\n";
        return 1;
    }
    if (isCancel && cancelToken.size() > 14) {
        std::cerr << "--cancel-token must be at most 14 bytes.\n";
        return 1;
    }

    char symbol[8];
    PadSymbol(symbol, symbolStr);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) throw std::runtime_error("Unable to create socket");

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) != 1) {
        std::cerr << "Invalid --host: " << host << "\n";
        return 1;
    }
    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Unable to connect to " << host << ":" << port << "\n";
        close(sock);
        return 1;
    }

    if (isReplace) {
        char existingTokenBuf[14];
        std::memset(existingTokenBuf, ' ', 14);
        std::memcpy(existingTokenBuf, replaceToken.data(), replaceToken.size());

        char newToken[14];
        if (!tokenOverride.empty()) {
            std::memset(newToken, ' ', 14);
            std::memcpy(newToken, tokenOverride.data(), std::min<size_t>(tokenOverride.size(), 14));
        } else {
            MakeToken(newToken, static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        }

        auto frame = BuildOuchReplaceOrderFrame(existingTokenBuf, newToken,
            static_cast<uint32_t>(qty), static_cast<uint32_t>(price));

        ssize_t sent = send(sock, frame.data(), frame.size(), 0);
        if (sent != static_cast<ssize_t>(frame.size())) {
            std::cerr << "Failed to send REPLACE_ORDER frame\n";
            close(sock);
            return 1;
        }

        std::cout << "Sent REPLACE_ORDER: existing_token=" << replaceToken
                  << " price=" << price << " qty=" << qty << "\n";
    } else if (isCancel) {
        char cancelTokenBuf[14];
        std::memset(cancelTokenBuf, ' ', 14);
        std::memcpy(cancelTokenBuf, cancelToken.data(), cancelToken.size());

        auto frame = BuildOuchCancelOrderFrame(cancelTokenBuf, qty > 0 ? static_cast<uint32_t>(qty) : 0);

        ssize_t sent = send(sock, frame.data(), frame.size(), 0);
        if (sent != static_cast<ssize_t>(frame.size())) {
            std::cerr << "Failed to send CANCEL_ORDER frame\n";
            close(sock);
            return 1;
        }

        std::cout << "Sent CANCEL_ORDER: token=" << cancelToken << "\n";
    } else {
        for (int i = 0; i < count; ++i) {
            char token[14];
            if (!tokenOverride.empty() && count == 1) {
                std::memset(token, ' ', 14);
                std::memcpy(token, tokenOverride.data(), std::min<size_t>(tokenOverride.size(), 14));
            } else {
                MakeToken(token, static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() ^ i));
            }

            auto frame = BuildOuchEnterOrderFrame(token, side, static_cast<uint32_t>(qty), symbol,
                static_cast<uint32_t>(price), timeInForce, firmId);

            ssize_t sent = send(sock, frame.data(), frame.size(), 0);
            if (sent != static_cast<ssize_t>(frame.size())) {
                std::cerr << "Failed to send frame " << i << "\n";
                close(sock);
                return 1;
            }

            std::cout << "Sent ENTER_ORDER: side=" << side << " price=" << price
                      << " qty=" << qty << " symbol=" << symbolStr << " firm_id=" << firmId << "\n";

            if (i + 1 < count && intervalMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            }
        }
    }

    if (listenMs > 0) {
        std::cout << "Listening for responses for " << listenMs << "ms...\n";
        ListenForResponses(sock, listenMs);
    }

    close(sock);
    return 0;
}
