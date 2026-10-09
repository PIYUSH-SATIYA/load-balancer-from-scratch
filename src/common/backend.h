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
    BackendStatus status;
    std::atomic<int> active_connections;

    Backend(std::string ip, int port) 
        : ip(ip), port(port), status(BackendStatus::UP), active_connections(0) {}
    
    // Delete copy constructor/assignment because of std::atomic
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    
    // Enable move semantics
    Backend(Backend&& other) noexcept 
        : ip(std::move(other.ip)), 
          port(other.port), 
          status(other.status), 
          active_connections(other.active_connections.load()) {}
};
