# C++ Load Balancer from Scratch

A high-performance Layer 4 (TCP) Load Balancer built completely from scratch in modern C++ (C++17). 

This project explores the fundamental building blocks of networked applications and distributed systems, simulating the core architectures of production tools like Nginx and HAProxy.

## Features Built
1. **Multi-Architecture Support:**
   - **Thread-Per-Connection (`ProxyCore`)**: Spawns two threads per connection to handle bidirectional traffic. Achieves extreme throughput on multi-core systems but scales poorly with memory.
   - **Asynchronous Event Loop (`EpollCore`)**: A single-threaded `epoll`-based event loop (C10k solution) that scales to tens of thousands of connections using minimal memory, mirroring Nginx and Redis architectures.
2. **Routing Algorithms**:
   - `round_robin`: Distributes connections evenly across all available backends.
   - `least_connections`: Tracks active connection counts and routes to the least busy backend.
   - `consistent_hashing`: Hashes the client IP to ensure the same client always hits the same backend (useful for session affinity).
3. **Active Health Checking**: A background thread continuously probes backends, dynamically removing them from the routing pool if they crash and re-adding them when they recover.
4. **Graceful Teardown**: Proper handling of TCP half-closes and signal-handling (SIGINT, SIGTERM, SIGPIPE).

## Project Iterations
The project was built over 9 clear, documented iterations to incrementally learn network engineering:
- **Iter 01:** Foundation & CMake Toolchain
- **Iter 02:** Dumb Proxy (Single Backend Relay)
- **Iter 03:** Multi-backend & Round Robin
- **Iter 04:** Configuration, CLI Flags & Logging
- **Iter 05:** Least Connections Algorithm
- **Iter 06:** Consistent Hashing Algorithm
- **Iter 07:** Active Health Checks
- **Iter 08:** The Epoll Event Loop
- **Iter 09:** High-Concurrency Benchmarking

Detailed notes for each iteration can be found in `docs/iterations/`.

## Running the Project
The project comes with a Docker Compose environment that spins up the Load Balancer, three Dummy HTTP Backends, and an Nginx container for baseline comparison.

```bash
docker compose build
docker compose up -d
```

### Command Line Flags
By default, the load balancer runs using the Thread-per-connection architecture and Round Robin algorithm on port 8080.
```bash
# Attach to the container
docker compose exec loadbalancer bash

# Run with Epoll Architecture (-e)
./build/loadbalancer -a round_robin -e

# Run with Least Connections
./build/loadbalancer -a least_connections

# Run with Consistent Hashing
./build/loadbalancer -a consistent_hashing
```

## Benchmarking
You can benchmark the load balancer using `wrk` from within the load balancer container:
```bash
wrk -t4 -c400 -d10s http://127.0.0.1:8080/
```
*(Note: Be wary of exceeding the Docker container's file descriptor limit (`ulimit -n = 1024`) during high concurrency testing. Set `-c 400` to stay safely within limits.)*
