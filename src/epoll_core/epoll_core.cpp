#include "epoll_core.h"
#include "../common/logger.h"
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <signal.h>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 8192

EpollCore::EpollCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm)
    : listening_port(port), backends(backends), algorithm(algorithm), server_fd(-1), epoll_fd(-1) {
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE on broken sockets
}

EpollCore::~EpollCore() {
    if (server_fd >= 0) close(server_fd);
    if (epoll_fd >= 0) close(epoll_fd);
}

void EpollCore::set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void EpollCore::update_epoll(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

void EpollCore::remove_epoll(int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

// Dynamically computes whether we should listen for EPOLLIN / EPOLLOUT based on the buffer state and TCP half-close state.
void EpollCore::update_session_epoll(std::shared_ptr<Session> session) {
    if (session->fully_closed) return;

    uint32_t c_events = 0;
    if (!session->client_read_closed) c_events |= EPOLLIN;
    if (!session->b2c_buffer.empty()) c_events |= EPOLLOUT;
    update_epoll(session->client_fd, c_events);

    uint32_t b_events = 0;
    if (!session->backend_read_closed) b_events |= EPOLLIN;
    if (!session->c2b_buffer.empty()) b_events |= EPOLLOUT;
    update_epoll(session->backend_fd, b_events);
}

// If both ends have signaled EOF and all buffers are fully flushed, we can finally destroy the session.
void EpollCore::check_session_done(std::shared_ptr<Session> session) {
    if (session->fully_closed) return;

    if (session->client_read_closed && session->backend_read_closed &&
        session->c2b_buffer.empty() && session->b2c_buffer.empty()) {
        close_session(session);
    }
}

void EpollCore::start() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_non_blocking(server_fd);

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(listening_port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOG_ERROR("Bind failed: " << strerror(errno));
        return;
    }

    listen(server_fd, SOMAXCONN);

    epoll_fd = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    LOG_INFO("EpollCore proxy listening on port " << listening_port << "...");

    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0 && errno != EINTR) break;

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;

            if (fd == server_fd) {
                handle_new_connection();
                continue;
            }

            auto it = sessions.find(fd);
            if (it == sessions.end()) continue; 
            auto session = it->second;

            if (revents & (EPOLLERR | EPOLLHUP)) {
                close_session(session);
                continue;
            }

            if (fd == session->client_fd) {
                if (revents & EPOLLIN) handle_client_read(session);
                if (revents & EPOLLOUT) handle_client_write(session);
            } else if (fd == session->backend_fd) {
                if (revents & EPOLLIN) handle_backend_read(session);
                if (revents & EPOLLOUT) handle_backend_write(session);
            }
        }
    }
}

void EpollCore::handle_new_connection() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) return;

    set_non_blocking(client_fd);
    std::string client_ip = inet_ntoa(client_addr.sin_addr);

    Backend* selected = algorithm->select_backend(backends, client_ip);
    if (!selected) {
        close(client_fd);
        return;
    }

    // Commented out to prevent log spam during extreme load testing
    // LOG_INFO("Accepted connection from " << client_ip << ":" << ntohs(client_addr.sin_port));
    // LOG_INFO("Routed connection to backend: " << selected->ip << ":" << selected->port);

    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) {
        close(client_fd);
        return;
    }
    set_non_blocking(backend_fd);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(selected->ip.c_str(), std::to_string(selected->port).c_str(), &hints, &res);

    connect(backend_fd, res->ai_addr, res->ai_addrlen); 
    freeaddrinfo(res);

    selected->active_connections.fetch_add(1);

    auto session = std::make_shared<Session>();
    session->client_fd = client_fd;
    session->backend_fd = backend_fd;
    session->backend_node = selected;
    session->client_read_closed = false;
    session->backend_read_closed = false;
    session->fully_closed = false;

    sessions[client_fd] = session;
    sessions[backend_fd] = session;

    struct epoll_event ev_c;
    ev_c.events = EPOLLIN;
    ev_c.data.fd = client_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_c);

    struct epoll_event ev_b;
    ev_b.events = EPOLLIN | EPOLLOUT; 
    ev_b.data.fd = backend_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, backend_fd, &ev_b);
}

void EpollCore::handle_client_read(std::shared_ptr<Session> session) {
    char buf[BUFFER_SIZE];
    ssize_t n = recv(session->client_fd, buf, sizeof(buf), 0);
    
    if (n > 0) {
        session->c2b_buffer.append(buf, n);
        update_session_epoll(session);
    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        session->client_read_closed = true;
        shutdown(session->backend_fd, SHUT_WR); // TCP Half-Close: Tell backend we are done sending
        update_session_epoll(session);
        check_session_done(session);
    }
}

void EpollCore::handle_client_write(std::shared_ptr<Session> session) {
    if (session->b2c_buffer.empty()) {
        update_session_epoll(session);
        return;
    }
    ssize_t n = send(session->client_fd, session->b2c_buffer.data(), session->b2c_buffer.size(), 0);
    if (n > 0) {
        session->b2c_buffer.erase(0, n);
        update_session_epoll(session);
        check_session_done(session);
    } else if (n < 0 && errno != EAGAIN) {
        close_session(session);
    }
}

void EpollCore::handle_backend_read(std::shared_ptr<Session> session) {
    char buf[BUFFER_SIZE];
    ssize_t n = recv(session->backend_fd, buf, sizeof(buf), 0);
    if (n > 0) {
        session->b2c_buffer.append(buf, n);
        update_session_epoll(session);
    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        session->backend_read_closed = true;
        shutdown(session->client_fd, SHUT_WR); // TCP Half-Close: Tell client we are done sending
        update_session_epoll(session);
        check_session_done(session);
    }
}

void EpollCore::handle_backend_write(std::shared_ptr<Session> session) {
    if (session->c2b_buffer.empty()) {
        update_session_epoll(session);
        return;
    }
    ssize_t n = send(session->backend_fd, session->c2b_buffer.data(), session->c2b_buffer.size(), 0);
    if (n > 0) {
        session->c2b_buffer.erase(0, n);
        update_session_epoll(session);
        check_session_done(session);
    } else if (n < 0 && errno != EAGAIN) {
        close_session(session);
    }
}

void EpollCore::close_session(std::shared_ptr<Session> session) {
    if (session->fully_closed) return;

    if (session->backend_node) {
        session->backend_node->active_connections.fetch_sub(1);
        session->backend_node = nullptr; 
    }
    
    remove_epoll(session->client_fd);
    remove_epoll(session->backend_fd);
    
    close(session->client_fd);
    close(session->backend_fd);

    sessions.erase(session->client_fd);
    sessions.erase(session->backend_fd);

    session->fully_closed = true;
    // LOG_INFO("Session closed gracefully.");
}
