#include "proxy_core/proxy_core.h"
#include "epoll_core/epoll_core.h"
#include "lb_algorithms/round_robin.h"
#include "lb_algorithms/least_connections.h"
#include "lb_algorithms/consistent_hashing.h"
#include "health_checker/health_checker.h"
#include "config/config.h"
#include "common/logger.h"
#include <vector>
#include <memory>
#include <csignal>
#include <thread>
#include <pthread.h>

// Global pointer purely so the signal handler can print the stats
std::vector<Backend>* global_backends = nullptr;

void handle_sigusr1(int /*sig*/) {
    if (!global_backends) return;
    
    std::cerr << "\n=== [STATS] Backend Status ===" << std::endl;
    for (const auto& b : *global_backends) {
        std::cerr << b.ip << ":" << b.port 
                  << " | Status: " << (b.status == BackendStatus::UP ? "UP" : "DOWN")
                  << " | Active Conns: " << b.active_connections.load() 
                  << " | Total Requests: " << b.total_requests.load() << std::endl;
    }
    std::cerr << "==============================\n" << std::endl;
}

int main(int argc, char* argv[]) {
    LOG_INFO("Starting Load Balancer...");

    // Parse command-line arguments
    Config config = ConfigParser::parse_cli(argc, argv);

    std::shared_ptr<LbAlgorithm> algo;
    if (config.algorithm == "consistent_hashing") {
        algo = std::make_shared<ConsistentHashing>(config.backends);
    } else if (config.algorithm == "least_connections") {
        algo = std::make_shared<LeastConnections>();
    } else if (config.algorithm == "round_robin") {
        algo = std::make_shared<RoundRobin>();
    } else {
        LOG_WARN("Unknown algorithm '" << config.algorithm << "', falling back to round_robin");
        algo = std::make_shared<RoundRobin>();
    }

    // Register signal handler to print stats on demand
    global_backends = &config.backends;
    std::signal(SIGUSR1, handle_sigusr1);

    // Start the health checker background thread
    HealthChecker health_checker(config.backends, config.health_check_interval);
    health_checker.start();

    // Pass configuration to the requested core
    if (config.use_epoll) {
        if (config.workers > 1) {
            LOG_INFO("Using High-Performance EpollCore architecture with " << config.workers << " workers (SO_REUSEPORT).");
            std::vector<std::thread> workers;
            for (int i = 0; i < config.workers; ++i) {
                workers.emplace_back([&config, algo, i]() {
                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    CPU_SET(i, &cpuset);
                    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

                    LOG_INFO("Worker " << i << " started EpollCore on Core " << i);
                    EpollCore proxy(config.port, config.backends, algo);
                    proxy.start();
                });
            }
            for (auto& w : workers) {
                if (w.joinable()) w.join();
            }
        } else {
            LOG_INFO("Using High-Performance EpollCore architecture (Single Core).");
            EpollCore proxy(config.port, config.backends, algo);
            proxy.start(); // Blocks main thread
        }
    } else {
        LOG_INFO("Using Thread-per-Connection ProxyCore architecture.");
        ProxyCore proxy(config.port, config.backends, algo);
        proxy.start(); // Blocks main thread
    }

    return 0;
}
