# Project Proposal: L4 TCP Load Balancer from Scratch

## 1. Project Overview

This project aims to design and implement a **Layer-4 TCP Load Balancer from scratch in C++**, without relying on any third-party networking libraries. The goal is to build a high-performance network proxy that intelligently distributes incoming TCP traffic across a pool of backend servers.

What makes this project academically meaningful is its **incremental, phase-gated construction methodology**: each module is built, tested, and validated before the next is layered on top. This mirrors real systems engineering practice and ensures that every layer of the final system is well-understood and demonstrably correct.

By the end of the project, we will have a fully functional load balancer supporting **three routing algorithms**, **active health checking with automatic failover**, and a **non-blocking `epoll`-based concurrency model** — all benchmarked against Nginx under identical controlled conditions to produce a genuine empirical comparison.

This document serves as a formal proposal for the professor, outlining the system's requirements, module architecture, phased development plan, and a structured 5-week execution timeline.

---

## 2. Requirements Specification

### 2.1 Functional Requirements

#### FR-01: TCP Traffic Proxying
The load balancer must act as a transparent TCP proxy. It must:
- Accept incoming connections from clients using the standard POSIX socket API (`socket()`, `bind()`, `listen()`, `accept()`).
- Establish an outbound connection to a selected backend server (`connect()`).
- Relay all bytes bidirectionally between the client and backend with zero modification to the payload.
- Correctly distinguish between a **clean connection close** (`recv()` returns 0) and a **connection error** (`recv()` returns -1, `errno` inspected), and handle both gracefully without crashing.

#### FR-02: Load Balancing Algorithms
The system must support three pluggable routing strategies:

- **Round Robin:** Maintains a shared atomic counter. Each new connection is routed to backend `counter % N`, then the counter is incremented. Requires no knowledge of current connection state, making it a stateless and fast algorithm.
- **Least Connections:** Maintains a per-backend active connection counter. On each new connection, the backend with the minimum active count is selected. The counter is incremented on connect and decremented on disconnect. A shared mutex guards these counters to prevent race conditions under concurrent load.
- **Consistent Hashing:** Maps both backend identifiers and incoming client IPs onto a fixed-size numeric ring. A new connection is hashed by client IP and routed to the next backend clockwise on the ring. This ensures **session stickiness** (same client always reaches the same backend) and **minimal reshuffling** when backends are added or removed — only ~1/N of mappings change.

#### FR-03: Active Health Checking
A dedicated background thread must periodically probe each configured backend server (using a lightweight TCP `connect()` attempt). The result updates a shared backend status table (UP/DOWN). The health check interval must be configurable.

#### FR-04: Automatic Failover & Recovery
All routing algorithms must query the health status table before selecting a backend. A backend marked DOWN must be automatically excluded from the routing pool. Once it recovers and responds to health pings, it must be automatically reintroduced — without any operator intervention or system restart.

The end-to-end window between a backend going offline and the load balancer ceasing to route to it must be bounded by the health check interval (configurable, target: ≤5 seconds).

#### FR-05: Configurability
The load balancer must not require recompilation to change operational parameters. The following must be configurable at startup:
- Backend server list (IP:Port pairs).
- Active load balancing algorithm.
- Health check interval.
- Listening port for the proxy.

#### FR-06: Operational Observability
The system must log which backend each connection is routed to, and emit aggregate statistics (requests served per backend, backends currently marked as UP/DOWN) on demand (e.g., via a `SIGUSR1` signal or on clean exit).

---

### 2.2 Non-Functional Requirements

#### NFR-01: Concurrency & Scalability
The system must handle a large number of simultaneous client connections:
- **Phase 1 (Thread-per-Connection):** Each accepted connection spawns a dedicated `std::thread` to handle relay. This is simple and correct, but does not scale past ~thousands of connections due to OS thread limits.
- **Phase 2 (`epoll`-based Event Loop):** A single-threaded non-blocking I/O loop using `epoll()` replaces Phase 1. Only sockets with pending I/O events are serviced, allowing the system to handle tens of thousands of concurrent connections within a single thread.

#### NFR-02: Thread Safety
Any shared mutable state (backend status table, connection counters, round-robin index) must be protected by appropriate synchronization primitives (`std::mutex`, `std::atomic`). The system must be free of data races and undefined behavior under concurrent load.

#### NFR-03: Fault Tolerance
The load balancer must not crash or enter an inconsistent state due to:
- A backend process being killed mid-request.
- A client disconnecting unexpectedly.
- A backend being unreachable at startup.

