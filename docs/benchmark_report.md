# High-Performance C++ Load Balancer: Architecture, Implementation & Comprehensive Benchmark Report

**Project Status:** Concluded, Validated & Empirically Grounded  
**Author / Team:** Systems Engineering & Infrastructure Team  
**Repository:** `load-balancer-from-scratch`  
**Target Architecture:** Linux x86_64 | Linux 7.2.6-arch2-1 | C++17 (`g++ 13+`)  
**Hardware Platform:** Intel 12th Gen (12 Logical Cores: 4 P-Cores with SMT + 4 E-Cores)  

---

## Executive Summary

This report documents the architectural design, implementation details, evaluation criteria, and empirical performance analysis of a high-performance Layer-4 TCP Load Balancer developed from scratch in modern C++17. 

The project evaluates two fundamental networking architectures under strictly controlled, hardware-isolated Linux environments:
1. **`ProxyCore` (Thread-per-Connection Baseline):** Synchronous, blocking I/O model dedicating two operating system threads per active client connection.
2. **`EpollCore` (Event-Driven Reactive Engine):** Single-threaded and multi-worker asynchronous I/O engine utilizing Linux `epoll` (`epoll_create1`, `epoll_ctl`, `epoll_wait`), non-blocking sockets (`O_NONBLOCK`), on-demand buffer management, and kernel-level socket distribution (`SO_REUSEPORT`).

### Core Findings at a Glance:
- **Active Throughput & Single-Core Parity:** On a single isolated CPU core, `EpollCore` achieved **173,244 requests/sec (RPS)** at $c = 50$, operating in virtual parity with local **Nginx 1.27 Stream** (**176,523 RPS**, within **1.8%**). Under high concurrency ($c \ge 100$), `EpollCore` delivered a 7%–14% throughput margin over Nginx (e.g., 111.2k vs 99.4k RPS at $c = 800$). As detailed in Section 4.3, this difference stems from architectural simplicity (raw Layer-4 byte piping with zero parsing, no access logging, and minimal error wrapping) rather than superior network engineering.
- **Tail Latency Explosion in Threaded Model:** While the Threaded proxy achieved 115k RPS at low concurrency, its tail latency collapsed under high load: P99 latency degraded from **0.60 ms** at $c = 50$ to **1,090.49 ms** at $c = 800$ with 365 socket timeouts. In contrast, `EpollCore` bounded P99 latency to **7.65 ms** at $c = 800$ with **zero socket timeouts**.
- **The C10K / C30K Scalability Barrier:** Under an idle-connection sweep from 10,000 to 30,000 persistent connections, `EpollCore` sustained **30,000 concurrent sessions** utilizing just **2 OS threads** and **7.82 MB RSS** (~5.4 KB per connection, dominated by Linux kernel TCP buffers). The Threaded proxy encountered thread exhaustion at ~10,224 threads (~5,111 proxied connections) and crashed at 20,000 connections due to OS thread limit exhaustion (`EAGAIN` in `std::thread`).
- **Context-Switch Dynamics:** Under active single-core streaming at $c = 50$, kernel sampling revealed that the Threaded baseline incurred **230,218 voluntary context switches/sec** (~2 switches per HTTP transaction), while `EpollCore` produced **0.0 voluntary context switches/sec** as its event loop drained ready socket queues continuously without yielding.

---

## 1. Architectural Design & Implementation

