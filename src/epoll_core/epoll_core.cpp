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

// Maximum ready events returned by one epoll_wait call; this is a batch size,
// not a limit on how many descriptors the epoll instance can register.
#define MAX_EVENTS 1024

EpollCore::EpollCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm, bool passive_failover)
    : listening_port(port), backends(backends), algorithm(algorithm), passive_failover(passive_failover), server_fd(-1), epoll_fd(-1) {}

EpollCore::~EpollCore() {
    if (server_fd >= 0) close(server_fd);
    if (epoll_fd >= 0) close(epoll_fd);
}

bool EpollCore::set_non_blocking(int fd) {
    // Preserve existing file-status flags and add O_NONBLOCK for the event loop.
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool EpollCore::update_epoll(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl MOD failed for fd " << fd << ": " << strerror(errno));
        return false;
    }
    return true;
}

void EpollCore::remove_epoll(int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

// Dynamically computes whether we should listen for EPOLLIN / EPOLLOUT based on the buffer state and TCP half-close state.
void EpollCore::update_session_epoll(std::shared_ptr<Session> session) {
    if (session->fully_closed) return;

    if (session->client_fd >= 0) {
        uint32_t c_events = 0;
        // Stop reading when the destination queue is full: this applies
        // backpressure instead of growing an unbounded userspace queue.
        if (!session->client_read_closed && (!session->c2b_buffer || session->c2b_buffer->space() > 0)) c_events |= EPOLLIN;
        // EPOLLOUT is normally always-ready. Subscribe only while there are
        // bytes to flush, or the loop would wake repeatedly without progress.
        if (session->b2c_buffer && !session->b2c_buffer->empty()) c_events |= EPOLLOUT;
        
        if (c_events != session->client_events) {
            if (!update_epoll(session->client_fd, c_events)) {
                close_session(session);
                return;
            }
            session->client_events = c_events;
        }
    }

    if (session->backend_fd >= 0) {
        uint32_t b_events = 0;
        if (!session->backend_read_closed && (!session->b2c_buffer || session->b2c_buffer->space() > 0)) b_events |= EPOLLIN;
        if ((!session->backend_connected) || (session->c2b_buffer && !session->c2b_buffer->empty())) b_events |= EPOLLOUT;

        if (b_events != session->backend_events) {
            if (!update_epoll(session->backend_fd, b_events)) {
                close_session(session);
                return;
            }
            session->backend_events = b_events;
        }
    }
}

// If both ends have signaled EOF and all buffers are fully flushed, we can finally destroy the session.
void EpollCore::check_session_done(std::shared_ptr<Session> session) {
    if (session->fully_closed) return;

    bool c2b_done = !session->c2b_buffer || session->c2b_buffer->empty();
    bool b2c_done = !session->b2c_buffer || session->b2c_buffer->empty();

    if (session->client_read_closed && session->backend_read_closed && c2b_done && b2c_done) {
        close_session(session);
    }
}

void EpollCore::start() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR("Failed to create listening socket: " << strerror(errno));
        return;
    }
    int opt = 1;
    // SO_REUSEPORT allows each worker's independent listener to share a port;
    // SO_REUSEADDR makes restarting the listener practical after a prior run.
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    if (!set_non_blocking(server_fd)) {
        LOG_ERROR("Failed to make listening socket nonblocking: " << strerror(errno));
        return;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(listening_port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOG_ERROR("Bind failed: " << strerror(errno));
        return;
    }

    // SOMAXCONN asks for the platform's maximum listen backlog; the kernel may
    // cap it further through system settings.
    if (listen(server_fd, SOMAXCONN) < 0) {
        LOG_ERROR("Listen failed: " << strerror(errno));
        return;
    }

    // One epoll instance belongs to this core/worker and watches listener plus
    // every client/upstream socket it owns.
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_ERROR("epoll_create1 failed: " << strerror(errno));
        return;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        LOG_ERROR("Failed to register listening socket: " << strerror(errno));
        return;
    }

    LOG_INFO("EpollCore proxy listening on port " << listening_port << " (Passive Failover: " << (passive_failover ? "ON" : "OFF") << ")...");

    struct epoll_event events[MAX_EVENTS];

    // Level-triggered readiness is the default: if data remains unread, the fd
    // is reported again. Handlers can therefore do bounded work per event.
    while (true) {
        // Sleep until an event when no upstream connect is pending. Otherwise
        // wake every 50 ms to enforce connect_timeout_ms even if no readiness
        // event arrives for a blackholed destination.
        int timeout_ms = pending_connects.empty() ? -1 : 50;
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout_ms);
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

            // Handle pending backend connect completion / asynchronous connect failure
            if (fd == session->backend_fd && !session->backend_connected) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                const int socket_error = getsockopt(session->backend_fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (socket_error < 0) so_error = errno;
                if (socket_error == 0 && so_error == 0) {
                    session->backend_connected = true;
                    pending_connects.erase(session);
                    // Forward any buffered client request that arrived during connect
                    if (session->c2b_buffer && !session->c2b_buffer->empty()) {
                        ssize_t sent = send(session->backend_fd, session->c2b_buffer->data + session->c2b_buffer->head, session->c2b_buffer->length(), 0);
                        if (sent > 0) {
                            session->c2b_buffer->consume(sent);
                            if (session->c2b_buffer->empty()) session->c2b_buffer.reset();
                        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            close_session(session);
                            continue;
                        }
                    }
                    if (session->client_read_closed &&
                        (!session->c2b_buffer || session->c2b_buffer->empty())) {
                        shutdown(session->backend_fd, SHUT_WR);
                    }
                    update_session_epoll(session);
                } else {
                    // Async connection failed (e.g. ECONNREFUSED)
                    pending_connects.erase(session);
                    if (passive_failover) {
                        if (session->backend_node) {
                            session->backend_node->status.store(BackendStatus::DOWN);
                            LOG_WARN("PassiveFailover (EpollCore): Asynchronous connect error to " 
                                     << session->backend_node->ip << ":" << session->backend_node->port 
                                     << " (" << strerror(so_error) << "), marking DOWN and retrying next backend.");
                        }
                        if (connect_session_backend(session)) {
                            continue;
                        }
                    }
                    close_session(session);
                    continue;
                }
            }

            if (revents & EPOLLERR) {
                close_session(session);
                continue;
            }

            if (fd == session->client_fd) {
                if (revents & EPOLLIN) handle_client_read(session);
                if (!session->fully_closed && (revents & EPOLLOUT)) handle_client_write(session);
            } else if (fd == session->backend_fd) {
                if (revents & EPOLLIN) handle_backend_read(session);
                if (!session->fully_closed && (revents & EPOLLOUT)) handle_backend_write(session);
            }
        }

        // Check for connect timeouts on pending connects (e.g. dropped SYN / blackholed container)
        if (!pending_connects.empty()) {
            auto now = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<Session>> timed_out;
            for (const auto& s : pending_connects) {
                if (!s->backend_connected && (now - s->connect_start_time) >= std::chrono::milliseconds(connect_timeout_ms)) {
                    timed_out.push_back(s);
                }
            }
            for (auto& s : timed_out) {
                pending_connects.erase(s);
                if (passive_failover) {
                    if (s->backend_node) {
                        s->backend_node->status.store(BackendStatus::DOWN);
                        LOG_WARN("PassiveFailover (EpollCore): Connect timeout (" << connect_timeout_ms << "ms) to " 
                                 << s->backend_node->ip << ":" << s->backend_node->port 
                                 << ", marking DOWN and retrying next backend.");
                    }
                    if (!connect_session_backend(s)) {
                        close_session(s);
                    }
                } else {
                    close_session(s);
                }
            }
        }
    }
}

