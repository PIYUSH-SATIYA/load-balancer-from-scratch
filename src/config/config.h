#pragma once
#include <string>
#include <vector>
#include "../common/backend.h"

struct Config {
    // Defaults live here so parsing and startup share one source of truth.
    // Backend hostnames match service names on the Compose network.
    // -p selects the client-facing TCP listener.
    int port = 8080;
    // -a selects the connection-routing strategy, not an HTTP request policy.
    std::string algorithm = "round_robin";
    // -h is the pause between sequential active TCP health-check sweeps.
    int health_check_interval = 5;
    // Keep the blocking model as the simple default; -e opts into Linux epoll.
    bool use_epoll = false;
    // Worker count applies only to epoll; each worker owns a separate loop.
    int workers = 1;
    // Retries apply to initial upstream connection failures, not midstream data.
    bool passive_failover = true;
    std::vector<Backend> backends;
};

class ConfigParser {
public:
    static Config parse_cli(int argc, char* argv[]);
};