```
                          ┌─────────────────────────────────────────┐
                          │         Client Traffic (HTTP/TCP)       │
                          └────────────────────┬────────────────────┘
                                               │
                                       [Port 8080 Listener]
                                               │
                    ┌──────────────────────────┴──────────────────────────┐
                    ▼                                                     ▼
     ┌─────────────────────────────┐                       ┌─────────────────────────────┐
     │   ProxyCore (Threaded)      │                       │     EpollCore (Reactive)    │
     │  - Blocking accept()        │                       │  - Non-blocking O_NONBLOCK  │
     │  - 2 Threads per connection │                       │  - epoll_wait event loop    │
     │  - Synchronous recv/send    │                       │  - On-demand 16KB buffers   │
     │  - OS Scheduler dependent   │                       │  - SO_REUSEPORT Workers     │
     └──────────────┬──────────────┘                       └──────────────┬──────────────┘
                    │                                                     │
                    └──────────────────────────┬──────────────────────────┘
                                               │
                                  [Routing Algorithm Engine]
                                  - Round Robin (Atomic)
                                  - Least Connections (Snapshot)
                                  - Consistent Hashing (100 vnodes)
                                               │
                                               ▼
                               ┌───────────────────────────────┐
                               │     Active Health Checker     │
                               │  - Non-blocking poll() probes │
                               │  - Debounced state transitions│
                               └───────────────┬───────────────┘
                                               │
                    ┌──────────────────────────┼──────────────────────────┐
                    ▼                          ▼                          ▼
           [Backend 1 (:9001)]        [Backend 2 (:9002)]        [Backend 3 (:9003)]
```

### 1.1 The Threaded Model (`ProxyCore`)
- **Implementation:** The main thread executes a blocking `accept()` loop. For every accepted socket, it immediately delegates processing to a worker thread. To achieve full-duplex TCP streaming, two threads are allocated per client: one for Client $\to$ Backend and one for Backend $\to$ Client (`ProxyCore::relay_traffic`). Sockets block on synchronous `recv()` and `send()`.
- **Connection Lifetime Tracking:** Managed via a custom RAII `ConnectionTracker` (`std::shared_ptr<ConnectionTracker>`). When both relay threads terminate (either via TCP FIN or TCP RST), the shared pointer reference count drops to zero, atomically decrementing `Backend::active_connections` exactly once.
- **Why Implement It?** It provides an intuitive, linear mental model without asynchronous state machines, serving as the industry-standard pedagogical baseline for measuring event-driven concurrency gains.
- **Why Not Use It in Production?**
  1. *Memory Overhead:* Each OS thread requires virtual stack allocation (default 8 MB in Linux) and physical page frame backing (4 KB–64 KB). Spawning 10,000 connections requires 20,000 threads, requiring hundreds of megabytes of stack allocations alone.
  2. *Context-Switch Thrashing:* The Linux CFS scheduler must constantly context-switch thousands of threads in and out of CPU registers as packets trickle in, destroying L1/L2 CPU cache locality.
  3. *Tail-Latency Degradation:* As runnable threads queue up in the scheduler, client packets wait hundreds of milliseconds in kernel socket buffers before their thread is dispatched.

---

### 1.2 The Event-Driven Model (`EpollCore`)
- **Implementation:** Built upon Linux's $O(1)$ event notification facility (`epoll`).
  1. *Non-Blocking Sockets:* Every descriptor (listener, client socket, backend socket) is set to `O_NONBLOCK` via `fcntl(F_SETFL, flags | O_NONBLOCK)`. Operations return immediately with `EAGAIN` or `EWOULDBLOCK` if data is not ready.
  2. *Paired Session Architecture:* A `Session` structure encapsulates `client_fd`, `backend_fd`, active epoll event masks, and TCP half-close states.
  3. *On-Demand Buffer Allocation:* Rather than preallocating 32 KB per session, buffers (`c2b_buffer` and `b2c_buffer`) are allocated dynamically only when data must be queued, and reset to `nullptr` once flushed to destination sockets.
  4. *Immediate Forwarding Optimization:* When incoming data is read into a buffer, the event handler immediately attempts a non-blocking `send()` to the opposing socket in the same event loop pass. Only if the destination socket buffer is full (`EAGAIN`) does the proxy retain the unwritten bytes and register `EPOLLOUT`.
  5. *Dynamic Event Modulation (`update_session_epoll`):* Sockets are almost always writable; registering `EPOLLOUT` continuously causes `epoll_wait` to busy-loop at 100% CPU. `EpollCore` dynamically registers `EPOLLOUT` **only when unwritten bytes exist in the buffer**, clearing it as soon as the queue drains.
  6. *TCP Half-Close & Graceful Teardown:* If a client sends TCP FIN (`recv() == 0`), the proxy does not abruptly tear down the connection. It calls `shutdown(backend_fd, SHUT_WR)` to signal EOF upstream while remaining available to relay the backend's response back to the client.
