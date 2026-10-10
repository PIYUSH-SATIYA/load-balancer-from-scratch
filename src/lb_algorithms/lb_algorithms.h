#pragma once
#include <vector>
#include <string>
#include "../common/backend.h"

// Base interface for all Load Balancing Algorithms
class LbAlgorithm {
public:
    virtual ~LbAlgorithm() = default;

    // Selects a backend from the pool.
    // client_ip is passed for algorithms like Consistent Hashing that need it.
    virtual Backend* select_backend(std::vector<Backend>& backends, const std::string& client_ip) = 0;
};
