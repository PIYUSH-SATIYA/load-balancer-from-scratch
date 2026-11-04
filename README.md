# C++ TCP Proxy: A Systems Learning Project

A small Layer 4 TCP proxy written in modern C++17. This repository is a learning project for practicing sockets, connection lifecycles, routing, health checks, and the difference between thread-per-connection and `epoll` event loops. It does not aim to be a production proxy or to reproduce NGINX/HAProxy features.

## Features Built
1. **Multi-Architecture Support:**
   - **Thread-Per-Connection (`ProxyCore`)**: Spawns two threads per connection to handle bidirectional traffic. This makes the concurrency model easy to inspect and demonstrates the memory and scheduling cost of many threads.
   - **Asynchronous Event Loop (`EpollCore`)**: A single-threaded `epoll`-based event loop that demonstrates event-driven I/O without one proxy thread per connection. Its measured capacity depends on the host and is recorded in the benchmark artifacts.
2. **Routing Algorithms**:
   - `round_robin`: Distributes connections evenly across all available backends.
   - `least_connections`: Tracks active connection counts and routes to the least busy backend.
   - `consistent_hashing`: Hashes the client IP to ensure the same client always hits the same backend (useful for session affinity).
3. **Active Health Checking**: A background thread periodically probes backends, removing them from the routing pool when marked down and re-adding them after recovery.
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

## Getting Started (Fresh Clone Setup)

When freshly cloning the repository, the backends require the `/app/build/dummy_backend` binary. Because `/app/build` is stored inside a Docker volume (`build_cache`), you must build the images and compile the binaries first before starting the services.

### 1. Initial Build & Startup
```bash
# 1. Build the shared development image once (Ubuntu 24.04 with cmake, g++, wrk)
docker compose build loadbalancer

# 2. Compile the binaries as root into the named build volume
docker compose run --rm --user root loadbalancer bash -c "cd /app && cmake -B build -S . && cmake --build build"

# 3. Spin up all containers in the background
docker compose up -d --no-build

# 4. Verify all 5 containers are up
docker compose ps
```
Services started:
- `loadbalancer`: Dev container (listening on port 8080)
- `backend1`, `backend2`, `backend3`: Dummy HTTP backends (ports 9001, 9002, 9003)
- `nginx-comparison`: Nginx baseline (port 8081)

---

## Building the Binaries & Reflecting Code Changes

The project uses volume mounts:
- Host files are live-mounted into the container at `/app` (`.:/app`). Any edits to `.cpp` or `.h` on your host are immediately reflected inside the container.
- Build artifacts reside in `/app/build` (`build_cache` named volume).

### How to Recompile After Code Edits:
From your host terminal, execute:
```bash
docker compose exec loadbalancer cmake --build /app/build
# OR using the Makefile shortcut:
make build
```

### Restarting Services with New Binaries:
- **For `loadbalancer`**: Kill the existing process (`pkill -9 loadbalancer` or Ctrl+C if interactive) and re-run `./build/loadbalancer`.
- **For `dummy_backend`**: If you changed `backends/dummy_backend.cpp`, restart the backend containers:
  ```bash
  docker compose restart backend1 backend2 backend3
  ```

---

## Running & Manually Verifying Traffic Allocation

### 1. Start the Load Balancer
Attach an interactive bash session to the load balancer container:
```bash
docker compose exec -it loadbalancer bash
```

Inside the container, run the load balancer:
```bash
# Epoll Event Loop + Round Robin (Recommended)
./build/loadbalancer -a round_robin -e

# Epoll with 4 worker threads (multi-core SO_REUSEPORT)
./build/loadbalancer -a round_robin -e -w 4

# Thread-per-Connection + Least Connections
./build/loadbalancer -a least_connections

# Consistent Hashing (session affinity by client IP)
./build/loadbalancer -a consistent_hashing
```
*(Note: If no `-b` flags are passed, the load balancer automatically defaults to `backend1:9001`, `backend2:9002`, and `backend3:9003`).*

### 2. Observe Real-Time Server Allocation
In a second terminal on your host, follow the backend logs:
```bash
docker compose logs -f backend1 backend2 backend3
```

In a third terminal on your host, send requests through port `8080`:
```bash
curl http://localhost:8080/
curl http://localhost:8080/
curl http://localhost:8080/
```
In the backend logs, you will observe each backend accepting connections in turn:
```text
backend1-1  | [Backend] Accepted connection from 172.22.0.6:50564
backend2-1  | [Backend] Accepted connection from 172.22.0.6:55024
backend3-1  | [Backend] Accepted connection from 172.22.0.6:36746
```

### 3. Dump Live Internal Statistics (`SIGUSR1`)
The load balancer includes an on-demand stats signal handler. Send `SIGUSR1`:
```bash
docker compose exec loadbalancer pkill -SIGUSR1 loadbalancer
```
The load balancer process will immediately print the stats table to stderr:
```text
=== [STATS] Backend Status ===
backend1:9001 | Status: UP | Active Conns: 0 | Backend Selections: 1
backend2:9002 | Status: UP | Active Conns: 0 | Backend Selections: 1
backend3:9003 | Status: UP | Active Conns: 0 | Backend Selections: 1
==============================
```

