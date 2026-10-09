#pragma once
#include <string>

class ProxyCore {
public:
    ProxyCore(int port, const std::string& backend_host, int backend_port);
    ~ProxyCore();

    // Start the proxy loop, blocking the current thread
    void start();

private:
    int listening_port;
    std::string backend_host;
    int backend_port;
    int server_fd;

    // The relay loops run in their own threads
    static void relay_traffic(int from_fd, int to_fd, const std::string& direction);
};
