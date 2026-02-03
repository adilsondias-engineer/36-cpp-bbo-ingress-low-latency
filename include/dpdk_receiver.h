#pragma once

#include "bbo_data.h"
#include "bbo_pool.h"
#include "bbo_parser_fast.h"
#include "likely.h"
#include "rdtsc.h"

// DPDK headers
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_mempool.h>

// Disruptor shared memory
#include "../../common/disruptor/BboRingBuffer.h"
#include "../../common/disruptor/SharedMemoryManager.h"
#include "../../common/bbo_data.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ultra_ll {

// Configuration constants
constexpr uint16_t BURST_SIZE = 32;         // Smaller burst = lower latency variance
constexpr uint16_t RX_RING_SIZE = 1024;     // RX descriptor ring size
constexpr uint16_t MBUF_POOL_SIZE = 8191;   // Number of mbufs
constexpr uint16_t MBUF_CACHE_SIZE = 250;   // Cache size per core

// DPDK Receiver - Ultra Low Latency Single-Threaded Packet Handler
//
// Design:
// - Single polling loop (no context switches)
// - Prefetch next packet while processing current
// - Zero allocation in hot path (object pool)
// - RDTSC timestamps (no syscalls)
// - Writes directly to Disruptor shared memory
//
class DPDKReceiver {
public:
    struct Config {
        uint16_t port_id = 0;
        uint16_t queue_id = 0;
        uint16_t udp_port = 12345;
        int lcore_id = -1;              // -1 = auto-detect
        std::string shm_name = "gateway";
        bool enable_stats = true;
    };

    // Statistics (cache-line aligned to prevent false sharing)
    struct alignas(64) Stats {
        std::atomic<uint64_t> packets_received{0};
        std::atomic<uint64_t> packets_processed{0};
        std::atomic<uint64_t> packets_dropped{0};
        std::atomic<uint64_t> parse_errors{0};
        std::atomic<uint64_t> ring_buffer_full{0};
    };

    explicit DPDKReceiver(const Config& config);
    ~DPDKReceiver();

    // Non-copyable
    DPDKReceiver(const DPDKReceiver&) = delete;
    DPDKReceiver& operator=(const DPDKReceiver&) = delete;

    // Initialize DPDK and shared memory
    bool initialize(int argc, char** argv);

    // Run the polling loop (blocks until stop() is called)
    void poll_loop();

    // Stop the polling loop
    void stop() { running_.store(false, std::memory_order_relaxed); }

    // Check if running
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Warm-up: pre-fault caches and run synthetic packets
    void warm_up(int synthetic_packets = 1000);

    // Get statistics
    const Stats& get_stats() const { return stats_; }
    void print_stats() const;
    void reset_stats();

    // Get TSC calibrator (for external timing)
    const TSCCalibrator& get_tsc() const { return tsc_; }

private:
    Config config_;
    Stats stats_;
    TSCCalibrator tsc_;

    // DPDK resources
    rte_mempool* mbuf_pool_ = nullptr;
    bool dpdk_initialized_ = false;

    // Object pool for BBO parsing
    BBOPool<1024> bbo_pool_;

    // Shared memory ring buffer
    disruptor::BboRingBuffer* ring_buffer_ = nullptr;

    // Running flag
    std::atomic<bool> running_{false};

    // Packet sequence counter
    uint32_t sequence_ = 0;

    // Internal methods
    bool init_dpdk_eal(int argc, char** argv);
    bool init_mempool();
    bool init_port();
    bool init_shared_memory();

    // Hot path methods
    HOT_FUNC void process_burst(rte_mbuf** pkts, uint16_t count);
    HOT_FUNC void process_packet(rte_mbuf* pkt);
    HOT_FUNC void publish_bbo(const BBODataFast& fast);

    // Convert fast BBO to gateway format for shared memory
    HOT_FUNC void convert_and_publish(const BBODataFast& fast);

    // Warm-up helpers
    void warm_cache();
    void warm_dpdk_path(int count);
    rte_mbuf* create_dummy_packet();
};

