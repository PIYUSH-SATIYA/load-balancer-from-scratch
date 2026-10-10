#include "round_robin.h"

RoundRobin::RoundRobin() : counter(0) {}

Backend* RoundRobin::select_backend(std::vector<Backend>& backends, const std::string& /*client_ip*/) {
    if (backends.empty()) {
        return nullptr;
    }

    // Try up to backends.size() times to find an UP backend
    for (size_t i = 0; i < backends.size(); ++i) {
        // fetch_add(1) atomically increments the counter and returns the value BEFORE the increment.
        unsigned int index = counter.fetch_add(1) % backends.size();
        
        Backend& selected = backends[index];
        
        // If it's UP, we found our target!
        if (selected.status.load() == BackendStatus::UP) {
            selected.total_requests.fetch_add(1);
            return &selected;
        }
        // If DOWN, the loop continues and it grabs the next one
    }
    
    // If we checked all backends and they are all DOWN
    return nullptr;
}
