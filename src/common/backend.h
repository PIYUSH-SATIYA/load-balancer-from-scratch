#pragma once
#include <string>
#include <atomic>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <mutex>

enum class BackendStatus {
    UP,
    DOWN
};

struct Backend {
    std::string ip;
    int port;
    std::atomic<BackendStatus> status;
    std::atomic<int> active_connections;
    std::atomic<int> backend_selections;
    struct sockaddr_in addr;
    std::atomic<bool> addr_resolved;
    std::mutex address_mutex;

    Backend(std::string ip, int port) 
        : ip(ip), port(port), status(BackendStatus::UP), active_connections(0), backend_selections(0), addr_resolved(false) {
        memset(&addr, 0, sizeof(addr));
        resolve_address();
    }

    bool resolve_address() {
        std::lock_guard<std::mutex> lock(address_mutex);
        if (addr_resolved.load()) return true;
        struct addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
            memcpy(&addr, res->ai_addr, sizeof(struct sockaddr_in));
            freeaddrinfo(res);
            addr_resolved.store(true);
            return true;
        }
        return false;
    }
    
    // Delete copy constructor/assignment because of std::atomic
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    
    // Enable move semantics
    Backend(Backend&& other) noexcept 
        : ip(std::move(other.ip)), 
          port(other.port), 
          status(other.status.load()), 
          active_connections(other.active_connections.load()),
          backend_selections(other.backend_selections.load()),
          addr(other.addr),
          addr_resolved(other.addr_resolved.load()) {}
};
