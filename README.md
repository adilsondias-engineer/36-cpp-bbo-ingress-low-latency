# Project 36: Ultra Low Latency RX - DPDK Kernel Bypass Network Handler

**Platform:** Linux
**Technology:** C++20, DPDK 25.11, LMAX Disruptor, POSIX Shared Memory
**Status:** In Development

---

## Overview

Ultra-optimized DPDK network handler for BBO (Best Bid/Offer) data processing. This project is a stripped-down, hyper-optimized version of Project 14's network handler, focusing purely on the critical path from NIC to shared memory.

**Primary Data Flow:**
```
[NIC] ──DPDK──→ [BBO Parser] ──→ [Shared Memory Ring Buffer] ──→ Project 15 (Market Maker)
```

**Design Philosophy:**
- All distribution components removed (Kafka, MQTT, TCP server, CSV logging)
- All input methods except DPDK removed (UDP, XDP)
- Single-threaded design: One polling loop, one core, zero context switches
- Zero-allocation hot path

**Trading Relevance:** Reduces tail latency (P99) for ultra-low-latency trading applications. Target: P99/P50 ratio < 2.5x (down from 5.5x in Project 14).

---

## Performance Target

| Metric | Project 14 | Project 36 Target | Improvement |
|--------|------------|-------------------|-------------|
| P50 | 39 ns | 35-38 ns | -3-4 ns |
| P85 | 71 ns | 42-45 ns | -26-29 ns |
| P90 | 90 ns | 48-52 ns | -38-42 ns |
| P99 | 216 ns | 80-100 ns | **-116-136 ns** |
| P99/P50 | 5.5x | **<2.5x** | **>50% tighter** |

---

## Hardware Requirements

| Component | Specification |
|-----------|---------------|
| CPU | x86_64 with RDTSC support |
| Memory | Hugepage support (2MB or 1GB pages) |
| NIC | DPDK-compatible (Intel I219-LM, most Intel/Mellanox NICs) |
| OS | Linux kernel 5.4+ |

---

## Architecture

### Core Components

```
┌─────────────────────────────────────────────────────────────────┐
│              PROJECT 36 (CRITICAL PATH ONLY)                    │
│                                                                 │
│  ┌───────────────┐    ┌────────────────┐    ┌───────────────┐  │
│  │ DPDK Receiver │───→│ BBO Parser     │───→│ Ring Buffer   │  │
│  │ (Poll Mode)   │    │ (Zero-Copy)    │    │ (Disruptor)   │  │
│  │               │    │                │    │               │  │
│  │ Zero-copy RX  │    │ Branch hints   │    │ Lock-free     │  │
│  │ Huge pages    │    │ Prefetch       │    │ Atomic seq    │  │
│  │ Busy polling  │    │ RDTSC timing   │    │ 131 KB shm    │  │
│  └───────────────┘    └────────────────┘    └───────────────┘  │
│                                                                 │
│  Single thread, zero allocation, L1/L2 cache optimized         │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼ (via Disruptor IPC)
┌─────────────────────────────────────────────────────────────────┐
│               PROJECT 15 (Market Maker FSM)                     │
│                                                                 │
│  Consumes BBO updates from shared memory ring buffer            │
└─────────────────────────────────────────────────────────────────┘
```

### Data Structures

**BBODataFast (64 bytes - 1 cache line):**
```cpp
struct alignas(64) BBODataFast {
    char symbol[8];          // 8 bytes
    double bid_price;        // 8 bytes
    double ask_price;        // 8 bytes
    uint32_t bid_shares;     // 4 bytes
    uint32_t ask_shares;     // 4 bytes
    double spread;           // 8 bytes
    uint64_t timestamp_ns;   // 8 bytes
    uint32_t sequence;       // 4 bytes
    uint8_t valid;           // 1 byte
    uint8_t flags;           // 1 byte
    uint8_t padding[10];     // 10 bytes
};
static_assert(sizeof(BBODataFast) == 64);
```

### Memory Layout

| Component | Size | Location |
|-----------|------|----------|
| BBO Pool | 64 KB | Hugepages (or aligned heap) |
| Disruptor Ring | 2 MB | Shared memory (/dev/shm) |
| DPDK Mbufs | 4-8 MB | Hugepages |

---

## Key Optimizations

### 1. Zero-Allocation Hot Path
- Pre-allocated BBO object pool (1024 entries)
- Circular buffer reuse (no malloc/free)
- 64-byte cache-line aligned structures

### 2. Branch Prediction Hints
```cpp
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
```

### 3. RDTSC Timestamps
- Cycle-accurate timing without syscalls
- Calibrated once at startup
- ~13 cycles overhead vs ~9ns syscall

### 4. Prefetch Pipeline
```cpp
// Prefetch next packet while processing current
if (i + 1 < count) {
    rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 1], void*));
}
```

### 5. Compile-Time Calculations
```cpp
constexpr double PRICE_MULTIPLIER = 0.0001;  // Multiply instead of divide
```

### 6. Aggressive Compiler Optimizations
- `-O3 -march=native -ffast-math`
- `-fno-exceptions -fno-rtti`
- `-flto` (link-time optimization)

### 7. Two-Stage Warm-up
1. Cache touch: Pre-fault all hot data structures
2. Synthetic packets: Train branch predictor

---

## Building

### Prerequisites

- DPDK 25.11+ (same version as Project 14)
- CMake 3.16+
- GCC 11+ or Clang 14+
- Linux with hugepages configured

### Build Commands

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Profile-Guided Optimization (Optional)

PGO provides additional performance gains:

