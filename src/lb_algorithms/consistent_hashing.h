#pragma once
#include "lb_algorithms.h"
#include <map>
#include <string>

class ConsistentHashing : public LbAlgorithm {
public:
    // Build a fixed ring at startup; dynamic pool changes are not implemented.
    ConsistentHashing(const std::vector<Backend>& initial_backends, int virtual_nodes = 100);
    ~ConsistentHashing() override = default;

    Backend* select_backend(std::vector<Backend>& backends, const std::string& client_ip) override;

private:
    // Replicas improve expected spread with a small pool; 100 balances ring
    // size against smoother ownership ranges for this learning project.
    int virtual_nodes;
    
    // The Hash Ring. 
    // std::map automatically keeps keys (hashes) sorted!
    // Key: Hash value, Value: Index of the backend in the vector
    std::map<size_t, size_t> hash_ring; 

    void add_node(size_t backend_index, const std::string& ip, int port);
};
