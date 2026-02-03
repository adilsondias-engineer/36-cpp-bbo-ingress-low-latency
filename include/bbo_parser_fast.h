#pragma once

#include "bbo_data.h"
#include "bbo_pool.h"
#include "likely.h"
#include <cstring>
#include <cstdint>

namespace ultra_ll {

// Compile-time constants for BBO parsing
// Using multiplication instead of division for better performance
constexpr double PRICE_MULTIPLIER = 0.0001;  // 1/10000 for 4 decimal places
constexpr size_t BBO_MIN_SIZE = 28;          // Symbol(8) + prices/shares(20)
constexpr size_t BBO_FULL_SIZE = 44;         // With 4-point timestamps

// Packet layout offsets
constexpr size_t SYMBOL_OFFSET = 0;
constexpr size_t BID_PRICE_OFFSET = 8;
constexpr size_t BID_SHARES_OFFSET = 12;
constexpr size_t ASK_PRICE_OFFSET = 16;
constexpr size_t ASK_SHARES_OFFSET = 20;
constexpr size_t SPREAD_OFFSET = 24;
constexpr size_t T1_OFFSET = 28;
constexpr size_t T2_OFFSET = 32;
constexpr size_t T3_OFFSET = 36;
constexpr size_t T4_OFFSET = 40;

// Fast BBO parser - optimized for ultra-low latency
// No string operations, no exceptions, minimal branching
class BBOParserFast {
public:
    // Parse BBO data from raw UDP payload
    // Returns pointer to pool-allocated BBODataFast, or nullptr on failure
    //
    // @param data     Pointer to UDP payload (BBO at start)
    // @param len      Length of payload
    // @param pool     Pre-allocated BBO object pool
    // @param ts_ns    Reception timestamp (from RDTSC)
    // @param sequence Packet sequence number
    //
    template<size_t PoolSize>
    HOT_FUNC
    static BBODataFast* parse(
        const uint8_t* data,
        size_t len,
        BBOPool<PoolSize>& pool,
        uint64_t ts_ns,
        uint32_t sequence = 0
    ) noexcept {
        // Fast reject for undersized packets
        if (unlikely(len < BBO_MIN_SIZE)) {
            return nullptr;
        }

        // Acquire slot from pool (zero allocation)
        BBODataFast* bbo = pool.acquire();

        // Symbol: 8 bytes at offset 0
        // Single 64-bit load (assuming aligned or unaligned load is fast)
        std::memcpy(bbo->symbol, data + SYMBOL_OFFSET, 8);

        // Price data: all big-endian uint32_t
        // Using __builtin_bswap32 for efficient byte swap
        const uint32_t* prices = reinterpret_cast<const uint32_t*>(data + BID_PRICE_OFFSET);

        uint32_t bid_raw = __builtin_bswap32(prices[0]);
        uint32_t bid_shares = __builtin_bswap32(prices[1]);
        uint32_t ask_raw = __builtin_bswap32(prices[2]);
        uint32_t ask_shares = __builtin_bswap32(prices[3]);
        uint32_t spread_raw = __builtin_bswap32(prices[4]);

        // Convert to doubles using multiplication (faster than division)
        bbo->bid_price = bid_raw * PRICE_MULTIPLIER;
        bbo->ask_price = ask_raw * PRICE_MULTIPLIER;
        bbo->spread = spread_raw * PRICE_MULTIPLIER;

        bbo->bid_shares = bid_shares;
        bbo->ask_shares = ask_shares;

        bbo->timestamp_ns = ts_ns;
        bbo->sequence = sequence;
        bbo->valid = 1;

        // Check if packet has FPGA timestamps
        if (len >= BBO_FULL_SIZE) {
            bbo->flags = BboFlags::HAS_FPGA_TIMESTAMPS;
        } else {
            bbo->flags = 0;
        }

        return bbo;
    }

    // Extract FPGA timestamps from packet (separate from hot path)
    // Only call this when detailed timing analysis is needed
    COLD_FUNC
    static FPGATimestamps extract_timestamps(const uint8_t* data, size_t len) noexcept {
        FPGATimestamps ts;

        if (len < BBO_FULL_SIZE) {
            return ts;  // No timestamps available
        }

        const uint32_t* tdata = reinterpret_cast<const uint32_t*>(data + T1_OFFSET);

        ts.t1 = __builtin_bswap32(tdata[0]);
        ts.t2 = __builtin_bswap32(tdata[1]);
        ts.t3 = __builtin_bswap32(tdata[2]);
        ts.t4 = __builtin_bswap32(tdata[3]);

        ts.calculate_latencies();

        return ts;
    }

    // Validate packet without full parse (for filtering)
    HOT_FUNC
    static bool is_valid_bbo(const uint8_t* data, size_t len) noexcept {
        if (unlikely(len < BBO_MIN_SIZE)) {
            return false;
        }

        // Check symbol is printable ASCII (basic validation)
        for (size_t i = 0; i < 8; ++i) {
            uint8_t c = data[i];
            if (unlikely(c < 0x20 || c > 0x7E)) {
                return false;
            }
        }

        return true;
    }

    // Quick symbol check without full parse
    HOT_FUNC
    static bool symbol_matches(const uint8_t* data, const char* target, size_t target_len) noexcept {
        // Compare up to target_len bytes
        for (size_t i = 0; i < target_len && i < 8; ++i) {
            if (data[i] != static_cast<uint8_t>(target[i])) {
                return false;
            }
        }
        return true;
    }
};

// Inline helper for common case: parse with default pool
template<size_t PoolSize>
HOT_FUNC
inline BBODataFast* parse_bbo(
    const uint8_t* data,
    size_t len,
    BBOPool<PoolSize>& pool,
    uint64_t ts_ns
) noexcept {
    return BBOParserFast::parse(data, len, pool, ts_ns);
}

}  // namespace ultra_ll
