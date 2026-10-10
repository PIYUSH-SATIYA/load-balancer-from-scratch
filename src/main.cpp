#include "proxy_core/proxy_core.h"
#include "lb_algorithms/round_robin.h"
#include "lb_algorithms/least_connections.h"
#include "lb_algorithms/consistent_hashing.h"
#include "config/config.h"
#include "common/logger.h"
#include <vector>
#include <memory>
#include <csignal>

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
    LOG_INFO("Starting Load Balancer (Iteration 4: Config & Logging)");

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

    // Pass configuration to the proxy
    ProxyCore proxy(config.port, config.backends, algo);
    
    // start() blocks the main thread in the accept loop
    proxy.start();

    return 0;
}
