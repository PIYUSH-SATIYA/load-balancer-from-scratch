#include "proxy_core/proxy_core.h"
#include "common/logger.h"

int main() {
    LOG_INFO("Starting Load Balancer (Iteration 2: Dumb Proxy)");

    // Hardcode listening port 8080, and target backend1 on port 9001
    ProxyCore proxy(8080, "backend1", 9001);
    
    // start() blocks the main thread in the accept loop
    proxy.start();

    return 0;
}
