#pragma once

// Branch prediction hints - guide the CPU branch predictor
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// Prefetch hints for cache management
// rte_prefetch0/1/2 are DPDK wrappers; standalone versions below
// for use outside DPDK contexts

// Prefetch into L1 cache (highest locality, for data needed immediately)
#define prefetch_read(addr)  __builtin_prefetch((addr), 0, 3)
#define prefetch_write(addr) __builtin_prefetch((addr), 1, 3)

// Prefetch into L2 cache (moderate locality, for data needed soon)
#define prefetch_l2(addr)    __builtin_prefetch((addr), 0, 2)

// Prefetch into L3 cache (low locality, for data needed eventually)
#define prefetch_l3(addr)    __builtin_prefetch((addr), 0, 1)

// No temporal locality (use for streaming data that won't be reused)
#define prefetch_nta(addr)   __builtin_prefetch((addr), 0, 0)

// Function attributes for hot/cold path optimization
#define HOT_FUNC   __attribute__((hot, always_inline))
#define COLD_FUNC  __attribute__((cold, noinline))
#define FORCE_INLINE __attribute__((always_inline)) inline
#define NEVER_INLINE __attribute__((noinline))

// Compiler memory barrier (prevents reordering across this point)
#define compiler_barrier() asm volatile("" ::: "memory")

// CPU memory fence (full barrier - use sparingly)
#define memory_fence() __sync_synchronize()

// Acquire/Release barriers for lock-free code
#define load_acquire(ptr) __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
#define store_release(ptr, val) __atomic_store_n((ptr), (val), __ATOMIC_RELEASE)
