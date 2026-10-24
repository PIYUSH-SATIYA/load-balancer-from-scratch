#include "least_connections.h"

Backend* LeastConnections::select_backend(std::vector<Backend>& backends, const std::string& /*client_ip*/) {
    if (backends.empty()) {
        return nullptr;
    }

    // Serialize selections so concurrent callers do not all scan and choose
    // the same currently-light backend at once. Counters stay atomic because
    // setup/teardown updates happen outside this mutex. This is still a
    // best-effort snapshot: counts can change while this scan runs.
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
        least_loaded->backend_selections.fetch_add(1);
    }

    return least_loaded;
}
