/**
 * Ultra Low Latency RX - Project 36
 *
 * Optimized DPDK-only network handler for BBO data
 * Critical path: NIC -> DPDK -> BBO Parser -> Shared Memory
 *
 * Target: P99/P50 ratio < 2.5x (down from 5.5x in Project 14)
 */

#include "dpdk_receiver.h"
#include "likely.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <getopt.h>
#include <sys/mman.h>
#include <sched.h>

// nlohmann/json for config parsing (header-only)
// If not available, use simple command-line config
#ifndef USE_JSON_CONFIG
#define USE_JSON_CONFIG 0
#endif

// Global receiver pointer for signal handler
static ultra_ll::DPDKReceiver *g_receiver = nullptr;

// Signal handler for graceful shutdown
void signal_handler(int sig)
{
    std::printf("\nReceived signal %d, stopping...\n", sig);
    if (g_receiver)
    {
        g_receiver->stop();
    }
}

// Lock all memory to prevent page faults
void setup_memory_locking()
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
    {
        std::fprintf(stderr, "Warning: mlockall failed (run as root for best performance)\n");
    }
    else
    {
        std::printf("Memory locked (no page faults during operation)\n");
    }
}

// Set CPU governor to performance mode
void setup_cpu_governor()
{
    int ret = std::system(
        "echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor "
        "> /dev/null 2>&1");
    if (ret == 0)
    {
        std::printf("CPU governor set to performance mode\n");
    }
}

// Pin current thread to specified CPU core
void pin_to_core(int core_id)
{
    if (core_id >= 0)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);

        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0)
        {
            std::printf("Pinned to CPU core %d\n", core_id);
        }
        else
        {
            std::fprintf(stderr, "Warning: Failed to pin to core %d\n", core_id);
        }
    }
}

// Print usage
void print_usage(const char *prog)
{
    std::printf(
        "Ultra Low Latency RX - Project 36\n"
        "\n"
        "Usage: %s [DPDK_EAL_OPTIONS] -- [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -p, --port <id>        DPDK port ID (default: 0)\n"
        "  -q, --queue <id>       RX queue ID (default: 0)\n"
        "  -u, --udp-port <port>  UDP port to listen on (default: 12345)\n"
        "  -c, --core <id>        CPU core to pin to (default: auto)\n"
        "  -s, --shm <name>       Shared memory name (default: gateway)\n"
        "  -w, --warmup <count>   Warm-up packet count (default: 1000)\n"
        "  -n, --no-warmup        Skip warm-up phase\n"
        "  -b, --benchmark        Enable benchmark mode (stats every 5s)\n"
        "  -h, --help             Show this help\n"
        "\n"
        "Example:\n"
        "  sudo %s -l 14 -a 0000:09:00.0 -- -p 0 -u 5000 -c 14\n"
        "\n",
        prog, prog);
}

int main(int argc, char *argv[])
{
    // Default configuration
    ultra_ll::DPDKReceiver::Config config;
    int warmup_count = 1000;
    bool skip_warmup = false;
    bool benchmark_mode = false;
    int pin_core = -1;

    // Parse DPDK EAL arguments first
    // DPDK consumes arguments up to "--"
    int dpdk_argc = argc;
    char **dpdk_argv = argv;

    // Find "--" separator
    int separator_idx = -1;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--") == 0)
        {
            separator_idx = i;
            dpdk_argc = i;
            break;
        }
    }

    // Parse application options after "--"
    if (separator_idx > 0 && separator_idx < argc - 1)
    {
        int opt_argc = argc - separator_idx;
        char **opt_argv = argv + separator_idx;

        static struct option long_options[] = {
            {"port", required_argument, 0, 'p'},
            {"queue", required_argument, 0, 'q'},
            {"udp-port", required_argument, 0, 'u'},
            {"core", required_argument, 0, 'c'},
            {"shm", required_argument, 0, 's'},
            {"warmup", required_argument, 0, 'w'},
            {"no-warmup", no_argument, 0, 'n'},
            {"benchmark", no_argument, 0, 'b'},
            {"help", no_argument, 0, 'h'},
            {0, 0, 0, 0}};

        int opt;
        optind = 1; // Reset getopt
        while ((opt = getopt_long(opt_argc, opt_argv, "p:q:u:c:s:w:nbh",
                                  long_options, nullptr)) != -1)
        {
            switch (opt)
            {
            case 'p':
                config.port_id = static_cast<uint16_t>(std::atoi(optarg));
                break;
            case 'q':
                config.queue_id = static_cast<uint16_t>(std::atoi(optarg));
                break;
            case 'u':
                config.udp_port = static_cast<uint16_t>(std::atoi(optarg));
                break;
            case 'c':
                pin_core = std::atoi(optarg);
                break;
            case 's':
                config.shm_name = optarg;
                break;
            case 'w':
                warmup_count = std::atoi(optarg);
                break;
            case 'n':
                skip_warmup = true;
                break;
            case 'b':
                benchmark_mode = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
            }
        }
    }

    std::printf("=== Ultra Low Latency RX - Project 36 ===\n");
    std::printf("Configuration:\n");
    std::printf("  DPDK port:    %u\n", config.port_id);
    std::printf("  RX queue:     %u\n", config.queue_id);
    std::printf("  UDP port:     %u\n", config.udp_port);
    std::printf("  Shared mem:   %s\n", config.shm_name.c_str());
    std::printf("  Warm-up:      %s (%d packets)\n",
                skip_warmup ? "disabled" : "enabled", warmup_count);
    std::printf("  Benchmark:    %s\n", benchmark_mode ? "enabled" : "disabled");
    std::printf("\n");

    // Setup system optimizations
    setup_memory_locking();
    setup_cpu_governor();

    if (pin_core >= 0)
    {
        pin_to_core(pin_core);
    }

    // Create receiver
    ultra_ll::DPDKReceiver receiver(config);
    g_receiver = &receiver;

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize DPDK and shared memory
    std::printf("Initializing DPDK...\n");
    if (!receiver.initialize(dpdk_argc, dpdk_argv))
    {
        std::fprintf(stderr, "Error: Failed to initialize receiver\n");
        return 1;
    }

    // Warm-up phase
    if (!skip_warmup)
    {
        receiver.warm_up(warmup_count);
    }

    // Print initial stats
    receiver.print_stats();

    // Start polling loop
    std::printf("\nStarting ultra low latency polling loop...\n");
    std::printf("Press Ctrl+C to stop\n\n");

    if (benchmark_mode)
    {
        // In benchmark mode, print stats periodically
        std::thread stats_thread([&receiver]()
                                 {
            while (receiver.is_running()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (receiver.is_running()) {
                    receiver.print_stats();
                }
            } });

        receiver.poll_loop();
        stats_thread.join();
    }
    else
    {
        receiver.poll_loop();
    }

    // Print final stats
    std::printf("\n=== Final Statistics ===\n");
    receiver.print_stats();

    g_receiver = nullptr;
    return 0;
}
