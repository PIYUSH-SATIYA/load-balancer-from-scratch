#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include "../common/backend.h"

class HealthChecker {
public:
    // Keep a reference to the shared objects so status changes immediately
    // affect the routing algorithms' view of backend eligibility.
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
