#include "consistent_hashing.h"
#include <functional> // for std::hash

ConsistentHashing::ConsistentHashing(const std::vector<Backend>& initial_backends, int virtual_nodes) 
    : virtual_nodes(virtual_nodes) {
    // Populate the hash ring at startup
    for (size_t i = 0; i < initial_backends.size(); ++i) {
        add_node(i, initial_backends[i].ip, initial_backends[i].port);
    }
}

void ConsistentHashing::add_node(size_t backend_index, const std::string& ip, int port) {
    std::hash<std::string> hasher;
    
    // Create 'virtual_nodes' number of replicas for this backend on the ring.
    // This ensures a uniform distribution of traffic.
    for (int i = 0; i < virtual_nodes; ++i) {
        std::string vnode_name = ip + ":" + std::to_string(port) + "#" + std::to_string(i);
        size_t hash_val = hasher(vnode_name);
        hash_ring[hash_val] = backend_index;
    }
}

Backend* ConsistentHashing::select_backend(std::vector<Backend>& backends, const std::string& client_ip) {
    if (backends.empty() || hash_ring.empty()) return nullptr;

    std::hash<std::string> hasher;
    size_t client_hash = hasher(client_ip);

    // std::map::lower_bound(k) is O(log N). It finds the first element whose key is >= k.
    // This perfectly models "moving clockwise around the ring" until we hit a node.
    auto it = hash_ring.lower_bound(client_hash);
    
    // If we passed the last node, wrap around to the beginning of the ring
    if (it == hash_ring.end()) {
        it = hash_ring.begin();
    }

    size_t backend_index = it->second;
    
    // Fallback: If the selected backend is DOWN, walk the ring to find the next UP backend.
    if (backends[backend_index].status == BackendStatus::DOWN) {
        auto search_it = it;
        while (true) {
            search_it++;
            if (search_it == hash_ring.end()) search_it = hash_ring.begin();
            
            if (search_it == it) break; // We checked the whole ring and everything is down
            
            if (backends[search_it->second].status == BackendStatus::UP) {
                backend_index = search_it->second;
                break;
            }
        }
    }

    Backend& selected = backends[backend_index];
    if (selected.status == BackendStatus::DOWN) {
        return nullptr;
    }
    selected.total_requests.fetch_add(1);
    
    return &selected;
}
