#pragma once
#include <string>
#include <vector>
#include <memory>
#include "../common/backend.h"
#include "../lb_algorithms/lb_algorithms.h"

struct ConnectionTracker {
    // Both relay threads share this owner. Its destructor closes each fd and
    // decrements the backend count once, after the last relay releases it.
    Backend* backend;
    int client_fd;
    int backend_fd;
    ConnectionTracker(Backend* b, int client, int upstream)
        : backend(b), client_fd(client), backend_fd(upstream) {}
    ~ConnectionTracker();
};

class ProxyCore {
public:
    ProxyCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm, bool passive_failover = true);
    ~ProxyCore();

    // Start the proxy loop, blocking the current thread
    void start();

private:
    int listening_port;
    std::vector<Backend>& backends;
    std::shared_ptr<LbAlgorithm> algorithm;
    bool passive_failover;
    int server_fd;

    // One blocking relay runs in each direction; tracker lifetime ties their
    // cleanup together even when one side half-closes before the other.
    static void relay_traffic(int from_fd, int to_fd, std::shared_ptr<ConnectionTracker> tracker);
};
