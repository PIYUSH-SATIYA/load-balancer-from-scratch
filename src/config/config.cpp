#include "config.h"
#include "../common/logger.h"
#include <unistd.h>
#include <iostream>
#include <cstdlib>

Config ConfigParser::parse_cli(int argc, char* argv[]) {
    Config config;
    
    int opt;
    // In getopt's option string, ':' means the option consumes a value:
    // p/a/h/b/w take arguments; e/F are switches. -F disables the default-on
    // passive retry behavior so the safer default is to try another backend.
    while ((opt = getopt(argc, argv, "p:a:h:b:ew:F")) != -1) {
        switch (opt) {
            case 'p':
                // Listener port is separate from each upstream port in -b.
                config.port = std::stoi(optarg);
                break;
            case 'a':
                // Keep algorithm selection as a string so the factory in main
                // owns validation and fallback behavior in one place.
                config.algorithm = optarg;
                break;
            case 'h':
                // Seconds keep the CLI human-readable for a periodic probe.
                config.health_check_interval = std::stoi(optarg);
                break;
            case 'w':
                // A count is meaningful only with -e; it creates that many
                // independent event loops/listeners, not a thread pool.
                config.workers = std::stoi(optarg);
                break;
            case 'b': {
                // This simple split supports DNS names and IPv4 endpoints.
                // Bracketed IPv6 literals need a different parser and are not
                // supported by this project's configuration format.
                std::string b = optarg;
                size_t colon = b.find(':');
                if (colon != std::string::npos) {
                    std::string ip = b.substr(0, colon);
                    int port = std::stoi(b.substr(colon + 1));
                    config.backends.emplace_back(ip, port);
                } else {
                    LOG_WARN("Invalid backend format. Use ip:port (e.g. backend1:9001)");
                }
                break;
            }
            case 'e':
                // Select the nonblocking event-loop architecture.
                config.use_epoll = true;
                break;
            case 'F':
                // Retain the legacy single-attempt behavior for comparison and
                // to isolate active health-check failover in the test suite.
                config.passive_failover = false;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-p port] [-a algorithm] [-h health_check_interval_sec] [-w workers] [-b backend_ip:port]... [-e] [-F (disable passive failover)]" << std::endl;
                exit(1);
        }
    }

    // Repeated -b options define the pool; otherwise use Docker Compose service
    // names, which resolve only when running on that Compose network.
    if (config.backends.empty()) {
        config.backends.emplace_back("backend1", 9001);
        config.backends.emplace_back("backend2", 9002);
        config.backends.emplace_back("backend3", 9003);
    }

    return config;
}
