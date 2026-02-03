#include "dpdk_receiver.h"
#include <rte_bus_pci.h>
#include <rte_log.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace ultra_ll {

DPDKReceiver::DPDKReceiver(const Config& config)
    : config_(config) {
}

DPDKReceiver::~DPDKReceiver() {
    stop();

    // Disconnect from shared memory
    if (ring_buffer_) {
        disruptor::SharedMemoryManager<disruptor::BboRingBuffer>::disconnect(ring_buffer_);
        ring_buffer_ = nullptr;
    }

    // Stop and close DPDK port
    if (dpdk_initialized_) {
        rte_eth_dev_stop(config_.port_id);
        rte_eth_dev_close(config_.port_id);
    }

    // DPDK EAL cleanup happens at process exit
}

bool DPDKReceiver::initialize(int argc, char** argv) {
    if (!init_dpdk_eal(argc, argv)) {
        return false;
    }

    if (!init_mempool()) {
        return false;
    }

    if (!init_port()) {
        return false;
    }

    if (!init_shared_memory()) {
        return false;
    }

    dpdk_initialized_ = true;
    return true;
}

bool DPDKReceiver::init_dpdk_eal(int argc, char** argv) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        std::fprintf(stderr, "Error: DPDK EAL initialization failed: %s\n",
                     rte_strerror(rte_errno));
        return false;
    }

    // Check if the configured port exists
    if (!rte_eth_dev_is_valid_port(config_.port_id)) {
        std::fprintf(stderr, "Error: Invalid port ID %u\n", config_.port_id);
        return false;
    }

    std::printf("DPDK EAL initialized, using port %u\n", config_.port_id);
    return true;
}

bool DPDKReceiver::init_mempool() {
    mbuf_pool_ = rte_pktmbuf_pool_create(
        "MBUF_POOL",
        MBUF_POOL_SIZE,
        MBUF_CACHE_SIZE,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_socket_id()
    );

    if (!mbuf_pool_) {
        std::fprintf(stderr, "Error: Failed to create mbuf pool: %s\n",
                     rte_strerror(rte_errno));
        return false;
    }

    std::printf("Created mbuf pool with %u mbufs\n", MBUF_POOL_SIZE);
    return true;
}

bool DPDKReceiver::init_port() {
    int ret;
    rte_eth_dev_info dev_info;

    // Get device info
    ret = rte_eth_dev_info_get(config_.port_id, &dev_info);
    if (ret != 0) {
        std::fprintf(stderr, "Error: Failed to get device info for port %u\n",
                     config_.port_id);
        return false;
    }

    std::printf("Port %u: %s\n", config_.port_id, dev_info.driver_name);

    // Configure port
    rte_eth_conf port_conf{};
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

    // Disable checksum offloads for lower latency
    port_conf.rxmode.offloads = 0;

    ret = rte_eth_dev_configure(config_.port_id, 1, 0, &port_conf);
    if (ret < 0) {
        std::fprintf(stderr, "Error: Failed to configure port %u: %s\n",
                     config_.port_id, rte_strerror(-ret));
        return false;
    }

    // Setup RX queue
    rte_eth_rxconf rxconf = dev_info.default_rxconf;
    rxconf.offloads = 0;  // Disable offloads for lower latency

    ret = rte_eth_rx_queue_setup(
        config_.port_id,
        config_.queue_id,
        RX_RING_SIZE,
        rte_eth_dev_socket_id(config_.port_id),
        &rxconf,
        mbuf_pool_
    );

    if (ret < 0) {
        std::fprintf(stderr, "Error: Failed to setup RX queue: %s\n",
                     rte_strerror(-ret));
        return false;
    }

    // Start port
    ret = rte_eth_dev_start(config_.port_id);
    if (ret < 0) {
        std::fprintf(stderr, "Error: Failed to start port: %s\n",
                     rte_strerror(-ret));
        return false;
    }

    // Enable promiscuous mode
    ret = rte_eth_promiscuous_enable(config_.port_id);
    if (ret != 0) {
        std::fprintf(stderr, "Warning: Failed to enable promiscuous mode\n");
    }

    // Print link status
    rte_eth_link link;
    ret = rte_eth_link_get(config_.port_id, &link);
    if (ret == 0 && link.link_status) {
        std::printf("Port %u: Link up - speed %u Mbps - %s\n",
                    config_.port_id,
                    link.link_speed,
                    link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ?
                    "full-duplex" : "half-duplex");
    } else {
        std::printf("Port %u: Link down\n", config_.port_id);
    }

    return true;
}