bool EpollCore::connect_session_backend(std::shared_ptr<Session> session) {
    if (session->fully_closed) return false;

    // Clean up previous backend descriptor if retrying
    if (session->backend_fd >= 0) {
        remove_epoll(session->backend_fd);
        sessions.erase(session->backend_fd);
        close(session->backend_fd);
        session->backend_fd = -1;
    }

    if (session->backend_node) {
        session->backend_node->active_connections.fetch_sub(1);
        session->backend_node = nullptr;
    }

    pending_connects.erase(session);
    session->backend_connected = false;

    // At most one attempt per configured backend in this session prevents a
    // failed pool from creating an infinite retry loop.
    size_t max_retries = passive_failover ? backends.size() : 1;
    while (session->failover_retries < max_retries) {
        session->failover_retries++;

        Backend* selected = algorithm->select_backend(backends, session->client_ip);
        if (!selected) {
            return false;
        }

        int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (backend_fd < 0) {
            return false;
        }
        if (!set_non_blocking(backend_fd)) {
            close(backend_fd);
            return false;
        }

        if (!selected->addr_resolved.load()) {
            if (!selected->resolve_address()) {
                close(backend_fd);
                if (passive_failover) {
                    selected->status.store(BackendStatus::DOWN);
                    LOG_WARN("PassiveFailover (EpollCore): Failed to resolve address for " << selected->ip << ":" << selected->port << ", marking DOWN.");
                    continue;
                }
                return false;
            }
        }

        session->connect_start_time = std::chrono::steady_clock::now();
        int conn_res = connect(backend_fd, (struct sockaddr*)&selected->addr, sizeof(selected->addr));
        if (conn_res < 0 && errno != EINPROGRESS) {
            // Immediate connect error (e.g. ECONNREFUSED)
            close(backend_fd);
            if (passive_failover) {
                selected->status.store(BackendStatus::DOWN);
                LOG_WARN("PassiveFailover (EpollCore): Immediate connection failure to " << selected->ip << ":" << selected->port << " (" << strerror(errno) << "), marking DOWN and retrying next backend.");
                continue;
            }
            return false;
        }

        session->backend_fd = backend_fd;
        session->backend_node = selected;
        selected->active_connections.fetch_add(1);

        sessions[backend_fd] = session;

        // EPOLLOUT signals either connect completion or room to flush queued
        // bytes, so subscribe while connecting and recompute after SO_ERROR.
        session->backend_events = EPOLLIN | EPOLLOUT;
        struct epoll_event ev_b;
        ev_b.events = EPOLLIN | EPOLLOUT;
        ev_b.data.fd = backend_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, backend_fd, &ev_b) < 0) {
            sessions.erase(backend_fd);
            close(backend_fd);
            session->backend_fd = -1;
            selected->active_connections.fetch_sub(1);
            session->backend_node = nullptr;
            if (passive_failover) continue;
            return false;
        }

        if (conn_res == 0) {
            session->backend_connected = true;
            pending_connects.erase(session);
            if (session->c2b_buffer && !session->c2b_buffer->empty()) {
                ssize_t sent = send(backend_fd, session->c2b_buffer->data + session->c2b_buffer->head, session->c2b_buffer->length(), 0);
                if (sent > 0) {
                    session->c2b_buffer->consume(sent);
                    if (session->c2b_buffer->empty()) session->c2b_buffer.reset();
                }
                update_session_epoll(session);
            }
        } else {
            session->backend_connected = false;
            pending_connects.insert(session);
        }

        return true;
    }

    return false;
}