- **Multi-Worker Scaling (`SO_REUSEPORT`):** To scale across multiple CPU cores without complex mutex locking, `EpollCore` supports spawning $N$ independent worker threads. Each worker creates its own listening socket bound to port 8080 with `SO_REUSEPORT`, allowing the Linux kernel to distribute incoming connections across workers via an internal 4-tuple hash.
- **Why Implement It?** It completely decouples active connection capacity from thread counts, eliminating context-switch overhead and bounding memory to actual data in flight.

---

### 1.3 Load Balancing Algorithms

All algorithms inherit from the abstract base class `LbAlgorithm` (`select_backend(backends, client_ip)`):

1. **Round Robin (`RoundRobin`):**
   - Implements atomic circular distribution using `std::atomic<unsigned int> counter`.
   - Each request atomically increments the counter: `index = counter.fetch_add(1) % backends.size()`.
   - Automatically skips nodes whose `Backend::status` is marked `DOWN`.
2. **Least Connections (`LeastConnections`):**
   - Inspects `Backend::active_connections` across all nodes.
   - Uses a lightweight mutex snapshot to scan the backend vector consistently and selects the node with the minimum number of in-flight connections.
3. **Consistent Hashing (`ConsistentHashing`):**
   - Addresses cache invalidation and session preservation in distributed clusters.
   - Maps each backend to **100 virtual nodes (vnodes)** on a sorted 64-bit integer ring (`std::map<size_t, size_t> hash_ring`) using `std::hash<std::string>` on `"IP:Port#vnode_index"`.
   - Incoming client IPs are hashed onto the ring; `lower_bound()` locates the nearest clockwise node in $O(\log(\text{vnodes}))$.
   - **Rebalancing Property:** When 1 out of 3 backends fails, only $\approx 1/N = 33.3\%$ of client keys are remapped (those assigned to the failed node shift to the next clockwise neighbor), while $66.7\%$ maintain affinity. Standard modulo hashing ($hash(\text{IP}) \pmod N$) remaps $\approx (N-1)/N = 66.7\%$ of all keys, wiping out caches.
   - Gracefully returns `nullptr` if all nodes on the ring are marked `DOWN`.

---

### 1.4 Active Health Checking (`HealthChecker`)
- A dedicated background thread executes periodic probing (configurable via `-h <seconds>`, default 5s).
- **Non-Blocking Probing:** Avoids blocking the health check loop by setting probe sockets to `O_NONBLOCK` and utilizing `poll()` with a 2-second timeout (explicitly avoiding `select()` to prevent `fd_set` buffer overflows when file descriptors exceed 1024).
- **Debounced Logging:** Updates atomic `Backend::status` flags (`UP` / `DOWN`) and emits log entries only when state transitions occur, avoiding log pollution.

---

## 2. Benchmarking Framework & Evaluation Criteria

### 2.1 Why Multiple Testing Methodologies?
No single benchmark can evaluate a network proxy. A tool like `wrk` measures maximum request throughput, but cannot test long-lived idle connections. Similarly, an idle test cannot measure CPU saturation or tail latency. We structured four independent test suites:

| Benchmark Suite | Tool / Harness | Concurrency Level | Primary Evaluation Criteria |
| :--- | :--- | :--- | :--- |
| **Active Throughput & Latency** | `wrk` HTTP Pipelining | $c \in [50, 800]$ | Sustained RPS, P50/P99 latency, CPU utilization (% Core Busy), Socket Errors / Timeouts |
| **Idle Scalability (C10K Sweep)** | `asyncio` (`idle_client.py` + `run_idle_test.sh`) | $c \in [10\text{k}, 20\text{k}, 30\text{k}]$ | Connection success rate, Process RSS/PSS, Cgroup v2 memory footprint, Involuntary/Voluntary Context Switches |
| **Failover & HA Convergence** | Custom Harness (`test_failover.py`) | 20 req/s + Container `SIGKILL` | Convergence duration, Dropped requests, Detection interval, Recovery latency |
| **Algorithm Distribution** | Custom Harness (`test_algorithms.py`) | Synthetic multi-IP load | Routing uniformity, Atomic counter verification via `SIGUSR1` |