### 4. Establishing Persistent Connections (`nc`)
Because this is a Layer 4 TCP proxy, connections remain open until either side sends a TCP FIN.

Open an interactive TCP connection using `nc`:
```bash
nc 127.0.0.1 8080
```
Type an HTTP request and hit Enter twice:
```http
GET / HTTP/1.1
Host: localhost

```
You will receive `Hello, World!` with `Connection: keep-alive`. **`nc` will remain open.**

While `nc` is open, run `pkill -SIGUSR1 loadbalancer` in another terminal. You will see:
```text
backend1:9001 | Status: UP | Active Conns: 1 | Backend Selections: 1
```
When you exit `nc` (Ctrl+C), `Active Conns` drops back to `0`.

---

## Important Gotchas & Diagnostics

### 1. Frequent Logs in `backend1`, `backend2`, `backend3`
If you see recurring `Accepted connection` followed immediately by `Client disconnected cleanly`, this is the **Active Health Checker thread**. Every 5 seconds (default), it probes backends by establishing a TCP handshake and immediately closing the socket. To adjust probe frequency, use `-h <seconds>` (e.g., `-h 60`).

### 2. Linux `SO_REUSEPORT` Multi-Process Gotcha
If you send 8 curl requests but only 4 appear in your terminal stats:
Both `ProxyCore` and `EpollCore` set `SO_REUSEPORT` on port 8080. If an older load balancer process was left running in the background, the Linux kernel will distribute incoming connections between both processes.
Always ensure older instances are terminated before launching an interactive session:
```bash
docker compose exec loadbalancer pkill -9 loadbalancer
```

---

## Comprehensive Benchmarking Suite

The project has five local behavior and benchmark scripts in `bench/`:

| Benchmark Suite | Script | Description |
| :--- | :--- | :--- |
| **1. Active Throughput & Latency** | `bench/run_bench.sh` | Uses `wrk` across concurrency levels (50 to 800) to measure RPS, P50/P90/P99 latencies, host busy time on the pinned CPU set (not process CPU), and context switches across proxy modes, a direct-backend control, and Nginx. Replaces `results/summary.csv` for each run and preserves raw output under `results/raw/`. |
| **2. Idle Connection Sweep** | `bench/run_idle_test.sh` + `bench/idle_client.py` | Attempts 10,000 to 30,000 persistent idle connections. Runs the client outside the proxy container, rotates source IPs between cells to avoid `TIME_WAIT` port reuse, and samples the exact proxy PID for RSS, PSS, virtual size, threads, FDs, and open-file limits. Records partial and failed runs with client errors. |
| **3. Backend Failure & Recovery** | `bench/test_failover.py` | Runs repeated 20-probe/s trials while stopping and restarting a backend. Records every probe for active-only and passive retry modes; zero failures means no recovery interval was measured. |
| **4. TCP Half-Close Check** | `bench/test_half_close.py` | Sends a 1 MiB request, half-closes the client write side, and verifies the complete large backend response through both proxy modes. |
| **5. Routing Algorithm Checks** | `bench/test_algorithms.py` | Checks round-robin distribution, least-connections behavior with held sessions, and consistent-hash affinity across 30 client IPs. Writes individual trial data to JSON. |

### Running the Benchmarks:

```bash
# 1. Throughput Benchmark (smoke test with 50 connections):
docker compose exec loadbalancer bash -c \
  "WARMUP_SEC=1 DURATION_SEC=2 N_RUNS=1 CONNS_OVERRIDE='50' /app/bench/run_bench.sh Epoll_Optimized Nginx_Stream"

# 2. Idle connection sweep (5s hold):
HOLD_SEC=5 bash bench/run_idle_test.sh

# 3. Failover & Recovery trials (5 runs per mode):
python3 bench/test_failover.py 5 --port 18081

# 4. TCP half-close and payload preservation in both proxy modes:
python3 bench/test_half_close.py

# 5. Routing Algorithm Suite (starts local test backends):
python3 bench/test_algorithms.py --runs 5 --port 18080

# 6. Regenerate the report from the current CSV and JSON artifacts:
python3 bench/generate_report.py
```

This is a systems-learning project, not a product target or an NGINX/HAProxy replacement. Measurements are local experiments for comparing implementation choices; they do not establish production performance. Detailed methodology and current results are in [`docs/benchmark_report.md`](docs/benchmark_report.md).

---

## Managing Container Lifecycle

To safely stop all containers without removing them or losing state/cache:
```bash
docker compose stop
```

To verify they are stopped:
```bash
docker compose ps -a
```

To resume the environment later:
```bash
docker compose start
```
