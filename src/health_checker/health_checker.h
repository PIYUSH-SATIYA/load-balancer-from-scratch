#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include "../common/backend.h"

class HealthChecker {
public:
    HealthChecker(std::vector<Backend>& backends, int interval_seconds);
    ~HealthChecker();

    // Spawns the background health checking thread
    void start();

private:
    std::vector<Backend>& backends;
    int interval_seconds;
    std::thread checker_thread;
    std::atomic<bool> running;

    void check_loop();
    bool check_backend(const std::string& ip, int port);
};