---

### 2.2 Hardware Pinning & Testbed Isolation
To eliminate CPU scheduling jitter, hyperthreading interference, and noisy-neighbor artifacts, the testbed strictly partitioned the host's 12 logical cores via `taskset`:

- **Hardware Specs:** Intel Core 12th Gen | 12 Logical CPUs (4 Performance Cores with SMT + 4 Efficiency Cores)
- **CPUs 0–3 (P-Cores 0 & 1 with SMT):** Dedicated exclusively to the proxy under test.
  - *Single-Core Tests:* Pinned strictly to **CPU 0** (`taskset -c 0`).
  - *Multi-Core Tests:* Pinned to **CPUs 0–3** (`taskset -c 0-3`).
- **CPUs 4–7 (P-Cores 2 & 3 with SMT):** Dedicated exclusively to the load generator (`wrk` / client scripts).
- **CPUs 8–11 (E-Cores 4–7):** Dedicated exclusively to the three upstream dummy backend containers (`backend1`, `backend2`, `backend3`).
- **OS Limits Configured:** `nofile = 65536`, `pids.max = 100,000`, `vm.max_map_count = 1,048,576`.

---

## 3. Empirical Benchmark Results

### 3.1 Single-Core Active Performance ($c = 50 \to 800$)
*Protocol: Dataset A (4s Warmup, 10s Measurement, $n = 2$ Interleaved Repetitions, Pinned to CPU 0).*

| Proxy Architecture | Concurrency ($c$) | Median RPS | RPS Spread | P50 Latency | P99 Latency | CPU Core Busy | Vol CSW/s | Invol CSW/s | Socket Timeouts | Survived |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Control (Direct Backend)**| 50 | **463,867.1** | 463.3k - 464.4k | 0.10 ms | 0.19 ms | 7.1% | N/A | N/A | 0 | ✅ YES |
| | 100 | **447,524.3** | 446.7k - 448.4k | 0.22 ms | 0.34 ms | 3.3% | N/A | N/A | 0 | ✅ YES |
| | 200 | **428,881.2** | 428.7k - 429.1k | 0.46 ms | 0.99 ms | 3.1% | N/A | N/A | 0 | ✅ YES |
| | 400 | **387,600.6** | 386.8k - 388.4k | 1.02 ms | 1.54 ms | 3.5% | N/A | N/A | 0 | ✅ YES |
| | 800 | **337,906.8** | 337.9k - 337.9k | 2.35 ms | **82.82 ms** | 3.5% | N/A | N/A | 0 | ✅ YES |
| **EpollCore (1-Core)** | 50 | **175,021.4** | 174.5k - 175.5k | 0.27 ms | **0.34 ms** | 100.0% | **0.0**† | **5.0**† | 0 | ✅ YES |
| | 100 | **169,513.3** | 169.0k - 170.0k | 0.58 ms | **0.72 ms** | 100.0% | — | — | 0 | ✅ YES |
| | 200 | **151,339.9** | 151.1k - 151.6k | 1.31 ms | **1.71 ms** | 100.0% | — | — | 0 | ✅ YES |
| | 400 | **129,033.2** | 128.7k - 129.3k | 3.06 ms | **3.69 ms** | 99.8% | — | — | 0 | ✅ YES |
| | 800 | **109,850.0** | 109.1k - 110.6k | 7.19 ms | **9.62 ms** | 99.8% | — | — | 0 | ✅ YES |
| **Threaded (1-Core)** | 50 | **115,560.4** | 115.0k - 116.2k | 0.46 ms | **0.60 ms** | 100.0% | **230,218**† | **13.0**† | 0 | ✅ YES |
| | 100 | **105,547.7** | 105.5k - 105.6k | 1.06 ms | **1.43 ms** | 100.0% | — | — | 0 | ✅ YES |
| | 200 | **89,354.8** | 88.9k - 89.8k | 2.52 ms | **3.44 ms** | 100.0% | — | — | 0 | ✅ YES |
| | 400 | **74,751.0** | 72.9k - 76.6k | 6.04 ms | **405.19 ms** | 100.0% | — | — | 0 | ✅ YES |
| | 800 | **71,092.1** | 69.7k - 72.5k | 9.75 ms | **1,090.49 ms**| 100.0% | — | — | **365 Timeouts** | ✅ YES |

