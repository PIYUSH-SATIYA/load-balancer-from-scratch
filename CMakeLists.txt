cmake_minimum_required(VERSION 3.16)
project(loadbalancer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug is convenient for this learning project. It adds symbols, but does not
# optimize throughput; benchmark performance claims should use an explicit,
# separately labeled Release build.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Treat warnings as errors so portability/cleanup issues surface during builds.
# Do not add -O here: CMake's Debug/Release configuration owns optimization.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wpedantic")

# Both the proxy and test responder use std::thread, including their health and
# per-connection work, so link the platform's thread flags through CMake.
find_package(Threads REQUIRED)

add_executable(loadbalancer
    src/main.cpp
    src/proxy_core/proxy_core.cpp
    src/epoll_core/epoll_core.cpp
    src/config/config.cpp
    src/lb_algorithms/round_robin.cpp
    src/lb_algorithms/least_connections.cpp
    src/lb_algorithms/consistent_hashing.cpp
    src/health_checker/health_checker.cpp
)
target_include_directories(loadbalancer PRIVATE src)
target_link_libraries(loadbalancer PRIVATE Threads::Threads)

add_executable(dummy_backend backends/dummy_backend.cpp)
target_include_directories(dummy_backend PRIVATE src)
target_link_libraries(dummy_backend PRIVATE Threads::Threads)

# CTest is enabled for future tests; the project currently relies on the
# standalone behavioral/benchmark scripts under bench/ rather than CTest cases.
enable_testing()

