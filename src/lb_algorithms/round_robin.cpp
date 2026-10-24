#include "round_robin.h"

RoundRobin::RoundRobin() : counter(0) {}

Backend* RoundRobin::select_backend(std::vector<Backend>& backends, const std::string& /*client_ip*/) {
    if (backends.empty()) {
        return nullptr;
    }

    // Consume at most one candidate per backend so an all-DOWN pool terminates.
    for (size_t i = 0; i < backends.size(); ++i) {
        // fetch_add(1) atomically increments the counter and returns the value BEFORE the increment.
        unsigned int index = counter.fetch_add(1) % backends.size();
        
        Backend& selected = backends[index];
        
        // The choice is per TCP connection, since the proxy forwards an opaque
        // stream and does not inspect individual HTTP requests.
        if (selected.status.load() == BackendStatus::UP) {
            selected.backend_selections.fetch_add(1);
            return &selected;
        }
        // If DOWN, the loop continues and it grabs the next one
    }
    
    // If we checked all backends and they are all DOWN
    return nullptr;
}
