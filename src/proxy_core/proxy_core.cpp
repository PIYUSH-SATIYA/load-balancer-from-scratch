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
#include <fcntl.h>
#include <poll.h>

ConnectionTracker::~ConnectionTracker() {
    // Centralize descriptor cleanup and accounting so two relay threads cannot
    // each decrement the same backend connection.
    if (client_fd >= 0) close(client_fd);
    if (backend_fd >= 0) close(backend_fd);
    if (backend) backend->active_connections.fetch_sub(1);
}

ProxyCore::ProxyCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm, bool passive_failover) 
    : listening_port(port), backends(backends), algorithm(algorithm), passive_failover(passive_failover), server_fd(-1) {
    
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

    // SO_REUSEADDR helps restart after prior connections. SO_REUSEPORT lets
    // multiple independent worker listeners bind this same port; Linux shares
    // incoming connections among them in the multi-worker configuration.
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

    LOG_INFO("Proxy listening on port " << listening_port << " (Passive Failover: " << (passive_failover ? "ON" : "OFF") << ")...");

    // Keep accept() in the main thread, then delegate backend connect and relay
    // setup so a slow upstream does not stop the process accepting new clients.
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
                Backend* selected_backend = nullptr;
                int backend_fd = -1;
                size_t max_attempts = passive_failover ? backends.size() : 1;

                for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
                    selected_backend = algorithm->select_backend(backends, client_ip);
                    if (!selected_backend) break;

                    backend_fd = socket(AF_INET, SOCK_STREAM, 0);
                    if (backend_fd < 0) break;

                    if (!selected_backend->addr_resolved.load()) {
                        if (!selected_backend->resolve_address()) {
                            close(backend_fd);
                            backend_fd = -1;
                            if (passive_failover) {
                                selected_backend->status.store(BackendStatus::DOWN);
                                LOG_WARN("PassiveFailover (ProxyCore): Failed to resolve " << selected_backend->ip << ":" << selected_backend->port << ", marking DOWN.");
                                continue;
                            }
                            break;
                        }
                    }

                    // Make just the connect phase nonblocking so poll can bound
                    // the wait; restore flags after success because these relay
                    // loops are intentionally the blocking baseline.
                    const int old_flags = fcntl(backend_fd, F_GETFL, 0);
                    bool connected = old_flags >= 0 && fcntl(backend_fd, F_SETFL, old_flags | O_NONBLOCK) == 0;
                    int connect_error = connected ? 0 : errno;
                    if (connected && connect(backend_fd, (struct sockaddr*)&selected_backend->addr,
                                             sizeof(selected_backend->addr)) < 0) {
                        if (errno == EINPROGRESS) {
                            pollfd event{backend_fd, POLLOUT, 0};
                            int ready;
                            do { ready = poll(&event, 1, 250); } while (ready < 0 && errno == EINTR);
                            socklen_t error_size = sizeof(connect_error);
                            if (ready > 0 && getsockopt(backend_fd, SOL_SOCKET, SO_ERROR,
                                                        &connect_error, &error_size) == 0) {
                                connected = connect_error == 0;
                            } else {
                                connect_error = ready == 0 ? ETIMEDOUT : errno;
                            }
                        } else {
                            connect_error = errno;
                            connected = false;
                        }
                    }
                    if (connected && fcntl(backend_fd, F_SETFL, old_flags) < 0) {
                        connect_error = errno;
                        connected = false;
                    }
                    if (!connected) {
                        close(backend_fd);
                        backend_fd = -1;
                        if (passive_failover) {
                            selected_backend->status.store(BackendStatus::DOWN);
                            LOG_WARN("PassiveFailover (ProxyCore): Backend " << selected_backend->ip << ":" << selected_backend->port << " failed to connect (" << strerror(connect_error) << "), marking DOWN and retrying next backend.");
                            continue;
                        }
                        break;
                    }

                    // Successfully connected
                    break;
                }

                if (backend_fd < 0 || !selected_backend) {
                    close(client_fd);
                    return;
                }

                // Only count a live upstream session. The shared tracker removes
                // this count after both relay directions have finished.
                selected_backend->active_connections.fetch_add(1);
                auto tracker = std::make_shared<ConnectionTracker>(selected_backend, client_fd, backend_fd);

                // Spawn second relay thread for backend->client, and use this current thread for client->backend
                try {
                    std::thread(relay_traffic, backend_fd, client_fd, tracker).detach();
                    relay_traffic(client_fd, backend_fd, tracker);
                } catch (const std::system_error& e) {
                    (void)e; // tracker owns descriptor cleanup and accounting
                }
            }).detach();
        } catch (const std::system_error& e) {
            close(client_fd);
        }
    }
}

void ProxyCore::relay_traffic(int from_fd, int to_fd, std::shared_ptr<ConnectionTracker> tracker) {
    // One call moves one direction. The stack buffer bounds temporary copy
    // space; blocking recv/send keeps this comparison model simple to inspect.
    char buffer[8192];
    bool fatal = false;
    
    while (true) {
        // Read from source
        ssize_t bytes_read = recv(from_fd, buffer, sizeof(buffer), 0);
        
        // --- CONNECTION CLOSE VS ERROR DETECTION ---
        if (bytes_read == 0) {
            // recv() returns exactly 0 when the peer performs an orderly shutdown (TCP FIN).
            break;
        } else if (bytes_read < 0 && errno == EINTR) {
            continue;
        } else if (bytes_read < 0) {
            // recv() returns -1 on an error.
            fatal = true;
            break;
        }

        // Write to destination
        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read) {
            ssize_t result = send(to_fd, buffer + bytes_sent, bytes_read - bytes_sent, 0);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) {
                fatal = true;
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
    if (fatal) {
        shutdown(from_fd, SHUT_RDWR);
        shutdown(to_fd, SHUT_RDWR);
    } else {
        shutdown(to_fd, SHUT_WR);
    }
    (void)tracker; // Retain ownership until both relay directions have finished.
}
