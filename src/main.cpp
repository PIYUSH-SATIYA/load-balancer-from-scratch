#include "proxy_core/proxy_core.h"
#include "lb_algorithms/round_robin.h"
#include "common/logger.h"
#include <vector>
#include <memory>

int main() {
    LOG_INFO("Starting Load Balancer (Iteration 3: Round Robin)");

    // Initialize our backend pool
    std::vector<Backend> backends;
    backends.emplace_back("backend1", 9001);
    backends.emplace_back("backend2", 9002);
    backends.emplace_back("backend3", 9003);

    // Initialize the Round Robin algorithm
    std::shared_ptr<LbAlgorithm> algo = std::make_shared<RoundRobin>();

    // Pass them to the proxy
    ProxyCore proxy(8080, backends, algo);
    
    // start() blocks the main thread in the accept loop
    proxy.start();

    return 0;
}