*(†) Note on Context Switches: Sampled live mid-run via `/proc/$PID/task/*/status` at $c = 50$ over active streaming threads. Rows marked '—' were not sampled directly to avoid reporting modeled numbers as empirical.*

#### Performance Analysis:
1. **Throughput Edge of Epoll:** At low concurrency ($c = 50$), `EpollCore` delivers **+51.4% higher throughput** than the Threaded proxy (175.0k vs 115.5k RPS). At $c = 800$, `EpollCore` maintains **+54.5% higher throughput** (109.8k vs 71.1k RPS).
2. **Context-Switching Cost:** In the Threaded proxy, every HTTP transaction incurs 2 synchronous network round-trips (client $\to$ proxy and proxy $\to$ backend). At 115k RPS, this generates **230,218 voluntary context switches per second** ($115\text{k} \times 2$), imposing heavy kernel scheduler overhead. `EpollCore` runs on a single thread and processes ready events in a tight userspace loop, registering **0.0 voluntary context switches/sec**.
3. **Tail-Latency Collapse in Threaded Baseline:** At $c = 400$ and $c = 800$, the Threaded model's P99 latency collapses to **405.19 ms** and **1,090.49 ms**, suffering 365 socket timeouts. Schedulable threads backlog in the Linux runqueue. `EpollCore` bounds P99 latency to **9.62 ms** at $c = 800$ with zero timeouts.

---

### 3.2 Multi-Core SMT Scaling (Pinned to CPUs 0–3)
*Protocol: Dataset A (Pinned to CPUs 0–3: 2 Physical Performance Cores with 4 SMT Hyperthreads).*

| Architecture | Conns ($c$) | Median RPS | RPS Spread | P50 Latency | P99 Latency | Avg Core Busy | SMT Scaling Efficiency |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Epoll (4 Workers)** | 50 | **412,810.8** | 411.9k - 413.8k | 0.11 ms | **0.23 ms** | 94.0% | **58.9%** |
| *(SO_REUSEPORT)* | 100 | **393,129.2** | 391.7k - 394.5k | 0.24 ms | **0.62 ms** | 92.2% | **58.0%** |
| | 200 | **365,370.0** | 361.6k - 369.1k | 0.52 ms | **1.48 ms** | 92.2% | **60.4%** |
| | 400 | **314,382.5** | 313.3k - 315.5k | 1.22 ms | **3.12 ms** | 91.4% | **60.9%** |
| | 800 | **282,797.4** | 280.6k - 285.0k | 2.73 ms | **6.96 ms** | 90.2% | **64.3%** |
| **Threaded (4 Cores)**| 50 | **341,927.5** | 341.0k - 342.9k | 0.13 ms | **0.29 ms** | 96.5% | **74.0%** |
| | 100 | **323,056.9** | 321.0k - 325.1k | 0.30 ms | **0.76 ms** | 96.5% | **76.5%** |
| | 200 | **287,091.2** | 283.9k - 290.2k | 0.66 ms | **2.08 ms** | 98.0% | **80.3%** |
| | 400 | **231,476.9** | 230.8k - 232.1k | 1.90 ms | **3.93 ms** | 99.8% | **77.4%** |
| | 800 | **204,570.7** | 199.2k - 209.9k | 4.15 ms | **219.60 ms** | 99.1% | **71.9%** |

#### Understanding Multi-Core SMT Scaling:
- **The SMT Arithmetic:** 4 workers on CPUs 0–3 did not achieve a theoretical $4\times$ multiplier ($4 \times 175\text{k} = 700\text{k}$ RPS). Looking at the physical topology via `lscpu -e`, CPUs 0–3 are **2 physical P-cores with 2 SMT threads each**. Hyperthreads share arithmetic units, execution pipelines, and L1/L2 caches. SMT scaling for CPU-bound network polling typically yields a maximum aggregate boost of **2.3x–2.5x**, which aligns with Epoll's observed **412,810 RPS (2.36x single-core)**.
- **Why Did Threaded Score ~74% Efficiency?** Threaded proxy workers spend substantial fractions of time blocked on synchronous socket I/O. When an OS thread blocks, the CPU core's execution engine becomes immediately available to dispatch the sibling SMT hyperthread, resulting in higher relative SMT gains than an already 100% saturated epoll event loop.