#### NFR-04: Performance Benchmarking
The system's performance must be empirically measured under reproducible conditions. Metrics to be captured include:
- p50, p95, and p99 latency percentiles.
- Throughput (requests/second).
- Failover recovery time (time from backend death to zero traffic reaching it).

These metrics must be measured for both our load balancer and Nginx configured identically, enabling a data-driven analysis of where and why the performance differs.

#### NFR-05: Portability & Build
The system must build cleanly on a Linux environment (native or WSL2) using a standard `g++`/`clang++` compiler with CMake or Make. It must not rely on any networking libraries beyond the POSIX socket API and the C++ standard library.

---

## 3. System Architecture & Module Breakdown

The system is designed around a clean separation of concerns, with each module owning a distinct responsibility.

```
┌───────────────────────────────────────────────────────────┐
│                        Client                             │
└────────────────────────┬──────────────────────────────────┘
                         │ TCP Connection
┌────────────────────────▼──────────────────────────────────┐
│              PROXY CORE MODULE                            │
│  (accept loop, byte relay, connection lifecycle)          │
│  Phase 1: thread-per-connection (std::thread)             │
│  Phase 2: non-blocking event loop (epoll)                 │
└──────────┬────────────────────────────────────────────────┘
           │ select backend
┌──────────▼──────────────────┐    ┌─────────────────────────┐
│   ALGORITHM MODULE          │    │  HEALTH CHECKER MODULE  │
│  - Round Robin              │◄───│  - Background thread    │
│  - Least Connections        │    │  - TCP probe per backend│
│  - Consistent Hashing       │    │  - Shared status table  │
└─────────────────────────────┘    └─────────────────────────┘
           │ connect to
┌──────────▼──────────────────┐
│      Backend Pool           │
│  Backend-1 | Backend-2 ...  │
└─────────────────────────────┘
```

### Module 1: Proxy Core
- Owns the listening socket lifecycle.
- In the thread-per-connection model: each accepted connection spawns two threads — one relaying `client → backend` and one relaying `backend → client`.
- In the `epoll` model: maintains a state machine per connection (CONNECTING, RELAYING, CLOSING), registered with `epoll`. Only processes file descriptors that signal readiness.
- Handles graceful half-close (one side closes while the other still has data in-flight).

### Module 2: Load Balancing Algorithms
- A pluggable interface (function pointer or base class) so the active algorithm can be swapped at runtime.
- **Round Robin:** O(1), stateless per-connection. Simply increments a counter.
- **Least Connections:** O(N) scan of backend connection counts. Uses a `std::mutex` to guard reads and writes to the count vector. The mutex is the first genuine concurrency bug surface in this project.
- **Consistent Hashing:** Constructs a hash ring of sorted virtual nodes on initialization. Routes in O(log N) via binary search. Adding/removing a backend requires partial ring rebuild — only ~1/N of client-to-backend mappings change.

### Module 3: Health Checker
- Runs on a separate `std::thread`, independent of the proxy core.
- Wakes up every N seconds (configurable), attempts a TCP `connect()` to each backend.
- Updates a `std::vector<BackendStatus>` (UP/DOWN) guarded by a shared mutex.
- The Algorithm Module checks this table before every routing decision.

### Module 4: Concurrency Layer (`epoll`)
- All sockets set to `O_NONBLOCK`.
- A single `epoll` instance watches all client and backend sockets.
- `epoll_wait()` returns only sockets with pending events — `EPOLLIN` (data to read) or `EPOLLOUT` (ready to write buffered data).
- Eliminates per-connection thread overhead; handles thousands of connections in a single OS thread.

### Module 5: Benchmark & Evaluation Harness
- Uses `wrk` as the load generation tool.
- Nginx configured with identical backend pool and round-robin algorithm as the control variable.
- Both systems tested with identical concurrency levels, request counts, and warm-up periods.
- `ulimit -n` raised before all tests to prevent artificial file-descriptor caps from skewing results.
- Separate failover test: a backend is killed mid-benchmark run; recovery time is measured for both systems.

---

## 4. Execution Timeline (5 Weeks)

The project is structured as a sequence of incremental milestones. **Each milestone must produce a runnable, demonstrable system** before the next phase begins. This prevents bugs from two different phases from conflating and confusing debugging.

---

### Week 1: Environment Setup & Basic TCP Proxy

**Goal:** Get a working, single-backend TCP proxy on localhost.

