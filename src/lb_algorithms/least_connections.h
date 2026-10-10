#pragma once
#include "lb_algorithms.h"
#include <mutex>

class LeastConnections : public LbAlgorithm {
public:
    LeastConnections() = default;
    ~LeastConnections() override = default;

    Backend* select_backend(std::vector<Backend>& backends, const std::string& client_ip) override;

private:
    std::mutex mtx; // Guards the scan to prevent race conditions during iteration
};