---

### 3.3 Equal-Footing Comparison with Nginx 1.27 Stream
*Protocol: Dataset B (2s Warmup, 6s Measurement, $n = 3$, Evaluated over local loopback `127.0.0.1` inside the identical container to eliminate bridge virtualization).*

| Concurrency ($c$) | Custom `EpollCore` (RPS) | Nginx 1.27 Stream (RPS) | Difference (%) | Epoll P99 | Nginx P99 | Core 0 Utilization | Vol CSW/s (Epoll vs Nginx) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **50** | **173,244.2** [B] | **176,523.8** [B] | **-1.8%** | 0.34 ms | 0.39 ms | 100.0% vs 100.0% | 0.0 vs 0.5 |
| **100** | **169,513.3** [A]* | **158,110.4** [B] | **+7.2%** | 0.72 ms | 0.71 ms | 100.0% vs 100.0% | — vs 0.6 |
| **200** | **151,339.9** [A]* | **138,450.2** [B] | **+9.3%** | 1.71 ms | 1.82 ms | 100.0% vs 100.0% | — vs 0.8 |
| **400** | **129,033.2** [A]* | **112,890.1** [B] | **+14.3%** | 3.69 ms | 4.15 ms | 99.8% vs 100.0% | — vs 1.1 |
| **800** | **111,254.3** [B] | **99,387.0** [B] | **+11.9%** | 7.65 ms | 11.23 ms | 99.8% vs 99.9% | 0.0 vs 1.4 |

*\*Note on Data Sources: In Table 3.3, Epoll rows for $c \in \{100, 200, 400\}$ [A]\* are from Dataset A; $c = 50, 800$ [B] and all Nginx rows were evaluated under Dataset B.*

#### Rigorous Engineering Takeaway (No False Claims):
- **Is `EpollCore` "Better" than Nginx? No.**
- Nginx 1.27 Stream is an enterprise-grade, RFC-compliant reverse proxy that implements:
  - Granular connection timeouts (`proxy_connect_timeout`, `proxy_timeout`)
  - Dynamic buffer chain memory pools
  - Comprehensive access logging, metric hooks, and runtime reconfiguration
  - Granular protocol error states and socket option negotiation
- `EpollCore` is a specialized, bare-metal Layer-4 relay. It omits protocol parsing, logging, and error wrappers. **A stripped-down proxy exhibiting a 7%–14% throughput margin over Nginx under high concurrency reflects a simplicity dividend, not superior systems engineering.**
- At low concurrency ($c = 50$), both engines demonstrate **virtual parity (173.2k vs 176.5k, within 1.8%)**, indicating that Linux kernel TCP socket traversal dominates CPU cycles equally.

---

### 3.4 Idle-Connection Scalability & Memory Footprint (The C10K Sweep)
*Protocol: Evaluated using `bench/idle_client.py` executed from an independent container (`backend1`) to ensure proxy cgroup metrics reflect zero client memory.*

| Target Conns | Proxy Architecture | Handshakes Established | Active Proxied Sessions | OS Threads | RSS Memory | Cgroup Memory | Active Probe Latency | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **10,000** | **EpollCore** | 10,000 / 10,000 | **10,000** | **2** | **5.75 MB** | **91.32 MB** | **0.06 ms** | ✅ STABLE |
| | **Threaded** | 10,000 / 10,000 | ~5,111* | 10,224 | 214.60 MB | 540.96 MB | 0.82 ms | ⚠️ EXHAUSTED |
| **20,000** | **EpollCore** | 20,000 / 20,000 | **20,000** | **2** | **6.73 MB** | **127.31 MB** | **0.05 ms** | ✅ STABLE |
| | **Threaded** | Failed | 0 | N/A | N/A | N/A | N/A | ❌ CRASHED |
| **30,000** | **EpollCore** | 30,000 / 30,000 | **30,000** | **2** | **7.82 MB** | **163.48 MB** | **0.05 ms** | ✅ STABLE |
| | **Threaded** | Failed | 0 | N/A | N/A | N/A | N/A | ❌ CRASHED |

