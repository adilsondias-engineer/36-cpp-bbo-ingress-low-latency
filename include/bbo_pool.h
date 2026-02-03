#pragma once

#include "bbo_data.h"
#include "likely.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <sys/mman.h>

namespace ultra_ll {

// Pre-allocated BBO object pool with optional hugepage backing
// Uses lock-free circular buffer for zero-allocation hot path
//
// Memory layout:
// - POOL_SIZE BBODataFast entries (64 bytes each)
// - 1024 entries = 64 KB (fits entirely in L2 cache)
// - Circular reuse - no explicit release needed
//
template<size_t POOL_SIZE = 1024>
class BBOPool {
    static_assert((POOL_SIZE & (POOL_SIZE - 1)) == 0, "POOL_SIZE must be power of 2");
    static_assert(POOL_SIZE >= 64, "POOL_SIZE should be at least 64 for burst handling");

    // Pool storage - either on hugepages or aligned heap
    BBODataFast* pool_;
    bool using_hugepages_;

    // Lock-free head pointer (only incremented, wraps via mask)
    alignas(64) std::atomic<uint32_t> head_{0};

    // Padding to prevent false sharing with other data
    char padding_[64 - sizeof(std::atomic<uint32_t>)];

public:
    BBOPool() : pool_(nullptr), using_hugepages_(false) {
        allocate_pool();
        prefault_pool();
    }

    ~BBOPool() {
        if (pool_) {
            if (using_hugepages_) {
                munmap(pool_, POOL_SIZE * sizeof(BBODataFast));
            } else {
                std::free(pool_);
            }
        }
    }

    // Non-copyable
    BBOPool(const BBOPool&) = delete;
    BBOPool& operator=(const BBOPool&) = delete;

    // Acquire a BBO slot from the pool
    // Always succeeds (circular buffer overwrites old entries)
    // Returns pointer valid until POOL_SIZE more acquires
    HOT_FUNC
    BBODataFast* acquire() noexcept {
        uint32_t idx = head_.fetch_add(1, std::memory_order_relaxed) & (POOL_SIZE - 1);
        return &pool_[idx];
    }

    // No explicit release needed - circular buffer reuses automatically
    // This is intentional for zero-overhead hot path
    void release(BBODataFast*) noexcept {
        // No-op - circular reuse
    }

    // Pre-warm all cache lines in the pool
    // Call before trading to eliminate first-access latency
    void warm_cache() noexcept {
        volatile uint64_t sink = 0;

        // Touch each entry's first cache line
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            sink += *reinterpret_cast<volatile uint64_t*>(&pool_[i]);
        }

        // Prevent optimizer from removing the loop
        compiler_barrier();
    }

    // Access pool entry by index (for warm-up and testing)
    BBODataFast& operator[](size_t i) noexcept { return pool_[i]; }
    const BBODataFast& operator[](size_t i) const noexcept { return pool_[i]; }

    // Pool metadata
    constexpr size_t size() const noexcept { return POOL_SIZE; }
    constexpr size_t bytes() const noexcept { return POOL_SIZE * sizeof(BBODataFast); }
    bool is_using_hugepages() const noexcept { return using_hugepages_; }

    // Current head position (for debugging)
    uint32_t current_head() const noexcept {
        return head_.load(std::memory_order_relaxed);
    }

private:
    void allocate_pool() {
        const size_t alloc_size = POOL_SIZE * sizeof(BBODataFast);

        // Try hugepages first (2MB pages for lower TLB pressure)
        pool_ = static_cast<BBODataFast*>(mmap(
            nullptr,
            alloc_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
            -1, 0
        ));

        if (pool_ != MAP_FAILED) {
            using_hugepages_ = true;
            return;
        }

        // Fallback: try hugepages with explicit 2MB size
        pool_ = static_cast<BBODataFast*>(mmap(
            nullptr,
            alloc_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT),
            -1, 0
        ));

        if (pool_ != MAP_FAILED) {
            using_hugepages_ = true;
            return;
        }

        // Final fallback: aligned heap allocation (64-byte for cache line)
        pool_ = static_cast<BBODataFast*>(aligned_alloc(64, alloc_size));
        using_hugepages_ = false;

        if (!pool_) {
            std::fprintf(stderr, "BBOPool: Failed to allocate %zu bytes\n", alloc_size);
            std::abort();
        }
    }

    void prefault_pool() {
        // Touch every page to ensure physical memory is allocated
        // and page faults happen during init, not during trading
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            pool_[i].clear();
        }
    }
};

// Default pool size for typical use
using DefaultBBOPool = BBOPool<1024>;

// Statistics helper
template<size_t N>
inline void print_pool_stats(const BBOPool<N>& pool) {
    std::printf("BBOPool: %zu entries, %zu KB, hugepages=%s, head=%u\n",
                pool.size(),
                pool.bytes() / 1024,
                pool.is_using_hugepages() ? "yes" : "no",
                pool.current_head());
}

}  // namespace ultra_ll
