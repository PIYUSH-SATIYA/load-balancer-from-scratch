#include "config.h"
#include "../common/logger.h"
#include <unistd.h>
#include <iostream>
#include <cstdlib>

Config ConfigParser::parse_cli(int argc, char* argv[]) {
    Config config;
    
    int opt;
    // -p: port, -a: algorithm, -h: health check interval, -b: backend, -e: enable epoll, -w: workers, -F: disable passive failover
    while ((opt = getopt(argc, argv, "p:a:h:b:ew:F")) != -1) {
        switch (opt) {
            case 'p':
                config.port = std::stoi(optarg);
                break;
            case 'a':
                config.algorithm = optarg;
                break;
            case 'h':
                config.health_check_interval = std::stoi(optarg);
                break;
            case 'w':
                config.workers = std::stoi(optarg);
                break;
            case 'b': {
                // Parse "ip:port" format
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
                config.use_epoll = true;
                break;
            case 'F':
                config.passive_failover = false;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-p port] [-a algorithm] [-h health_check_interval_sec] [-w workers] [-b backend_ip:port]... [-e] [-F (disable passive failover)]" << std::endl;
                exit(1);
        }
    }

    // Provide default backends if none were specified (useful for our docker-compose setup)
    if (config.backends.empty()) {
        config.backends.emplace_back("backend1", 9001);
        config.backends.emplace_back("backend2", 9002);
        config.backends.emplace_back("backend3", 9003);
    }

    return config;
}