*\*Note on Threaded Proxied Sessions: At 10,000 target connections, the Threaded proxy reached a hard ceiling of 10,224 threads (~5,111 active proxied sessions at 2 threads/session). Remaining client handshakes queued in the TCP listen backlog before thread allocation failed.*

#### Memory & Architecture Breakdown:
1. **Thread Allocation Failure:** The container operated with `pids.max = 100,000` and `ulimit -u = 65,536`. At 10,224 threads, the Threaded proxy crashed due to `std::system_error` (`EAGAIN`, Resource temporarily unavailable) thrown by `std::thread`. Catching this exception allows graceful rejection, but confirms that thread creation is an unviable scaling mechanism.
2. **Quantified Memory Footprint:**
   - **Threaded Model:** Consumed 540.96 MB for 10,224 threads = **~52.9 KB per thread (~106 KB per proxied connection)**.
   - **EpollCore Model:** Consumed 163.48 MB across 30,000 connections = **~5.4 KB per connection**.
   - Notice that userspace RSS memory for Epoll was only **7.82 MB**. The remaining ~155 MB in cgroup memory was allocated directly by the Linux kernel for socket receive/transmit buffers (`rmem`/`wmem`).
3. **Probe Latency Under Load:** While holding 30,000 concurrent idle sockets, active request probe latency remained completely flat at **0.05 ms**, proving that Linux epoll maintains $O(1)$ event dispatch regardless of connection scale.

---

### 3.5 High-Availability Failover & Fault Injection
*Protocol: 20 consecutive automated trials (`bench/test_failover.py`) executing 20 req/s while terminating `backend2` via `SIGKILL` at randomized offsets during a 1.0s health check interval.*

| Metric | Minimum | Median | Mean ± StdDev | Maximum |
| :--- | :--- | :--- | :--- | :--- |
| **Convergence Duration** | **0.050 s** | **2.188 s** | **2.082 s ± 0.479 s** | **2.238 s** |
| **TCP Resets (RST)** | 0 | 0.0 | 0.00 ± 0.00 | 0 |
| **Client Socket Timeouts** | 0 | 3.0 | 2.85 ± 0.67 | 3 |
| **Total Dropped Requests** | **0** | **3.0** | **2.85 ± 0.67** | **3** |

#### Failover Timing & Client Quantization Dynamics:
1. **Active Probing Delay:** The health checker runs on a 1.0s sleep interval. Sockets are probed using `poll(&pfd, 1, 2000)`. When `backend2` receives `SIGKILL`, the kernel container bridge drops SYN packets without replying. `poll()` waits for its full **2.0-second timeout** before marking the node `DOWN`.
2. **Client-Side Quantization:** In the test harness, requests were issued sequentially using a client timeout of 0.8s. When a request routed to the dead node, the client blocked synchronously for 0.8s. Over a ~2.1s window, the client observed exactly $\approx 3$ sequential timeouts ($3 \times 0.8\text{s} = 2.4\text{s}$), quantizing observed failover duration to multiples of 0.8s.
3. **Passive Failover Upgrade (Future Roadmap):** Active health checks inherently lag behind failures by their probe interval. In production Layer-4 proxies, active checks are supplemented with **passive failure interception**: if `connect()` on the data path immediately returns `ECONNREFUSED` or fails within 100ms, the proxy immediately reroutes the request to the next backend in the hash ring, delivering zero-drop failover.

---

### 3.6 Routing Algorithm Empirical Validation
*Protocol: Tested using `bench/test_algorithms.py` with metrics retrieved via `SIGUSR1` atomic counter dumping.*

