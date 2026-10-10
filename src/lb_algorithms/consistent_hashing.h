#pragma once
#include "lb_algorithms.h"
#include <map>
#include <string>

class ConsistentHashing : public LbAlgorithm {
public:
    // We pass the initial backends to pre-compute the hash ring
    ConsistentHashing(const std::vector<Backend>& initial_backends, int virtual_nodes = 100);
    ~ConsistentHashing() override = default;

    Backend* select_backend(std::vector<Backend>& backends, const std::string& client_ip) override;

private:
    int virtual_nodes;
    
    // The Hash Ring. 
    // std::map automatically keeps keys (hashes) sorted!
    // Key: Hash value, Value: Index of the backend in the vector
    std::map<size_t, size_t> hash_ring; 

    void add_node(size_t backend_index, const std::string& ip, int port);
};
