#include <iostream>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    std::string port = "unknown";
    if (argc > 1) {
        port = argv[1];
    }
    
    std::cout << "Dummy Backend started on port " << port << std::endl;
    
    // Keep alive
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}
