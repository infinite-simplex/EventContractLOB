#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <sys/mman.h>
#include "Alias.hpp"
#include "EventContractTypes.hpp"

// Network-layer orderToken -> OrderId resolution, structurally identical to
// SymbolRegistry (fixed bucket array, linear probing, multiplicative hash) —
// just keyed on the wider 14-byte orderToken instead of an 8-byte ticker, and
// (unlike SymbolRegistry's much smaller table) heap-allocated rather than an
// inline member array: at ~2^21 slots this is ~50MB, too big to safely live
// on the stack wherever this type gets declared as a local/member (same class
// of bug as BookSide's order pools, which moved off the stack for the same
// reason — see the comment above BookSide's m_order_pool). One allocation at
// construction; insert()/resolve() themselves stay allocation-free. Owned by
// OuchProtocolHandler, touched only from the single ingress thread (same
// thread that already does symbol resolution), so no synchronization is
// needed.
//
// Append-only by design: nothing ever removes an entry. That avoids a second
// writer thread ever touching this table (the matching thread never sees it),
// at the cost of a bounded lifetime capacity. A stale entry left behind by an
// order that later filled or was canceled just resolves to an id that safely
// no-ops downstream (OrderBook::cancel already tolerates unknown ids), exactly
// like SymbolRegistry already tolerates unknown lookups.
//
// Registrations stop at 7/8 of the table and insert() then returns false (the
// handler rejects the order with TOKEN_TABLE_FULL). Past that load a linear
// probe takes milliseconds, which stalls the whole gateway, and a table that is
// never allowed to fill also guarantees every probe ends at an empty slot.
//
// Memory behavior, which dominates this table's cost: every token hashes to a
// random bucket, so at full size nearly every lookup misses the CPU caches and
// the TLB. Two things address that, neither of which changes what insert() and
// resolve() return:
//   - A table of 2 MiB or more is placed on 2 MiB-aligned memory and the kernel
//     is asked (madvise) to back it with huge pages, so a lookup needs far fewer
//     page-table walks. The request is advisory: when the kernel declines, the
//     table works the same on ordinary pages.
//   - prefetch() lets the caller start loading a bucket before it is needed, so
//     the miss overlaps with other work. OuchProtocolHandler calls it for the
//     frames queued behind the one it is parsing.
class OrderTokenRegistry {
public:
    // bucketCount must be a power of two of at least 8.
    explicit OrderTokenRegistry(size_t bucketCount = DEFAULT_BUCKET_COUNT)
        : m_mask(bucketCount - 1), m_maxEntries(bucketCount / 8 * 7), m_table(allocate(bucketCount)) {}

    // Hints the CPU to start loading the bucket orderToken hashes to. Advisory only:
    // it never changes what insert() or resolve() do, and it is safe for a token that
    // is not registered. A slot can straddle two cache lines, so both are hinted.
    void prefetch(const char orderToken[14]) const noexcept {
        const Slot* slot = &m_table[hash(make_key(orderToken)) & m_mask];
        __builtin_prefetch(slot);
        __builtin_prefetch(reinterpret_cast<const char*>(slot) + sizeof(Slot) - 1);
    }

    // Registers (or, on a reused token, re-points) orderToken -> id. Returns
    // false, registering nothing, when the token is new and the table is at its
    // limit. Called only for ENTER_ORDER and REPLACE_ORDER frames.
    [[nodiscard]] bool insert(const char orderToken[14], OrderId id) noexcept {
        TokenKey key = make_key(orderToken);
        size_t bucket = hash(key) & m_mask;

        while (true) {
            Slot& slot = m_table[bucket];
            if (slot.id == INVALID_ORDER_ID) {
                if (m_size >= m_maxEntries) return false;
                slot.key = key;
                slot.id = id;
                ++m_size;
                return true;
            }
            if (slot.key.hi == key.hi && slot.key.lo == key.lo) {
                slot.id = id;
                return true;
            }
            bucket = (bucket + 1) & m_mask;
        }
    }

    // CRITICAL HOT PATH METHOD: resolves orderToken -> OrderId, or
    // INVALID_ORDER_ID if never registered.
    [[nodiscard]] OrderId resolve(const char orderToken[14]) const noexcept {
        TokenKey key = make_key(orderToken);
        size_t bucket = hash(key) & m_mask;

        while (true) {
            const Slot& slot = m_table[bucket];
            if (slot.id == INVALID_ORDER_ID) {
                return INVALID_ORDER_ID; // empty slot — not registered
            }
            if (slot.key.hi == key.hi && slot.key.lo == key.lo) {
                return slot.id;
            }
            bucket = (bucket + 1) & m_mask;
        }
    }

    [[nodiscard]] size_t size() const noexcept { return m_size; }
    [[nodiscard]] size_t capacity() const noexcept { return m_maxEntries; }

private:
    struct TokenKey {
        uint64_t hi; // first 8 bytes of the 14-byte token
        uint64_t lo; // last 6 bytes, zero-extended into 8
    };

    struct Slot {
        TokenKey key{ 0, 0 };
        OrderId id{ INVALID_ORDER_ID }; // INVALID_ORDER_ID (0) marks an empty slot
    };

    static inline TokenKey make_key(const char orderToken[14]) noexcept {
        TokenKey key{ 0, 0 };
        std::memcpy(&key.hi, orderToken, 8);
        std::memcpy(&key.lo, orderToken + 8, 6);
        return key;
    }

    // Same multiplicative mixing SymbolRegistry uses, applied to both halves.
    static inline uint64_t hash64(uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    static inline uint64_t hash(const TokenKey& key) noexcept {
        return hash64(key.hi) ^ hash64(key.lo);
    }

    // Power of 2 for cheap bitwise masking instead of modulo. Holds 1,835,008
    // tokens, which is about 2.3 million orders when 80% of them mint a token.
    static constexpr size_t DEFAULT_BUCKET_COUNT = 2'097'152; // 2^21

    static constexpr size_t HUGE_PAGE_BYTES = 2u << 20;

    // An all-zero Slot is an empty one (id == INVALID_ORDER_ID == 0), which is what lets the huge-page
    // path fill the table with memset instead of constructing each slot.
    static_assert(INVALID_ORDER_ID == 0 && std::is_aggregate_v<Slot> && std::is_trivially_copyable_v<Slot>);

    // The two allocation paths free differently, so the table remembers how to free itself.
    using TablePtr = std::unique_ptr<Slot[], void (*)(Slot*)>;

    static TablePtr allocate(size_t bucketCount) {
        const size_t bytes = bucketCount * sizeof(Slot);
        if (bytes < HUGE_PAGE_BYTES) {
            return TablePtr(new Slot[bucketCount](), [](Slot* p) { delete[] p; });
        }
        const size_t rounded = (bytes + HUGE_PAGE_BYTES - 1) / HUGE_PAGE_BYTES * HUGE_PAGE_BYTES;
        void* memory = std::aligned_alloc(HUGE_PAGE_BYTES, rounded);
        if (!memory) throw std::bad_alloc();
        madvise(memory, rounded, MADV_HUGEPAGE); // advisory: the table works on ordinary pages if the kernel declines
        std::memset(memory, 0, rounded);         // zero, and touch every page now rather than on the hot path
        return TablePtr(static_cast<Slot*>(memory), [](Slot* p) { std::free(p); });
    }

    size_t m_mask;
    size_t m_maxEntries;
    size_t m_size{ 0 };
    TablePtr m_table;
};
