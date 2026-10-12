#include "proxy_core.h"
#include "../common/logger.h"
#include <iostream>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <signal.h>

ProxyCore::ProxyCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm) 
    : listening_port(port), backends(backends), algorithm(algorithm), server_fd(-1) {
    
    // Suppress SIGPIPE. If we write to a socket that the other end has closed,
    // the OS sends SIGPIPE which kills the process by default. We want send() 
    // to just return -1 with errno=EPIPE instead.
    signal(SIGPIPE, SIG_IGN);
}

ProxyCore::~ProxyCore() {
    if (server_fd >= 0) {
        close(server_fd);
    }
}

void ProxyCore::start() {
    // 1. Create listening socket
    // AF_INET = IPv4, SOCK_STREAM = TCP
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR("Failed to create listening socket: " << strerror(errno));
        return;
    }

    // 2. Allow immediate port reuse (prevents TIME_WAIT binding issues)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    // 3. Bind to the listening port
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Bind to all interfaces (0.0.0.0)
    address.sin_port = htons(listening_port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOG_ERROR("Bind failed on port " << listening_port << ": " << strerror(errno));
        return;
    }

    // 4. Listen for incoming connections (use SOMAXCONN instead of 128)
    if (listen(server_fd, SOMAXCONN) < 0) {
        LOG_ERROR("Listen failed: " << strerror(errno));
        return;
    }

    LOG_INFO("Proxy listening on port " << listening_port << "...");

    // 5. Main Accept Loop: accept client and immediately delegate backend connection to worker thread
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // accept() blocks until a new client establishes a full TCP handshake
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            LOG_WARN("Accept failed: " << strerror(errno));
            continue;
        }

        std::string client_ip = inet_ntoa(client_addr.sin_addr);

        // Spawn a connection handler thread immediately to keep accept loop free of blocking I/O
        try {
            std::thread([this, client_fd, client_ip]() {
                Backend* selected_backend = algorithm->select_backend(backends, client_ip);
                if (!selected_backend) {
                    close(client_fd);
                    return;
                }

                int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (backend_fd < 0) {
                    close(client_fd);
                    return;
                }

                if (!selected_backend->addr_resolved) {
                    if (!selected_backend->resolve_address()) {
                        close(client_fd);
                        close(backend_fd);
                        return;
                    }
                }

                if (connect(backend_fd, (struct sockaddr*)&selected_backend->addr, sizeof(selected_backend->addr)) < 0) {
                    close(client_fd);
                    close(backend_fd);
                    return;
                }

                selected_backend->active_connections.fetch_add(1);
                auto tracker = std::make_shared<ConnectionTracker>(selected_backend);

                // Spawn second relay thread for backend->client, and use this current thread for client->backend
                try {
                    std::thread(relay_traffic, backend_fd, client_fd, tracker).detach();
                    relay_traffic(client_fd, backend_fd, tracker);
                } catch (const std::system_error& e) {
                    close(client_fd);
                    close(backend_fd);
                }
            }).detach();
        } catch (const std::system_error& e) {
            close(client_fd);
        }
    }
}

void ProxyCore::relay_traffic(int from_fd, int to_fd, std::shared_ptr<ConnectionTracker> /*tracker*/) {
    char buffer[8192];
    
    while (true) {
        // Read from source
        ssize_t bytes_read = recv(from_fd, buffer, sizeof(buffer), 0);
        
        // --- CONNECTION CLOSE VS ERROR DETECTION ---
        if (bytes_read == 0) {
            // recv() returns exactly 0 when the peer performs an orderly shutdown (TCP FIN).
            break;
        } else if (bytes_read < 0) {
            // recv() returns -1 on an error.
            break;
        }

        // Write to destination
        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read) {
            ssize_t result = send(to_fd, buffer + bytes_sent, bytes_read - bytes_sent, 0);
            if (result < 0) {
                goto end_relay; // Break out of both loops
            }
            bytes_sent += result;
        }
    }

end_relay:
    // When one direction closes, we want to tear down both sockets.
    // CRITICAL FIX: Calling close() on a file descriptor does NOT interrupt a blocking 
    // recv() call on another thread! The other thread will hang forever.
    // shutdown() operates on the underlying socket object and forcefully unblocks 
    // any pending recv() or send() calls, ensuring the opposite thread wakes up and exits.
    shutdown(from_fd, SHUT_RDWR);
    shutdown(to_fd, SHUT_RDWR);
    
    close(from_fd);
    close(to_fd);
}
