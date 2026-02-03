#pragma once

#include <cstdint>
#include <unistd.h>

// Read Time Stamp Counter - cycle-accurate timing
// RDTSC: ~13 cycles overhead, but no syscall
// vs ktime_get_ns(): ~9ns but involves syscall overhead

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// RDTSCP: serializing version (waits for all previous instructions)
// Use this for more accurate measurements at slight cost
static inline uint64_t rdtscp(uint32_t* aux) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(*aux));
    return ((uint64_t)hi << 32) | lo;
}

// RDTSCP without reading processor ID
static inline uint64_t rdtscp() {
    uint32_t lo, hi, aux;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

// TSC Calibrator - converts cycles to nanoseconds
// Calibrate once at startup, then use for all conversions
class TSCCalibrator {
    double ns_per_cycle_;
    double cycles_per_ns_;
    uint64_t base_tsc_;

public:
    TSCCalibrator() : ns_per_cycle_(0), cycles_per_ns_(0), base_tsc_(0) {
        calibrate();
    }

    void calibrate() {
        // Use a longer calibration period for accuracy
        constexpr int calibration_us = 10000;  // 10ms

        uint64_t start_tsc = rdtscp();
        usleep(calibration_us);
        uint64_t end_tsc = rdtscp();

        uint64_t cycles = end_tsc - start_tsc;
        double ns = calibration_us * 1000.0;

        ns_per_cycle_ = ns / cycles;
        cycles_per_ns_ = cycles / ns;
        base_tsc_ = rdtscp();
    }

    // Convert TSC cycles to nanoseconds
    inline uint64_t cycles_to_ns(uint64_t cycles) const {
        return static_cast<uint64_t>(cycles * ns_per_cycle_);
    }

    // Convert nanoseconds to TSC cycles
    inline uint64_t ns_to_cycles(uint64_t ns) const {
        return static_cast<uint64_t>(ns * cycles_per_ns_);
    }

    // Get elapsed nanoseconds since calibration
    inline uint64_t elapsed_ns() const {
        return cycles_to_ns(rdtscp() - base_tsc_);
    }

    // Get current time in nanoseconds (relative to epoch approximation)
    inline uint64_t now_ns() const {
        return cycles_to_ns(rdtscp());
    }

    // Accessors for calibration values
    double get_ns_per_cycle() const { return ns_per_cycle_; }
    double get_cycles_per_ns() const { return cycles_per_ns_; }
    double get_ghz() const { return cycles_per_ns_; }  // GHz = cycles per ns
};

// Lightweight timing helper for benchmarking
class ScopedTimer {
    uint64_t start_;
    uint64_t* result_;

public:
    explicit ScopedTimer(uint64_t* result) : start_(rdtscp()), result_(result) {}
    ~ScopedTimer() { *result_ = rdtscp() - start_; }

    // Non-copyable
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
};