void EpollCore::handle_new_connection() {
    // With a nonblocking listener, accept until EAGAIN: one readiness event can
    // represent multiple clients already queued in the kernel backlog.
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }

        if (!set_non_blocking(client_fd)) {
            close(client_fd);
            continue;
        }
        char client_ip_buffer[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_buffer, sizeof(client_ip_buffer)) == nullptr) {
            close(client_fd);
            continue;
        }
        std::string client_ip = client_ip_buffer;

        auto session = std::make_shared<Session>();
        session->client_fd = client_fd;
        session->client_ip = client_ip;
        session->client_read_closed = false;
        session->backend_read_closed = false;
        session->fully_closed = false;
        session->backend_connected = false;
        session->failover_retries = 0;

        sessions[client_fd] = session;

        session->client_events = EPOLLIN;
        struct epoll_event ev_c;
        ev_c.events = EPOLLIN;
        ev_c.data.fd = client_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_c) < 0) {
            LOG_WARN("Failed to register client socket " << client_fd << " with epoll: " << strerror(errno));
            close_session(session);
            continue;
        }

        if (!connect_session_backend(session)) {
            close_session(session);
            continue;
        }
    }
}

void EpollCore::handle_client_read(std::shared_ptr<Session> session) {
    // Allocate only when bytes arrive. If connect is still pending, preserve
    // the request in this bounded buffer until SO_ERROR confirms connection.
    if (!session->c2b_buffer) {
        session->c2b_buffer = std::make_unique<Buffer>();
    }
    if (session->c2b_buffer->space() == 0) session->c2b_buffer->compact();
    int space = session->c2b_buffer->space();
    if (space == 0) return;

    ssize_t n = recv(session->client_fd, session->c2b_buffer->data + session->c2b_buffer->tail, space, 0);
    
    if (n > 0) {
        session->c2b_buffer->tail += n;
        
        // Forward immediately only if backend connection is already established
        if (session->backend_connected && session->backend_fd >= 0) {
            ssize_t sent = send(session->backend_fd, session->c2b_buffer->data + session->c2b_buffer->head, session->c2b_buffer->length(), 0);
            if (sent > 0) {
                session->c2b_buffer->consume(sent);
                if (session->c2b_buffer->empty()) session->c2b_buffer.reset(); // Free 16KB on-demand
            } else if (sent == 0 || (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                close_session(session);
                return;
            }
            update_session_epoll(session);
        }
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        session->client_read_closed = true;
        if (session->backend_connected && session->backend_fd >= 0 &&
            (!session->c2b_buffer || session->c2b_buffer->empty())) {
            shutdown(session->backend_fd, SHUT_WR); // TCP Half-Close
        }
        update_session_epoll(session);
        check_session_done(session);
    }
}

void EpollCore::handle_client_write(std::shared_ptr<Session> session) {
    if (!session->b2c_buffer || session->b2c_buffer->empty()) {
        if (session->b2c_buffer && session->b2c_buffer->empty()) session->b2c_buffer.reset();
        if (session->backend_read_closed && session->client_fd >= 0) shutdown(session->client_fd, SHUT_WR);
        update_session_epoll(session);
        return;
    }
    ssize_t n = send(session->client_fd, session->b2c_buffer->data + session->b2c_buffer->head, session->b2c_buffer->length(), 0);
    if (n > 0) {
        session->b2c_buffer->consume(n);
        if (session->b2c_buffer->empty()) {
            session->b2c_buffer.reset();
            if (session->backend_read_closed) shutdown(session->client_fd, SHUT_WR);
        }
        update_session_epoll(session);
        check_session_done(session);
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close_session(session);
    }
}

void EpollCore::handle_backend_read(std::shared_ptr<Session> session) {
    // Symmetric path for backend-to-client data; a full buffer disables further
    // upstream reads until the client socket can accept more bytes.
    if (!session->b2c_buffer) {
        session->b2c_buffer = std::make_unique<Buffer>();
    }
    if (session->b2c_buffer->space() == 0) session->b2c_buffer->compact();
    int space = session->b2c_buffer->space();
    if (space == 0) return;

    ssize_t n = recv(session->backend_fd, session->b2c_buffer->data + session->b2c_buffer->tail, space, 0);
    
    if (n > 0) {
        session->b2c_buffer->tail += n;
        
        // Try forwarding immediately
        ssize_t sent = send(session->client_fd, session->b2c_buffer->data + session->b2c_buffer->head, session->b2c_buffer->length(), 0);
        if (sent > 0) {
            session->b2c_buffer->consume(sent);
            if (session->b2c_buffer->empty()) session->b2c_buffer.reset(); // Free 16KB on-demand
        } else if (sent == 0 || (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close_session(session);
            return;
        }
        update_session_epoll(session);
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        session->backend_read_closed = true;
        if (!session->b2c_buffer || session->b2c_buffer->empty()) {
            shutdown(session->client_fd, SHUT_WR); // TCP Half-Close: Tell client we are done sending
        }
        update_session_epoll(session);
        check_session_done(session);
    }
}

void EpollCore::handle_backend_write(std::shared_ptr<Session> session) {
    if (!session->c2b_buffer || session->c2b_buffer->empty()) {
        if (session->c2b_buffer && session->c2b_buffer->empty()) session->c2b_buffer.reset();
        if (session->client_read_closed && session->backend_fd >= 0) shutdown(session->backend_fd, SHUT_WR);
        update_session_epoll(session);
        return;
    }
    ssize_t n = send(session->backend_fd, session->c2b_buffer->data + session->c2b_buffer->head, session->c2b_buffer->length(), 0);
    if (n > 0) {
        session->c2b_buffer->consume(n);
        if (session->c2b_buffer->empty()) {
            session->c2b_buffer.reset();
            if (session->client_read_closed) shutdown(session->backend_fd, SHUT_WR);
        }
        update_session_epoll(session);
        check_session_done(session);
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close_session(session);
    }
}

void EpollCore::close_session(std::shared_ptr<Session> session) {
    // Idempotence matters: error, EOF, timeout, and failed epoll_ctl paths may
    // converge on cleanup. Marking closed prevents double accounting/close.
    if (session->fully_closed) return;

    pending_connects.erase(session);

    if (session->backend_node) {
        session->backend_node->active_connections.fetch_sub(1);
        session->backend_node = nullptr; 
    }
    
    if (session->client_fd >= 0) {
        remove_epoll(session->client_fd);
        close(session->client_fd);
        sessions.erase(session->client_fd);
        session->client_fd = -1;
    }
    if (session->backend_fd >= 0) {
        remove_epoll(session->backend_fd);
        close(session->backend_fd);
        sessions.erase(session->backend_fd);
        session->backend_fd = -1;
    }

    session->fully_closed = true;
}
