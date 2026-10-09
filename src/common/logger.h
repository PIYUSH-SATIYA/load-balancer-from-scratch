#pragma once
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <ctime>

namespace Logger {

inline std::string current_time() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    struct tm* tm_info = std::localtime(&now_time);
    char buffer[20];
    std::strftime(buffer, 20, "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

#define LOG_INFO(msg) \
    std::cerr << "[" << Logger::current_time() << "] [INFO] " << msg << std::endl;

#define LOG_WARN(msg) \
    std::cerr << "[" << Logger::current_time() << "] [WARN] " << msg << std::endl;

#define LOG_ERROR(msg) \
    std::cerr << "[" << Logger::current_time() << "] [ERROR] " << msg << std::endl;

} // namespace Logger
