cmake_minimum_required(VERSION 3.16)
project(loadbalancer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(loadbalancer
    src/main.cpp
    src/proxy_core/proxy_core.cpp
    src/lb_algorithms/lb_algorithms.cpp
    src/health_checker/health_checker.cpp
)
target_link_libraries(loadbalancer PRIVATE Threads::Threads)

add_executable(dummy_backend backends/dummy_backend.cpp)
target_link_libraries(dummy_backend PRIVATE Threads::Threads)
