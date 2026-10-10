#include "health_checker.h"
#include "../common/logger.h"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <fcntl.h>
#include <chrono>

HealthChecker::HealthChecker(std::vector<Backend>& backends, int interval) 
    : backends(backends), interval_seconds(interval), running(false) {}

HealthChecker::~HealthChecker() {
    running = false;
    if (checker_thread.joinable()) {
        checker_thread.join();
    }
}

void HealthChecker::start() {
    running = true;
    checker_thread = std::thread(&HealthChecker::check_loop, this);
}

bool HealthChecker::check_backend(const std::string& ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    // Set socket to non-blocking. If a backend is completely unresponsive (e.g. dropped packets),
    // a blocking connect() could hang for a long time (OS default is often minutes).
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        close(sock);
        return false;
    }

    bool is_up = false;
    int result = connect(sock, res->ai_addr, res->ai_addrlen);
    
    if (result == 0) {
        is_up = true; // Connected immediately
    } else if (result < 0 && errno == EINPROGRESS) {
        // Connection is in progress. We wait up to 2 seconds for it to succeed.
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        
        struct timeval tv;
        tv.tv_sec = 2; // 2 seconds timeout
        tv.tv_usec = 0;

        // select() will unblock if the socket becomes writable (meaning connection succeeded or failed)
        if (select(sock + 1, nullptr, &fdset, nullptr, &tv) == 1) {
            int so_error;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error == 0) {
                is_up = true;
            }
        }
    }

    freeaddrinfo(res);
    close(sock);
    return is_up;
}

void HealthChecker::check_loop() {
    LOG_INFO("HealthChecker background thread started.");
    
    while (running) {
        for (auto& backend : backends) {
            bool is_up = check_backend(backend.ip, backend.port);
            
            BackendStatus old_status = backend.status.load();
            BackendStatus new_status = is_up ? BackendStatus::UP : BackendStatus::DOWN;

            // Only log if the status actually changed to prevent log spam
            if (old_status != new_status) {
                backend.status.store(new_status);
                if (is_up) {
                    LOG_INFO("HealthCheck: " << backend.ip << ":" << backend.port << " is now UP");
                } else {
                    LOG_WARN("HealthCheck: " << backend.ip << ":" << backend.port << " is now DOWN");
                }
            }
        }

        // Sleep until the next interval
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
    }
}
