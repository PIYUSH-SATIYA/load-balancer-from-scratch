#pragma once
#include <string>
#include <vector>
#include <memory>
#include "../common/backend.h"
#include "../lb_algorithms/lb_algorithms.h"

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

    // The relay loops run in their own threads
    static void relay_traffic(int from_fd, int to_fd, const std::string& direction);
};
