cmake_minimum_required(VERSION 3.16)
project(loadbalancer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Strict warnings to catch bugs early
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wpedantic")

find_package(Threads REQUIRED)

add_executable(loadbalancer
    src/main.cpp
    src/proxy_core/proxy_core.cpp
    src/config/config.cpp
    src/lb_algorithms/round_robin.cpp
    src/lb_algorithms/least_connections.cpp
    src/health_checker/health_checker.cpp
)
target_include_directories(loadbalancer PRIVATE src)
target_link_libraries(loadbalancer PRIVATE Threads::Threads)

add_executable(dummy_backend backends/dummy_backend.cpp)
target_include_directories(dummy_backend PRIVATE src)
target_link_libraries(dummy_backend PRIVATE Threads::Threads)

enable_testing()