| Day | Tasks |
|-----|-------|
| 1–2 | Set up the Linux dev environment, CMake build system, and confirm compiler toolchain. Study the POSIX socket API: `socket()`, `bind()`, `listen()`, `accept()`, `connect()`. Understand each syscall at the kernel level — not just that the code compiles. |
| 3–4 | Write two minimal programs from scratch (not copy-pasted): a TCP echo server and a TCP client. Exchange one message, verify, and articulate what each syscall does. |
| 5–7 | Build the **Dumb Proxy**: accepts one client connection, connects to one hardcoded backend, and relays bytes in both directions until either side closes. Implement connection-close detection (`recv()` returns 0 vs. -1) and document this logic as an explicit comment — it carries forward into every future phase. |

**Milestone:** `curl` or `nc` through the proxy to a backend produces the correct response. Kill the backend mid-request; the proxy does not crash.

**Why this week matters:** Every bug in later phases (connection resets, half-closed sockets, EAGAIN errors) traces back to not having a solid mental model of these four syscalls at the kernel level. Rushing this step is the primary cause of confused debugging in all future phases.

---

### Week 2: Multi-Backend Routing & Advanced Algorithms

**Goal:** A configurable load balancer supporting all three routing strategies.

| Day | Tasks |
|-----|-------|
| 1–2 | Extend the backend list from one hardcoded address to a configurable `std::vector`. Implement **Round Robin** counter (`next = counter++ % backends.size()`). Move connection handling to a `std::thread` per accepted client. Print which backend each connection goes to. |
| 3–4 | Add command-line and/or config-file parsing (backend list, algorithm choice, health check interval). Add basic per-backend request counters; print them on exit or on `SIGUSR1`. |
| 5–6 | Implement **Least Connections**: per-backend counter incremented on connect, decremented on disconnect, guarded by `std::mutex`. Route to the minimum-count backend. This step deliberately introduces the first real concurrency bug surface — a data race here is expected and is the learning objective. Debug and fix it. |
| 7 | Implement **Consistent Hashing**: hash backend IDs and client IPs onto a numeric ring. Route to the next backend clockwise. Verify same client IP always lands on the same backend; verify adding/removing a backend only reshuffles ~1/N mappings. |

**Milestone:** Repeated concurrent requests visibly rotate per the active algorithm. All three algorithms can be selected without recompiling. Concurrent load does not produce crashes or garbled routing.

**Why Round Robin before Least Connections:** Round Robin requires no shared mutable state between threads. Least Connections does (a shared counter). Getting the threading model working and stable on the simple algorithm first avoids introducing a real concurrency bug surface before the model is even proven correct.

---

### Week 3: Health Checking & Automatic Failover

**Goal:** A resilient load balancer that survives backend outages without operator intervention.

| Day | Tasks |
|-----|-------|
| 1–2 | Implement the Health Checker background thread. It wakes every N seconds and performs a lightweight TCP `connect()` probe to each backend. |
| 3 | Implement the shared backend status table (UP/DOWN flags), guarded by a mutex shared with the Algorithm Module. |
| 4–5 | Integrate health status with all three routing algorithms: skip DOWN backends when selecting a route. |
| 6–7 | Conduct failure tests: kill a backend process mid-benchmark, confirm the load balancer stops routing to it within N seconds; restart the backend, confirm traffic resumes automatically. Harden edge cases: all backends down simultaneously, backend unreachable at startup. |

**Milestone (Key Demo Moment):** Kill a backend mid-test; traffic automatically reroutes within ≤5 seconds. Restart the backend; traffic resumes. This is the project's primary live-failure demonstration and must be rock-solid and repeatable, not just "works once."

---

### Week 4: epoll-Based Concurrency Rewrite

**Goal:** A non-blocking, event-driven proxy capable of handling massive concurrency without spawning OS threads.

| Day | Tasks |
|-----|-------|
| 1 | Study `man 2 epoll` in depth. This is done *after* having a working blocking proxy — the contrast makes the motivation and mechanics immediately clear. |
| 2–4 | Rewrite the Proxy Core: all sockets set to `O_NONBLOCK`. Single `epoll` instance. `epoll_wait()` drives the main loop. Maintain a per-connection state machine (CONNECTING, RELAYING, CLOSING). |
| 5–6 | Validate: the `epoll` version must pass all the same manual tests as the Week 1–2 implementation. |
| 7 | Run a concurrency comparison: `wrk -c 500` and `wrk -c 5000` against both the thread-per-connection and `epoll` versions. Record thread counts and memory usage. **The thread-per-connection version is preserved in a separate Git branch** — it is the production fallback if `epoll` is incomplete. |