```bash
# Step 1: Build with instrumentation
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO_GENERATE=ON ..
make -j
sudo ./network_handler [run with typical workload]

# Step 2: Build with profile data
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO_USE=ON ..
make -j
```

---

## System Setup

### Configure Hugepages

```bash
# 1. Configure hugepages (2MB pages)
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# 2. Bind NIC to DPDK driver
sudo dpdk-devbind.py -b vfio-pci 0000:01:00.0

# 3. Isolate CPU cores (add to /etc/default/grub)
# GRUB_CMDLINE_LINUX="isolcpus=14-15 nohz_full=14-15 rcu_nocbs=14-15"
```

---

## Usage

### Basic Usage

```bash
# Run with default settings
sudo ./network_handler -l 14 -a 0000:01:00.0 -- -u 12345

# Run with specific options
sudo ./network_handler -l 14 -a 0000:01:00.0 -- \
    -p 0           \  # DPDK port ID
    -u 5000        \  # UDP port
    -c 14          \  # Pin to CPU core 14
    -s gateway     \  # Shared memory name
    -w 1000        \  # Warm-up packets
    -b                # Enable benchmark mode
```

### Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-p, --port` | DPDK port ID | 0 |
| `-q, --queue` | RX queue ID | 0 |
| `-u, --udp-port` | UDP port to listen | 12345 |
| `-c, --core` | CPU core to pin to | auto |
| `-s, --shm` | Shared memory name | gateway |
| `-w, --warmup` | Warm-up packet count | 1000 |
| `-n, --no-warmup` | Skip warm-up | false |
| `-b, --benchmark` | Print stats every 5s | false |

### Configuration File

```json
{
    "dpdk": {
        "port_id": 0,
        "queue_id": 0,
        "eal_args": ["-l", "14", "-a", "0000:01:00.0", "--socket-mem=1024,0"]
    },
    "network": {
        "udp_port": 12345
    },
    "shared_memory": {
        "name": "gateway",
        "ring_size": 16384
    },
    "cpu": {
        "core_id": 14,
        "isolate_cores": "14-15",
        "governor": "performance"
    },
    "warmup": {
        "enabled": true,
        "synthetic_packets": 1000,
        "cache_touch": true
    },
    "benchmark": {
        "enabled": false,
        "stats_interval_seconds": 5
    }
}
```

---

## Integration with Project 15

Project 15 (Market Maker) reads from the same shared memory:

```cpp
// Project 15 code
DisruptorClient client("gateway");
client.connect();

while (running) {
    gateway::BBOData bbo;
    if (client.try_read_bbo(bbo)) {
        process_market_data(bbo);
    }
}
```

---

## Troubleshooting

### "EAL init failed"
- Ensure hugepages are configured
- Check NIC is bound to DPDK driver
- Run as root or with appropriate capabilities

### "Failed to create mbuf pool"
- Increase hugepage allocation
- Check NUMA socket configuration

### High latency variance
- Verify CPU isolation (`isolcpus`, `nohz_full`)
- Disable hyperthreading
- Check for SMI interrupts: `perf stat -e msr/smi/ -a sleep 10`

### Ring buffer full
- Increase ring size in config
- Check consumer (Project 15) is running

---

## Code Structure

```
36-ultra-low-latency-rx/
├── CMakeLists.txt          # Build configuration with aggressive optimizations
├── config.json             # Runtime configuration
├── README.md               # This file
├── include/
│   ├── likely.h            # Branch prediction macros
│   ├── rdtsc.h             # RDTSC timestamp utilities
│   ├── bbo_data.h          # 64-byte aligned BBO structure
│   ├── bbo_pool.h          # Pre-allocated object pool
│   ├── bbo_parser_fast.h   # Optimized BBO parser
│   └── dpdk_receiver.h     # DPDK receiver header
└── src/
    ├── main.cpp            # Entry point with warm-up
    └── dpdk_receiver.cpp   # DPDK implementation
```

---

## Key Differences from Project 14

| Aspect | Project 14 | Project 36 |
|--------|------------|------------|
| Input | UDP, XDP, DPDK | DPDK only |
| Output | Kafka, MQTT, TCP, CSV, SHM | Shared memory only |
| Threads | 4+ (UDP, Binance, Publish, TCP I/O) | 1 (polling loop) |
| Memory | Dynamic queues, unbounded vectors | Fixed pools, bounded |
| Branches | No hints | likely/unlikely everywhere |
| Parser | Generic, conditional | Optimized, branchless |
| FPU | Division by 10000 | Multiply by 0.0001 (constexpr) |
| Warm-up | None | Cache touch + synthetic packets |
| Working Set | Unbounded | <256KB (fits L2) |

---

## Related Projects

- **[14-order-gateway-cpp/](../14-order-gateway-cpp/)** - Full-featured order gateway (multi-protocol)
- **[15-market-maker/](../15-market-maker/)** - Market maker FSM (consumer)
- **[common/disruptor/](../common/disruptor/)** - LMAX Disruptor shared memory IPC

---

## References

- [DPDK Programmer's Guide](https://doc.dpdk.org/guides/)
- [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/)
- [Intel Optimization Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)

---

## Status

| Item | Status |
|------|--------|
| DPDK Receiver | Implemented |
| BBO Parser | Implemented |
| Object Pool | Implemented |
| Shared Memory IPC | Implemented |
| Warm-up | Implemented |
| Benchmark Mode | Implemented |
| Hardware Testing | Pending |

---

**Created:** January 2026
**Last Updated:** February 3, 2026
**Build Time:** ~15 seconds
**Hardware Status:** Pending DPDK testing on Linux