bool DPDKReceiver::init_shared_memory() {
    const std::string shm_name = "/bbo_ring_" + config_.shm_name;
    constexpr size_t shm_size = sizeof(disruptor::BboRingBuffer);
    int fd = -1;
    void* ptr = MAP_FAILED;

    // Try to open existing shared memory first (created by Project 14)
    fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    if (fd != -1) {
        ptr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr != MAP_FAILED) {
            ring_buffer_ = static_cast<disruptor::BboRingBuffer*>(ptr);
            std::printf("Connected to existing shared memory '%s'\n",
                        config_.shm_name.c_str());
            return true;
        }
        // mmap failed, fall through to create
    }

    // Create new shared memory
    shm_unlink(shm_name.c_str());  // Remove any stale instance

    fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd == -1) {
        std::fprintf(stderr, "Error: Failed to create shared memory '%s': %s\n",
                     shm_name.c_str(), std::strerror(errno));
        return false;
    }

    if (ftruncate(fd, shm_size) == -1) {
        std::fprintf(stderr, "Error: Failed to set shared memory size: %s\n",
                     std::strerror(errno));
        ::close(fd);
        shm_unlink(shm_name.c_str());
        return false;
    }

    ptr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);

    if (ptr == MAP_FAILED) {
        std::fprintf(stderr, "Error: Failed to map shared memory: %s\n",
                     std::strerror(errno));
        shm_unlink(shm_name.c_str());
        return false;
    }

    // Placement new to initialize the ring buffer
    ring_buffer_ = new (ptr) disruptor::BboRingBuffer();
    std::printf("Created new shared memory '%s'\n", config_.shm_name.c_str());

    return true;
}

void DPDKReceiver::poll_loop() {
    running_.store(true, std::memory_order_relaxed);

    rte_mbuf* pkts[BURST_SIZE];

    std::printf("Starting poll loop on port %u, queue %u, UDP port %u\n",
                config_.port_id, config_.queue_id, config_.udp_port);

    while (likely(running_.load(std::memory_order_relaxed))) {
        uint16_t nb_rx = rte_eth_rx_burst(
            config_.port_id,
            config_.queue_id,
            pkts,
            BURST_SIZE
        );

        if (likely(nb_rx > 0)) {
            process_burst(pkts, nb_rx);
        }
        // No pause/yield - busy poll for minimum latency
    }

    std::printf("Poll loop stopped\n");
}

void DPDKReceiver::warm_up(int synthetic_packets) {
    std::printf("Warming up caches and DPDK path...\n");

    // Stage 1: Warm BBO pool cache lines
    warm_cache();

    // Stage 2: Send synthetic packets through the processing path
    warm_dpdk_path(synthetic_packets);

    std::printf("Warm-up complete (%d synthetic packets processed)\n",
                synthetic_packets);
}

void DPDKReceiver::warm_cache() {
    // Touch all entries in BBO pool to bring into cache
    bbo_pool_.warm_cache();

    // Touch TSC calibrator to ensure it's in cache
    volatile uint64_t sink = tsc_.cycles_to_ns(rdtsc());
    (void)sink;

    compiler_barrier();
}

void DPDKReceiver::warm_dpdk_path(int count) {
    for (int i = 0; i < count; ++i) {
        rte_mbuf* dummy = create_dummy_packet();
        if (dummy) {
            process_packet(dummy);
            rte_pktmbuf_free(dummy);
        }
    }
}

