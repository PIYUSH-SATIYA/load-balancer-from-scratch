#pragma once
#include "lb_algorithms.h"
#include <atomic>

class RoundRobin : public LbAlgorithm {
public:
    RoundRobin();
    ~RoundRobin() override = default;

    Backend* select_backend(std::vector<Backend>& backends, const std::string& client_ip) override;

private:
    // Atomic counter for thread-safe increments across multiple connections
    std::atomic<unsigned int> counter;
};
