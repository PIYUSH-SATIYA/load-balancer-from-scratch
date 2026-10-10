#pragma once
#include <string>
#include <vector>
#include "../common/backend.h"

struct Config {
    int port = 8080;
    std::string algorithm = "round_robin";
    int health_check_interval = 5;
    bool use_epoll = false;
    std::vector<Backend> backends;
};

class ConfigParser {
public:
    static Config parse_cli(int argc, char* argv[]);
};
