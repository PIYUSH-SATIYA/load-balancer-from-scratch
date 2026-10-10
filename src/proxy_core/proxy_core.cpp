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
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        LOG_ERROR("setsockopt SO_REUSEADDR failed: " << strerror(errno));
        return;
    }

    // 3. Bind to the listening port
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Bind to all interfaces (0.0.0.0)
    address.sin_port = htons(listening_port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOG_ERROR("Bind failed on port " << listening_port << ": " << strerror(errno));
        return;
    }

    // 4. Listen for incoming connections
    if (listen(server_fd, 128) < 0) {
        LOG_ERROR("Listen failed: " << strerror(errno));
        return;
    }

    LOG_INFO("Proxy listening on port " << listening_port << "...");

    // 5. Main Accept Loop
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // accept() blocks until a new client establishes a full TCP handshake
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            LOG_WARN("Accept failed: " << strerror(errno));
            continue;
        }

        LOG_INFO("Accepted connection from " << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port));

        // 5.5 Select a backend using the active algorithm
        Backend* selected_backend = algorithm->select_backend(backends, inet_ntoa(client_addr.sin_addr));
        if (!selected_backend) {
            LOG_ERROR("No backends available!");
            close(client_fd);
            continue;
        }

        LOG_INFO("Routed connection to backend: " << selected_backend->ip << ":" << selected_backend->port);

        // 6. Connect to the selected backend
        int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (backend_fd < 0) {
            LOG_ERROR("Failed to create backend socket: " << strerror(errno));
            close(client_fd);
            continue;
        }

        // Resolve backend hostname (Docker compose DNS resolving)
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(selected_backend->ip.c_str(), std::to_string(selected_backend->port).c_str(), &hints, &res) != 0) {
            LOG_ERROR("Failed to resolve backend hostname: " << selected_backend->ip);
            close(client_fd);
            close(backend_fd);
            continue;
        }

        // connect() performs the TCP 3-way handshake with the backend
        if (connect(backend_fd, res->ai_addr, res->ai_addrlen) < 0) {
            LOG_ERROR("Failed to connect to backend " << selected_backend->ip << ":" << selected_backend->port << " - " << strerror(errno));
            freeaddrinfo(res);
            close(client_fd);
            close(backend_fd);
            continue;
        }
        freeaddrinfo(res);

        // 7. Increment active connections
        selected_backend->active_connections.fetch_add(1);

        // 8. Spawn two threads to handle bidirectional relay
        // We pass a shared_ptr<ConnectionTracker> to both threads.
        // When BOTH threads finish, the shared_ptr reference count hits 0, 
        // destroying the tracker and safely decrementing the connection exactly once.
        auto tracker = std::make_shared<ConnectionTracker>(selected_backend);

        std::thread(relay_traffic, client_fd, backend_fd, "Client->Backend", tracker).detach();
        std::thread(relay_traffic, backend_fd, client_fd, "Backend->Client", tracker).detach();
    }
}

void ProxyCore::relay_traffic(int from_fd, int to_fd, const std::string& direction, std::shared_ptr<ConnectionTracker> /*tracker*/) {
    char buffer[8192];
    
    while (true) {
        // Read from source
        ssize_t bytes_read = recv(from_fd, buffer, sizeof(buffer), 0);
        
        // --- CONNECTION CLOSE VS ERROR DETECTION ---
        if (bytes_read == 0) {
            // recv() returns exactly 0 when the peer performs an orderly shutdown (TCP FIN).
            LOG_INFO("[" << direction << "] Connection closed gracefully by peer.");
            break;
        } else if (bytes_read < 0) {
            // recv() returns -1 on an error.
            // ECONNRESET means the peer hard-aborted the connection (TCP RST).
            LOG_WARN("[" << direction << "] Read error: " << strerror(errno));
            break;
        }

        // Write to destination
        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read) {
            ssize_t result = send(to_fd, buffer + bytes_sent, bytes_read - bytes_sent, 0);
            if (result < 0) {
                LOG_WARN("[" << direction << "] Write error: " << strerror(errno));
                goto end_relay; // Break out of both loops
            }
            bytes_sent += result;
        }
    }

end_relay:
    // When one direction closes, we want to tear down both sockets.
    // For this basic proxy, simply closing both FDs triggers an error on the 
    // opposite thread (since it's reading/writing to a closed FD), cleanly stopping both.
    close(from_fd);
    close(to_fd);
}
