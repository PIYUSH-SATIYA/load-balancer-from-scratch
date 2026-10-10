#pragma once
#include <string>
#include <atomic>

enum class BackendStatus {
    UP,
    DOWN
};

struct Backend {
    std::string ip;
    int port;
    std::atomic<BackendStatus> status;
    std::atomic<int> active_connections;
    std::atomic<int> total_requests;

    Backend(std::string ip, int port) 
        : ip(ip), port(port), status(BackendStatus::UP), active_connections(0), total_requests(0) {}
    
    // Delete copy constructor/assignment because of std::atomic
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    
    // Enable move semantics
    Backend(Backend&& other) noexcept 
        : ip(std::move(other.ip)), 
          port(other.port), 
          status(other.status.load()), 
          active_connections(other.active_connections.load()),
          total_requests(other.total_requests.load()) {}
};
