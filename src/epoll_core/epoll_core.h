#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include "../common/backend.h"
#include "../lb_algorithms/lb_algorithms.h"
#include <sys/epoll.h>
#include <cstring>

struct Buffer {
    char data[16384]; // 16KB static buffer
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
    int client_fd;
    int backend_fd;
    Backend* backend_node = nullptr; // To decrement active_connections

    std::unique_ptr<Buffer> c2b_buffer; // Allocated on-demand, freed on flush
    std::unique_ptr<Buffer> b2c_buffer; // Allocated on-demand, freed on flush

    uint32_t client_events = 0;
    uint32_t backend_events = 0;

    bool client_read_closed = false;
    bool backend_read_closed = false;
    bool fully_closed = false;
};

class EpollCore {
public:
    EpollCore(int port, std::vector<Backend>& backends, std::shared_ptr<LbAlgorithm> algorithm);
    ~EpollCore();

    void start();

private:
    int listening_port;
    std::vector<Backend>& backends;
    std::shared_ptr<LbAlgorithm> algorithm;
    int server_fd;
    int epoll_fd;

    std::unordered_map<int, std::shared_ptr<Session>> sessions;

    void set_non_blocking(int fd);
    void update_epoll(int fd, uint32_t events);
    void remove_epoll(int fd);
    
    void update_session_epoll(std::shared_ptr<Session> session);
    void check_session_done(std::shared_ptr<Session> session);

    void handle_new_connection();
    void handle_client_read(std::shared_ptr<Session> session);
    void handle_client_write(std::shared_ptr<Session> session);
    void handle_backend_read(std::shared_ptr<Session> session);
    void handle_backend_write(std::shared_ptr<Session> session);
    void close_session(std::shared_ptr<Session> session);
};
