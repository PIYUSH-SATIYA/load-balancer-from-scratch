#pragma once
#include <vector>
#include <string>
#include "../common/backend.h"

// Strategy interface: both I/O cores call the same policy, keeping routing
// independent from whether connections use threads or epoll.
class LbAlgorithm {
public:
    virtual ~LbAlgorithm() = default;

    // Return a pointer into the shared pool, or nullptr when none are eligible.
    // Selection happens once per TCP connection; only hashing needs client_ip.
    virtual Backend* select_backend(std::vector<Backend>& backends, const std::string& client_ip) = 0;
};
