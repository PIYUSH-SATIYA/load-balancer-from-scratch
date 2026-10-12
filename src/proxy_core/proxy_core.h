#pragma once
#include <string>
#include <vector>
#include <memory>
#include "../common/backend.h"
#include "../lb_algorithms/lb_algorithms.h"

struct ConnectionTracker {
    Backend* backend;
    ConnectionTracker(Backend* b) : backend(b) {}
    ~ConnectionTracker() { 
        if (backend) {
            backend->active_connections.fetch_sub(1); 
        }
    }
};

class ProxyCore {
public:
    ProxyCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm);
    ~ProxyCore();

    // Start the proxy loop, blocking the current thread
    void start();

private:
    int listening_port;
    std::vector<Backend>& backends;
    std::shared_ptr<LbAlgorithm> algorithm;
    int server_fd;

    // The relay loops run in their own threads, tracker ensures decrements happen exactly once
    static void relay_traffic(int from_fd, int to_fd, std::shared_ptr<ConnectionTracker> tracker);
};
