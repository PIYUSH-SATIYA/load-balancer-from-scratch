#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {
struct ClientState {
    std::string output;
    std::size_t sent = 0;
    bool read_closed = false;
};

bool set_nonblocking(int fd) {
    // The test responder also uses readiness notifications, so accept/read/write
    // must return promptly instead of blocking the event loop on one client.
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_client(int epoll_fd, int fd, std::unordered_map<int, ClientState>& clients) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    clients.erase(fd);
}
} // namespace

int main(int argc, char* argv[]) {
    // One process per backend port lets Compose/harness create independent
    // upstreams while keeping response behavior identical across them.
    int port = 9000;
    if (argc > 1) port = std::stoi(argv[1]);

    const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0 || !set_nonblocking(server_fd)) {
        std::cerr << "[Backend] Failed to create nonblocking listener: " << strerror(errno) << '\n';
        return 1;
    }

    // Reuse the local port during repeated benchmark/test process restarts.
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server_fd, SOMAXCONN) < 0) {
        std::cerr << "[Backend] Failed to bind/listen on port " << port << ": " << strerror(errno) << '\n';
        close(server_fd);
        return 1;
    }

    const int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        std::cerr << "[Backend] epoll_create1 failed: " << strerror(errno) << '\n';
        close(server_fd);
        return 1;
    }
    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &listen_event) < 0) {
        std::cerr << "[Backend] Failed to register listener: " << strerror(errno) << '\n';
        close(epoll_fd);
        close(server_fd);
        return 1;
    }

    std::cout << "[Backend] Event-driven HTTP responder listening on port " << port << std::endl;
    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "X-Backend-Port: " + std::to_string(port) + "\r\n"
        "Connection: keep-alive\r\n\r\n"
        "Hello, World!";
    std::unordered_map<int, ClientState> clients;
    // This is only the number of ready client events processed per wait batch;
    // more connections can remain registered and be returned in later batches.
    epoll_event events[256];

    while (true) {
        const int count = epoll_wait(epoll_fd, events, 256, -1);
        if (count < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Backend] epoll_wait failed: " << strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < count; ++i) {
            const int fd = events[i].data.fd;
            if (fd == server_fd) {
                while (true) {
                    const int client_fd = accept(server_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::cerr << "[Backend] accept failed: " << strerror(errno) << '\n';
                        break;
                    }
                    if (!set_nonblocking(client_fd)) {
                        close(client_fd);
                        continue;
                    }
                    clients.emplace(client_fd, ClientState{});
                    epoll_event client_event{};
                    client_event.events = EPOLLIN | EPOLLRDHUP;
                    client_event.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) < 0) {
                        close_client(epoll_fd, client_fd, clients);
                    }
                }
                continue;
            }

            auto client = clients.find(fd);
            if (client == clients.end()) continue;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                close_client(epoll_fd, fd, clients);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                // The dummy backend does not parse HTTP; reading any bytes is
                // enough to trigger a fixed test response.
                char buffer[4096];
                const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
                if (n == 0) {
                    client->second.read_closed = true;
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    close_client(epoll_fd, fd, clients);
                    continue;
                }
                if (n > 0) {
                    client->second.output = response;
                    client->second.sent = 0;
                }
            }

            if (events[i].events & EPOLLOUT) {
                auto& state = client->second;
                const ssize_t n = send(fd, state.output.data() + state.sent,
                                       state.output.size() - state.sent, MSG_NOSIGNAL);
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    close_client(epoll_fd, fd, clients);
                    continue;
                }
                if (n > 0) state.sent += static_cast<std::size_t>(n);
                if (state.sent == state.output.size()) {
                    state.output.clear();
                    state.sent = 0;
                }
            }

            auto refreshed = clients.find(fd);
            if (refreshed == clients.end()) continue;
            if (refreshed->second.read_closed && refreshed->second.output.empty()) {
                close_client(epoll_fd, fd, clients);
                continue;
            }
            epoll_event client_event{};
            client_event.events = EPOLLRDHUP | (refreshed->second.output.empty() ? EPOLLIN : EPOLLOUT);
            client_event.data.fd = fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &client_event) < 0) {
                close_client(epoll_fd, fd, clients);
            }
        }
    }

    for (const auto& [fd, _] : clients) close(fd);
    close(epoll_fd);
    close(server_fd);
    return 1;
}
