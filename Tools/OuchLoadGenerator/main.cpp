// Open-loop OUCH load generator for a running NetworkServiceApp. It sends a seeded, reproducible mix of
// orders at a fixed rate over one TCP connection, in 1 ms bursts, and reads the private acks on a second
// thread so the gateway's egress never stalls on a full socket. Sending does not wait for acks, so a
// slow exchange shows up as a lower achieved rate, not as a slower sender.
//
// The mix (by draw): 55% limit orders priced within a few ticks of 50 (about one in six crosses the
// spread), 15% IOC orders that cross up to ten ticks, 20% cancels and 10% replaces of tokens drawn from
// the last 50,000 limit orders (many of them already filled or cancelled, as in a real stream). Bids use
// firm ids 1-32 and asks 33-64, so self-trade prevention never ends a match early.
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../OuchFrames.hpp"

namespace {

using namespace ouchframes;
using Token = std::array<char, 14>;

constexpr size_t LIVE_RING = 50'000;

void printUsage() {
    std::cerr <<
        "Usage: OuchLoadGenerator [options]\n"
        "  --host <ip>          default 127.0.0.1\n"
        "  --port <port>        default 10001\n"
        "  --symbol <name>      default TEST\n"
        "  --rate <orders/s>    default 10000\n"
        "  --duration-s <s>     default 30\n"
        "  --seed <n>           default 1337\n"
        "  --drain-ms <ms>      keep reading acks this long after the last order, default 1000\n";
}

Token makeToken(uint64_t n) {
    Token t;
    char text[15];
    std::snprintf(text, sizeof text, "G%013llu", static_cast<unsigned long long>(n));
    std::memcpy(t.data(), text, 14);
    return t;
}

struct Counts {
    uint64_t limit{ 0 }, ioc{ 0 }, cancel{ 0 }, replace{ 0 };
};

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 10001;
    std::string symbolStr = "TEST";
    double rate = 10000;
    double durationS = 30;
    uint64_t seed = 1337;
    int drainMs = 1000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--host") host = next();
        else if (arg == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--symbol") symbolStr = next();
        else if (arg == "--rate") rate = std::stod(next());
        else if (arg == "--duration-s") durationS = std::stod(next());
        else if (arg == "--seed") seed = std::stoull(next());
        else if (arg == "--drain-ms") drainMs = std::stoi(next());
        else if (arg == "--help") { printUsage(); return 0; }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage();
            return 1;
        }
    }
    if (rate <= 0 || durationS <= 0) {
        std::cerr << "--rate and --duration-s must be positive.\n";
        return 1;
    }

    char symbol[8];
    PadSymbol(symbol, symbolStr);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) throw std::runtime_error("Unable to create socket");
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) != 1) {
        std::cerr << "Invalid --host: " << host << "\n";
        return 1;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof serverAddr) < 0) {
        std::cerr << "Unable to connect to " << host << ":" << port << "\n";
        close(sock);
        return 1;
    }

    std::map<char, uint64_t> acks;
    std::thread reader([&] {
        std::vector<char> buf;
        char chunk[4096];
        for (;;) {
            ssize_t n = recv(sock, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            buf.insert(buf.end(), chunk, chunk + n);
            size_t at = 0;
            while (buf.size() - at >= 3) {
                const size_t frameLen = 2 + ReadBE16(buf.data() + at);
                if (buf.size() - at < frameLen) break;
                ++acks[buf[at + 2]];
                at += frameLen;
            }
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
        }
    });

    std::mt19937_64 rng(seed);
    auto below = [&](uint64_t n) { return rng() % n; };
    std::vector<Token> live(LIVE_RING);
    std::vector<char> liveSide(LIVE_RING);
    size_t liveCount = 0;
    uint64_t tokenCounter = 0;
    Counts counts;

    auto firmFor = [&](char side) { return static_cast<uint32_t>(side == 'B' ? 1 + below(32) : 33 + below(32)); };
    auto sendLimit = [&](std::vector<char>& out) {
        const char side = below(2) ? 'B' : 'S';
        const int k = static_cast<int>(below(14)) - 3; // -3..10 ticks from 50; negative crosses
        const uint32_t price = static_cast<uint32_t>(side == 'B' ? 50 - k : 50 + k);
        const Token t = makeToken(tokenCounter++);
        const auto frame = BuildOuchEnterOrderFrame(t.data(), side, static_cast<uint32_t>(1 + below(100)), symbol, price, 1, firmFor(side));
        out.insert(out.end(), frame.begin(), frame.end());
        live[liveCount % LIVE_RING] = t;
        liveSide[liveCount % LIVE_RING] = side;
        ++liveCount;
        ++counts.limit;
    };

    const auto start = std::chrono::steady_clock::now();
    uint64_t sent = 0;
    std::vector<char> burst;
    for (;;) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= durationS) break;
        const uint64_t due = static_cast<uint64_t>(elapsed * rate);
        burst.clear();
        for (; sent < due; ++sent) {
            const uint64_t draw = below(100);
            const size_t known = std::min(liveCount, LIVE_RING);
            if (draw < 55 || known == 0) {
                sendLimit(burst);
            } else if (draw < 70) {
                const char side = below(2) ? 'B' : 'S';
                const Token t = makeToken(tokenCounter++);
                const auto frame = BuildOuchEnterOrderFrame(t.data(), side, static_cast<uint32_t>(1 + below(300)), symbol,
                    side == 'B' ? 60u : 40u, 0, firmFor(side));
                burst.insert(burst.end(), frame.begin(), frame.end());
                ++counts.ioc;
            } else if (draw < 90) {
                const auto frame = BuildOuchCancelOrderFrame(live[below(known)].data(), 0);
                burst.insert(burst.end(), frame.begin(), frame.end());
                ++counts.cancel;
            } else {
                const size_t slot = below(known);
                const Token t = makeToken(tokenCounter++);
                const auto frame = BuildOuchReplaceOrderFrame(live[slot].data(), t.data(), static_cast<uint32_t>(1 + below(100)),
                    static_cast<uint32_t>(40 + below(21)));
                burst.insert(burst.end(), frame.begin(), frame.end());
                live[slot] = t;
                ++counts.replace;
            }
        }
        for (size_t off = 0; off < burst.size();) {
            const ssize_t n = send(sock, burst.data() + off, burst.size() - off, 0);
            if (n <= 0) {
                std::cerr << "send failed after " << sent << " orders\n";
                shutdown(sock, SHUT_RDWR);
                reader.join();
                close(sock);
                return 1;
            }
            off += static_cast<size_t>(n);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double sendSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::this_thread::sleep_for(std::chrono::milliseconds(drainMs));
    shutdown(sock, SHUT_RDWR);
    reader.join();
    close(sock);

    std::printf("sent %llu orders in %.2f s = %.0f orders/s (target %.0f): %llu limit, %llu ioc, %llu cancel, %llu replace\n",
        static_cast<unsigned long long>(sent), sendSeconds, static_cast<double>(sent) / sendSeconds, rate,
        static_cast<unsigned long long>(counts.limit), static_cast<unsigned long long>(counts.ioc),
        static_cast<unsigned long long>(counts.cancel), static_cast<unsigned long long>(counts.replace));
    std::printf("acks received:");
    for (const auto& [type, n] : acks) std::printf(" %c=%llu", type, static_cast<unsigned long long>(n));
    std::printf("\n");
    return 0;
}
