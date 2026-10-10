#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void handle_client(int client_fd) {
    char buffer[4096];
    while (true) {
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            std::string http_response = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 13\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: keep-alive\r\n\r\n"
                "Hello, World!";
            send(client_fd, http_response.c_str(), http_response.length(), 0);
        } else if (bytes_read == 0) {
            std::cout << "[Backend] Client disconnected cleanly." << std::endl;
            break;
        } else {
            std::cerr << "[Backend] Error reading from client." << std::endl;
            break;
        }
    }
    close(client_fd);
}

int main(int argc, char* argv[]) {
    int port = 9000;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[Backend] Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    // SO_REUSEADDR prevents "Address already in use" errors if we restart quickly
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        std::cerr << "[Backend] setsockopt failed\n";
        return 1;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "[Backend] Bind failed on port " << port << "\n";
        return 1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        std::cerr << "[Backend] Listen failed\n";
        return 1;
    }

    std::cout << "[Backend] Started echo server on port " << port << std::endl;

    std::vector<std::thread> threads;
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "[Backend] Accept failed\n";
            continue;
        }

        std::cout << "[Backend] Accepted connection from " 
                  << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) 
                  << std::endl;

        threads.emplace_back(std::thread(handle_client, client_fd));
        threads.back().detach(); // Detach so we don't need to join it
    }

    return 0;
}
