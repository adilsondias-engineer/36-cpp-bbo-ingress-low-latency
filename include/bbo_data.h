#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace ultra_ll {

// Cache-line aligned BBO structure for ultra-low-latency processing
// Exactly 64 bytes = 1 cache line for optimal memory access patterns
//
// Design decisions:
// - Fixed 8-byte symbol (vs 16 in gateway::BBOData) - most symbols fit
// - No FPGA timestamps in hot path (can be extracted separately)
// - All data needed for trading decision in single cache line fetch
//
struct alignas(64) BBODataFast {
    char symbol[8];          // 8 bytes: Stock ticker (space-padded)
    double bid_price;        // 8 bytes: Best bid price
    double ask_price;        // 8 bytes: Best ask price
    uint32_t bid_shares;     // 4 bytes: Total bid shares
    uint32_t ask_shares;     // 4 bytes: Total ask shares
    double spread;           // 8 bytes: Ask - Bid
    uint64_t timestamp_ns;   // 8 bytes: Reception timestamp (RDTSC-based)
    uint32_t sequence;       // 4 bytes: Packet sequence number
    uint8_t valid;           // 1 byte: Data validity flag
    uint8_t flags;           // 1 byte: Status flags (bit 0: has_timestamps)
    uint8_t padding[10];     // 10 bytes: Pad to exactly 64 bytes
    // Total: 64 bytes

    void clear() noexcept {
        std::memset(this, 0, sizeof(*this));
    }

    // Helper to set symbol from C string
    void set_symbol(const char* sym, size_t len) noexcept {
        size_t copy_len = len < 8 ? len : 8;
        std::memcpy(symbol, sym, copy_len);
        // Space-pad remaining
        for (size_t i = copy_len; i < 8; ++i) {
            symbol[i] = ' ';
        }
    }

    // Helper to get symbol as trimmed string
    std::string get_symbol() const {
        size_t len = 8;
        while (len > 0 && (symbol[len-1] == ' ' || symbol[len-1] == '\0')) {
            --len;
        }
        return std::string(symbol, len);
    }
};

// Compile-time checks
static_assert(sizeof(BBODataFast) == 64, "BBODataFast must be exactly 64 bytes (1 cache line)");
static_assert(alignof(BBODataFast) == 64, "BBODataFast must be cache-line aligned");

// FPGA timestamp extension (optional, stored separately from hot path)
// Used when detailed latency analysis is needed
struct FPGATimestamps {
    uint32_t t1;             // ITCH parse (125 MHz RGMII RX cycle count)
    uint32_t t2;             // itch_cdc_fifo write
    uint32_t t3;             // bbo_fifo read (125 MHz TX cycle count)
    uint32_t t4;             // TX start
    double latency_a_us;     // T2 - T1 in microseconds
    double latency_b_us;     // T4 - T3 in microseconds
    double total_us;         // Total FPGA latency

    FPGATimestamps() : t1(0), t2(0), t3(0), t4(0),
                       latency_a_us(0), latency_b_us(0), total_us(0) {}

    void calculate_latencies() {
        // 125 MHz = 8 ns per cycle
        constexpr double NS_PER_CYCLE = 8.0;
        constexpr double US_PER_NS = 0.001;
        latency_a_us = (t2 - t1) * NS_PER_CYCLE * US_PER_NS;
        latency_b_us = (t4 - t3) * NS_PER_CYCLE * US_PER_NS;
        total_us = latency_a_us + latency_b_us;
    }
};

// Flags for BBODataFast.flags field
namespace BboFlags {
    constexpr uint8_t HAS_FPGA_TIMESTAMPS = 0x01;
    constexpr uint8_t IS_SYNTHETIC = 0x02;  // Warm-up packet
    constexpr uint8_t IS_STALE = 0x04;      // Data may be outdated
}

}  // namespace ultra_ll

// Interoperability with gateway::BBOData (from common/bbo_data.h)
// This allows Project 36 to write to shared memory consumed by Project 15
namespace gateway {
    struct BBOData;  // Forward declaration
}

namespace ultra_ll {

// Convert fast BBO to gateway format for shared memory export
// Note: FPGA timestamps are lost in this conversion (they're not in the hot path)
inline void to_gateway_bbo(const BBODataFast& fast, gateway::BBOData& out) {
    // This function will be implemented where gateway::BBOData is fully visible
    // It's here to document the interface
    (void)fast;
    (void)out;
}

}  // namespace ultra_ll
