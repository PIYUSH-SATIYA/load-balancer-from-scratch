#pragma once
#include <string>
#include <atomic>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>

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
    struct sockaddr_in addr;
    bool addr_resolved = false;

    Backend(std::string ip, int port) 
        : ip(ip), port(port), status(BackendStatus::UP), active_connections(0), total_requests(0), addr_resolved(false) {
        memset(&addr, 0, sizeof(addr));
        resolve_address();
    }

    bool resolve_address() {
        struct addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
            memcpy(&addr, res->ai_addr, sizeof(struct sockaddr_in));
            freeaddrinfo(res);
            addr_resolved = true;
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
          total_requests(other.total_requests.load()),
          addr(other.addr),
          addr_resolved(other.addr_resolved) {}
};