rte_mbuf* DPDKReceiver::create_dummy_packet() {
    rte_mbuf* pkt = rte_pktmbuf_alloc(mbuf_pool_);
    if (!pkt) {
        return nullptr;
    }

    // Minimum packet: Ethernet + IP + UDP + BBO data
    constexpr size_t ETH_SIZE = sizeof(rte_ether_hdr);
    constexpr size_t IP_SIZE = sizeof(rte_ipv4_hdr);
    constexpr size_t UDP_SIZE = sizeof(rte_udp_hdr);
    constexpr size_t BBO_SIZE = 44;  // Full BBO with timestamps
    constexpr size_t TOTAL_SIZE = ETH_SIZE + IP_SIZE + UDP_SIZE + BBO_SIZE;

    char* data = rte_pktmbuf_append(pkt, TOTAL_SIZE);
    if (!data) {
        rte_pktmbuf_free(pkt);
        return nullptr;
    }

    std::memset(data, 0, TOTAL_SIZE);

    // Ethernet header
    auto* eth = reinterpret_cast<rte_ether_hdr*>(data);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // IP header
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(data + ETH_SIZE);
    ip->version_ihl = 0x45;  // IPv4, 20 bytes header
    ip->total_length = rte_cpu_to_be_16(IP_SIZE + UDP_SIZE + BBO_SIZE);
    ip->next_proto_id = IPPROTO_UDP;

    // UDP header
    auto* udp = reinterpret_cast<rte_udp_hdr*>(data + ETH_SIZE + IP_SIZE);
    udp->dst_port = rte_cpu_to_be_16(config_.udp_port);
    udp->dgram_len = rte_cpu_to_be_16(UDP_SIZE + BBO_SIZE);

    // BBO payload (synthetic)
    uint8_t* bbo = reinterpret_cast<uint8_t*>(data + ETH_SIZE + IP_SIZE + UDP_SIZE);

    // Symbol: "WARMUP  "
    std::memcpy(bbo, "WARMUP  ", 8);

    // Set valid prices (network byte order)
    uint32_t price = __builtin_bswap32(1500000);  // $150.00
    uint32_t shares = __builtin_bswap32(100);
    uint32_t spread = __builtin_bswap32(1000);    // $0.10

    std::memcpy(bbo + 8, &price, 4);   // bid_price
    std::memcpy(bbo + 12, &shares, 4); // bid_shares
    std::memcpy(bbo + 16, &price, 4);  // ask_price
    std::memcpy(bbo + 20, &shares, 4); // ask_shares
    std::memcpy(bbo + 24, &spread, 4); // spread

    return pkt;
}

void DPDKReceiver::print_stats() const {
    std::printf("=== DPDKReceiver Statistics ===\n");
    std::printf("  Packets received:  %lu\n",
                stats_.packets_received.load(std::memory_order_relaxed));
    std::printf("  Packets processed: %lu\n",
                stats_.packets_processed.load(std::memory_order_relaxed));
    std::printf("  Parse errors:      %lu\n",
                stats_.parse_errors.load(std::memory_order_relaxed));
    std::printf("  Ring buffer full:  %lu\n",
                stats_.ring_buffer_full.load(std::memory_order_relaxed));
    std::printf("  TSC calibration:   %.3f GHz\n", tsc_.get_ghz());
    std::printf("  BBO pool head:     %u\n", bbo_pool_.current_head());
    std::printf("  Using hugepages:   %s\n",
                bbo_pool_.is_using_hugepages() ? "yes" : "no");
}

void DPDKReceiver::reset_stats() {
    stats_.packets_received.store(0, std::memory_order_relaxed);
    stats_.packets_processed.store(0, std::memory_order_relaxed);
    stats_.packets_dropped.store(0, std::memory_order_relaxed);
    stats_.parse_errors.store(0, std::memory_order_relaxed);
    stats_.ring_buffer_full.store(0, std::memory_order_relaxed);
}

}  // namespace ultra_ll