> **Risk Note:** This is the highest-risk phase of the project. If `epoll` development over-runs the schedule, the thread-per-connection version with all three algorithms and health checks is a complete, fully gradeable deliverable on its own. An incomplete or buggy `epoll` rewrite is strictly worse. The Git branch strategy is non-negotiable.

**Milestone:** The `epoll` version handles 5,000 concurrent connections in a single thread without spawning additional OS threads, passing the same correctness tests as the blocking version.

---

### Week 5: Benchmarking, Empirical Evaluation & Final Report

**Goal:** Produce a rigorous, data-driven comparison between our load balancer and Nginx.

| Day | Tasks |
|-----|-------|
| 1–2 | Configure Nginx's `upstream` module with an identical backend pool and round-robin algorithm. **Freeze this config before any test run begins** to ensure the control variable is not changed mid-experiment. |
| 3–4 | Write `wrk`-driving scripts: warm-up period discarded; record p50/p95/p99 latency and requests/second at multiple concurrency levels. Raise `ulimit -n` before all tests to remove the artificial file-descriptor cap. |
| 5 | Kill a backend mid-benchmark for both systems; measure and compare failover recovery time. |
| 6–7 | Write the final report. The report must cover methodology, raw data tables, and — critically — an honest analysis of *why* the numbers differ from Nginx, not just that they do. This gap analysis (e.g., Nginx's production-tuned event loop, kernel bypass via `sendfile`, adaptive memory allocation) is the intellectual contribution that separates this from a simple demonstration. |

**Milestone:** A complete dataset with p50/p95/p99 latency, throughput, and failover recovery time for both systems under identical conditions, plus a final report explaining the results.

---

## 5. Current Progress

The project is currently in **Week 1 / early Week 2** of the execution timeline. The following has been completed:

- Development environment, build system, and project directory structure are set up.
- Core C++ module files have been initialized:
  - [`src/proxy_core/proxy_core.cpp`](../src/proxy_core/proxy_core.cpp) — TCP relay skeleton
  - [`src/lb_algorithms/lb_algorithms.cpp`](../src/lb_algorithms/lb_algorithms.cpp) — Algorithm interface stub
  - [`src/health_checker/health_checker.cpp`](../src/health_checker/health_checker.cpp) — Health check loop skeleton
  - [`src/main.cpp`](../src/main.cpp) — Entry point and module wiring
- The incremental build plan has been formally documented in [`docs/load_balancer_stepwise_plan.md`](./load_balancer_stepwise_plan.md).

**Immediate next steps:**
1. Finalize the dumb proxy relay loop and validate it is crash-safe on backend failure.
2. Implement the Round Robin counter and the thread-per-connection model.
3. Move to configurable backend list and config parsing before introducing shared state (Least Connections).

---

## 6. Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `epoll` rewrite over-runs the schedule | Medium | High | Preserve thread-per-connection in a separate Git branch as a ready fallback at all times |
| Subtle race condition in Least Connections | High | Medium | Use `std::mutex` (correct, simple) before considering atomics; add concurrent stress tests early |
| Benchmark results invalidated by `ulimit` | Medium | High | Always raise `ulimit -n` before any load test; document baseline system limits |
| Inconsistent Nginx vs. ours comparison | Medium | High | Freeze Nginx config before all test runs; do not alter it mid-experiment |
| Consistent Hashing churn analysis over-runs | Low | Low | Keep the algorithm; drop deep churn analysis if time is short |

---

## 7. What This Project Demonstrates

Beyond producing a functional artifact, this project demonstrates competency across four areas:

1. **Deep Systems Understanding:** Building from raw POSIX socket syscalls requires understanding what `bind()`, `listen()`, `accept()`, and `connect()` actually do at the kernel level. This surfaces the exact mental models needed to debug real production networking issues.
2. **Concurrency Engineering:** Designing safe shared state (mutex-protected connection counters, health status tables) and then optimizing the concurrency model with `epoll` demonstrates the full spectrum of concurrent systems design — from correctness first, to performance second.
3. **Empirical Thinking:** Benchmarking against an industry-standard system with a rigorous methodology (frozen control variables, warm-up periods, percentile reporting, deliberate failure injection) reflects real performance engineering practice — not just "my code runs fast."
4. **Incremental, Risk-Managed Delivery:** The phase-gated approach with a preserved fallback branch demonstrates professional software delivery discipline: shipping something complete and correct, with a defined contingency, rather than an ambitious but broken system.
