#include "round_robin.h"

RoundRobin::RoundRobin() : counter(0) {}

Backend* RoundRobin::select_backend(std::vector<Backend>& backends, const std::string& /*client_ip*/) {
    if (backends.empty()) {
        return nullptr;
    }

    // fetch_add(1) atomically increments the counter and returns the value BEFORE the increment.
    // This guarantees no two threads will ever get the same counter value, 
    // cleanly preventing race conditions.
    unsigned int index = counter.fetch_add(1) % backends.size();
    
    Backend& selected = backends[index];
    selected.total_requests.fetch_add(1);
    
    return &selected;
}
