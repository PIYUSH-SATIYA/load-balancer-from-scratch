#include "least_connections.h"

Backend* LeastConnections::select_backend(std::vector<Backend>& backends, const std::string& /*client_ip*/) {
    if (backends.empty()) {
        return nullptr;
    }

    // We use a lock here to satisfy our learning objective regarding mutexes.
    // While `active_connections` is atomic, locking the whole scan prevents 
    // the state from shifting underneath us while we iterate, ensuring we 
    // make a decision based on a single consistent snapshot in time.
    std::lock_guard<std::mutex> lock(mtx);

    Backend* least_loaded = nullptr;
    int min_conns = -1;

    for (auto& backend : backends) {
        if (backend.status == BackendStatus::DOWN) continue;

        int conns = backend.active_connections.load();
        if (min_conns == -1 || conns < min_conns) {
            min_conns = conns;
            least_loaded = &backend;
        }
    }

    if (least_loaded) {
        least_loaded->total_requests.fetch_add(1);
    }

    return least_loaded;
}
