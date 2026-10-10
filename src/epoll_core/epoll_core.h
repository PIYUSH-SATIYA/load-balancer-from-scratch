#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include "../common/backend.h"
#include "../lb_algorithms/lb_algorithms.h"
#include <sys/epoll.h>

struct Session {
    int client_fd;
    int backend_fd;
    Backend* backend_node; // To decrement active_connections

    std::string c2b_buffer; // Client -> Backend buffer
    std::string b2c_buffer; // Backend -> Client buffer

    bool client_closed;
    bool backend_closed;
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

    // Maps file descriptors to their associated Session
    std::unordered_map<int, std::shared_ptr<Session>> sessions;

    void set_non_blocking(int fd);
    void update_epoll(int fd, uint32_t events);
    void remove_epoll(int fd);

    void handle_new_connection();
    void handle_client_read(std::shared_ptr<Session> session);
    void handle_client_write(std::shared_ptr<Session> session);
    void handle_backend_read(std::shared_ptr<Session> session);
    void handle_backend_write(std::shared_ptr<Session> session);
    void close_session(std::shared_ptr<Session> session);
};