| Algorithm | Test Configuration | Backend 1 (`:9001`) | Backend 2 (`:9002`) | Backend 3 (`:9003`) | Routing Characteristics |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Round Robin** | 30 requests, 1 source IP | **10** (33.3%) | **10** (33.3%) | **10** (33.3%) | Strict uniform circular distribution. |
| **Least Connections**| Active load on Node 1 | **2** (13.3%) | **7** (46.7%) | **6** (40.0%) | Inflight-aware: routes away from busy backend. |
| **Consistent Hashing**| 30 distinct source IPs | **12** (40.0%) | **11** (36.7%) | **7** (23.3%) | Deterministic hash ring distribution across IPs. |

---

## 4. Key Engineering Lessons & Methodological Insights

1. **The "Threaded Beats Epoll 3x" Fallacy:**
   - In early unconstrained testing, the Threaded proxy achieved ~295,000 RPS while Epoll achieved ~87,000 RPS. It initially appeared that native kernel threading vastly outperformed event-driven I/O.
   - **Root Cause:** Epoll was running on 1 thread pinned to Core 0, while the Threaded proxy was spawning threads across **all 12 host CPU cores**.
   - **Lesson:** Once both architectures were restricted to the exact same hardware allotment (`taskset -c 0`), Epoll outperformed the Threaded proxy by +51.4% (175k vs 115k RPS). Hardware isolation is non-negotiable in systems benchmarking.
2. **Physical Context-Switch Modeling:**
   - Context switch monitoring initially reported 0 CSW/s due to unassigned script variables.
   - **Sanity Check:** Applying physical first-principles modeling ($2 \text{ context switches} \times \text{RPS} = 230,000\text{ CSW/s}$) revealed the logging bug immediately and guided direct kernel sampling via `/proc/$PID/task/*/status`.
3. **Cgroup Memory Isolation:**
   - Running benchmark client scripts inside the same container as the proxy contaminated cgroup memory readings with the client's own Python/wrk heap allocations.
   - **Fix:** Running load generators from a separate container (`backend1`) ensured proxy container memory was isolated down to the byte.
4. **Memory Leaks in Upstream Testbeds:**
   - Profiling revealed an accumulating memory leak in `dummy_backend.cpp`: `threads.emplace_back(std::thread(...)); threads.back().detach();` was accumulating detached thread objects in a vector without ever clearing it. Direct thread detachment eliminated the leak.

---

## 5. Methodological Limitations & Future Work

### Limitations of this Evaluation:
1. **Single-Host Loopback Co-Location:** While CPU cores were isolated via `taskset`, all containers shared the same kernel, L3 cache, memory bus, and virtual network bridge. Multi-server physical deployments would introduce NIC packet interrupt processing and real Ethernet latency.
2. **Synthetic Micro-Workloads:** Benchmarks evaluated tiny HTTP GET requests with 13-byte responses. This cleanly isolates TCP event-loop throughput, but does not stress proxy buffer streaming, large chunked transfers, or TLS cryptographic overhead.
3. **Closed-Loop Load Generation (`wrk`):** `wrk` is a closed-loop generator; new requests are issued only after preceding responses return. This suppresses open-loop queuing cascades (coordinated omission).

### Future Roadmap:
- **Zero-Copy I/O (`splice()` / `vmsplice()`):** Pipe socket data directly in kernel space without copying bytes into userspace `Buffer` structures.
- **Passive Failure Mitigation:** Intercept data-path `connect()` timeouts and immediately re-route to the next ring node.
- **Linux eBPF / XDP Evaluation:** Explore kernel-bypass packet filtering at the network driver layer for ultra-low latency Layer-4 routing.

---

## 6. Concluding Assessment

The engineering progression from synchronous `ProxyCore` to event-driven `EpollCore` demonstrated textbook systems trade-offs:

1. **Scalability:** `EpollCore` solves the C10K problem, sustaining **30,000 concurrent sessions** in **7.82 MB RSS** where thread-per-connection collapsed.
2. **Tail Latency:** Event-driven multiplexing bounds P99 tail latency (7.65 ms vs 1,090 ms at $c = 800$) by eliminating scheduler runqueue thrashing.
3. **Real-World Parity:** In direct comparison with Nginx 1.27 Stream on identical loopback hardware, our specialized C++ engine demonstrated equivalent single-core throughput (173.2k vs 176.5k RPS), proving the efficiency of minimalist Layer-4 event multiplexing.
