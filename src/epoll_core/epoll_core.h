#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include "../common/backend.h"
#include "../lb_algorithms/lb_algorithms.h"
#include <sys/epoll.h>
#include <cstring>
#include <chrono>
#include <unordered_set>

struct Buffer {
    // Fixed capacity bounds queued user bytes in this direction. Session owns
    // Buffers lazily, so idle connections do not allocate both 16 KiB buffers.
    char data[16384];
    int head = 0;
    int tail = 0;

    bool empty() const { return head == tail; }
    int length() const { return tail - head; }
    int space() const { return sizeof(data) - tail; }

    void consume(int n) {
        head += n;
        if (head == tail) head = tail = 0;
    }
    
    void compact() {
        if (head > 0 && tail > head) {
            memmove(data, data + head, tail - head);
            tail -= head;
            head = 0;
        }
    }
};

struct Session {
    // This state machine joins one client fd and one backend fd. The event loop
    // is single-threaded, so its mutable fields do not need per-session locks.
    int client_fd = -1;
    int backend_fd = -1;
    Backend* backend_node = nullptr; // To decrement active_connections

    std::unique_ptr<Buffer> c2b_buffer; // Allocated on-demand, freed on flush
    std::unique_ptr<Buffer> b2c_buffer; // Allocated on-demand, freed on flush

    uint32_t client_events = 0;
    uint32_t backend_events = 0;

    bool client_read_closed = false;
    bool backend_read_closed = false;
    bool fully_closed = false;

    // Passive failover state
    bool backend_connected = false;
    std::string client_ip;
    size_t failover_retries = 0;
    std::chrono::steady_clock::time_point connect_start_time;
};

class EpollCore {
public:
    EpollCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm, bool passive_failover = true);
    ~EpollCore();

    void start();

private:
    int listening_port;
    std::vector<Backend>& backends;
    std::shared_ptr<LbAlgorithm> algorithm;
    bool passive_failover;
    // Avoid retaining sessions for the kernel's much longer connect timeout
    // when a backend drops SYNs or a route is blackholed.
    int connect_timeout_ms = 250;
    int server_fd;
    int epoll_fd;

    // Both descriptors map to one session because readiness may arrive on
    // either socket; shared ownership simplifies teardown/retry paths.
    std::unordered_map<int, std::shared_ptr<Session>> sessions;
    std::unordered_set<std::shared_ptr<Session>> pending_connects;

    bool set_non_blocking(int fd);
    bool update_epoll(int fd, uint32_t events);
    void remove_epoll(int fd);
    
    void update_session_epoll(std::shared_ptr<Session> session);
    void check_session_done(std::shared_ptr<Session> session);

    void handle_new_connection();
    bool connect_session_backend(std::shared_ptr<Session> session);
    void handle_client_read(std::shared_ptr<Session> session);
    void handle_client_write(std::shared_ptr<Session> session);
    void handle_backend_read(std::shared_ptr<Session> session);
    void handle_backend_write(std::shared_ptr<Session> session);
    void close_session(std::shared_ptr<Session> session);
};