// Inline hot path implementations

HOT_FUNC
inline void DPDKReceiver::process_burst(rte_mbuf** pkts, uint16_t count) {
    for (uint16_t i = 0; i < count; ++i) {
        // Prefetch next packet's data into L1 cache
        if (likely(i + 1 < count)) {
            rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 1], void*));
        }
        // Prefetch packet after that into L2 cache
        if (likely(i + 2 < count)) {
            prefetch_l2(rte_pktmbuf_mtod(pkts[i + 2], void*));
        }

        process_packet(pkts[i]);
        rte_pktmbuf_free(pkts[i]);
    }
}

HOT_FUNC
inline void DPDKReceiver::process_packet(rte_mbuf* pkt) {
    // Capture timestamp immediately
    uint64_t ts = rdtsc();

    // Get Ethernet header
    auto* eth = rte_pktmbuf_mtod(pkt, rte_ether_hdr*);

    // Fast check: IPv4?
    if (unlikely(eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))) {
        return;
    }

    // Get IP header
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(eth + 1);

    // Fast check: UDP?
    if (unlikely(ip->next_proto_id != IPPROTO_UDP)) {
        return;
    }

    // Get UDP header (account for IP header length)
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    auto* udp = reinterpret_cast<rte_udp_hdr*>(
        reinterpret_cast<uint8_t*>(ip) + ihl
    );

    // Fast check: target port?
    if (unlikely(rte_be_to_cpu_16(udp->dst_port) != config_.udp_port)) {
        return;
    }

    // Extract payload
    const uint8_t* payload = reinterpret_cast<uint8_t*>(udp + 1);
    size_t payload_len = rte_be_to_cpu_16(udp->dgram_len) - sizeof(rte_udp_hdr);

    // Update stats
    if (config_.enable_stats) {
        stats_.packets_received.fetch_add(1, std::memory_order_relaxed);
    }

    // Convert TSC to nanoseconds
    uint64_t ts_ns = tsc_.cycles_to_ns(ts);

    // Parse BBO
    BBODataFast* bbo = BBOParserFast::parse(
        payload, payload_len, bbo_pool_, ts_ns, sequence_++
    );

    if (likely(bbo != nullptr)) {
        convert_and_publish(*bbo);

        if (config_.enable_stats) {
            stats_.packets_processed.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        if (config_.enable_stats) {
            stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

HOT_FUNC
inline void DPDKReceiver::convert_and_publish(const BBODataFast& fast) {
    // Convert to gateway::BBOData for shared memory
    gateway::BBOData bbo;

    // Copy symbol (8 bytes from fast, pad to 16 in gateway)
    std::memcpy(bbo.symbol, fast.symbol, 8);
    for (size_t i = 8; i < gateway::BBOData::SYMBOL_MAX_LEN; ++i) {
        bbo.symbol[i] = ' ';
    }
    bbo.symbol[gateway::BBOData::SYMBOL_MAX_LEN - 1] = '\0';

    bbo.bid_price = fast.bid_price;
    bbo.ask_price = fast.ask_price;
    bbo.bid_shares = fast.bid_shares;
    bbo.ask_shares = fast.ask_shares;
    bbo.spread = fast.spread;
    bbo.timestamp_ns = static_cast<int64_t>(fast.timestamp_ns);
    bbo.valid = (fast.valid != 0);

    // FPGA timestamps not available in hot path
    bbo.fpga_ts_t1 = 0;
    bbo.fpga_ts_t2 = 0;
    bbo.fpga_ts_t3 = 0;
    bbo.fpga_ts_t4 = 0;
    bbo.fpga_latency_a_us = 0;
    bbo.fpga_latency_b_us = 0;
    bbo.fpga_latency_us = 0;
    bbo.fpga_rx_timestamp = 0;
    bbo.fpga_tx_timestamp = 0;

    // Publish to ring buffer
    if (unlikely(!ring_buffer_->try_publish(bbo))) {
        if (config_.enable_stats) {
            stats_.ring_buffer_full.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace ultra_ll
