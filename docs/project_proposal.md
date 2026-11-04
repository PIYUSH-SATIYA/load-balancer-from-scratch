# Project Proposal: L4 TCP Load Balancer from Scratch

## 1. Project Overview

This project designs and implements a **Layer-4 TCP proxy in C++**, without relying on third-party networking libraries. Its purpose is to learn socket programming, connection lifecycles, routing strategies, health checks, and two concurrency models by building a small system and measuring its behavior.

The project is built incrementally so each module can be studied and exercised before the next is added. The accompanying checks and benchmark scripts provide evidence about selected behaviors; they do not prove general correctness.

The implementation demonstrates **three routing algorithms**, **active health checking with failover**, and a **non-blocking `epoll`-based concurrency model**. Nginx is included as a separate reference in a small local experiment. This project is not intended as a product or as an NGINX/HAProxy clone.

This document outlines the learning goals, module architecture, requirements, and an example phased development plan.

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
- **Consistent Hashing:** Maps backend identifiers and incoming client IPs onto a numeric ring. A new connection is hashed by client IP and routed to the next backend clockwise on the ring. The behavior check verifies that a client IP keeps its backend across reconnects while the backend set remains unchanged. Minimal remapping after a backend-set change is a known property to explore, not a behavior currently validated by the harness.

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
The system must log which backend each connection is routed to, and emit aggregate statistics (connections routed per backend and backend UP/DOWN status) on demand (e.g., via `SIGUSR1`). A Layer 4 proxy cannot count HTTP requests without parsing the payload.

---

### 2.2 Non-Functional Requirements

#### NFR-01: Concurrency & Scalability
The system must handle a large number of simultaneous client connections:
- **Phase 1 (Thread-per-Connection):** Each accepted connection uses two `std::thread` relay loops, one for each direction. This is straightforward to inspect and demonstrates the scheduling and memory costs of per-connection threads.
- **Phase 2 (`epoll`-based Event Loop):** A single-threaded non-blocking I/O loop using `epoll()` provides a contrasting model. Its capacity is measured on the available host; large connection counts are a learning target, not a guaranteed result.

#### NFR-02: Thread Safety
Any shared mutable state (backend status table, connection counters, round-robin index) must be protected by appropriate synchronization primitives (`std::mutex`, `std::atomic`). The system must be free of data races and undefined behavior under concurrent load.

#### NFR-03: Fault Tolerance
The load balancer must not crash or enter an inconsistent state due to:
- A backend process being killed mid-request.
- A client disconnecting unexpectedly.
- A backend being unreachable at startup.

#### NFR-04: Performance Benchmarking
The system's performance must be empirically measured under reproducible conditions. Metrics to be captured include:
- p50, p90, and p99 latency percentiles emitted by `wrk --latency`.
- Throughput (requests/second).
- Probe failures during an outage and the first successful probe routed to the recovered backend after restart.

The throughput comparison uses the same backend pool and round-robin behavior for the proxy and Nginx reference. Failover is measured separately with the local proxy harness. The results support discussion of this setup; they do not generalize to other workloads or machines.

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
- Nginx configured with the same backend pool and round-robin behavior for the limited throughput comparison.
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
| 7 | Run a concurrency comparison against the thread-per-connection and `epoll` versions. Record achieved connections, thread counts, and memory use. |

> **Learning note:** The blocking and event-driven implementations are retained together so their connection handling and resource costs can be compared directly.

**Milestone:** The `epoll` version handles 5,000 concurrent connections in a single thread without spawning additional OS threads, passing the same correctness tests as the blocking version.

---

### Week 5: Benchmarking, Empirical Evaluation & Final Report

**Goal:** Record a repeatable local comparison between the learning proxy and an Nginx reference, then inspect the limits of the measurements.

| Day | Tasks |
|-----|-------|
| 1–2 | Configure Nginx with the same backend pool and round-robin behavior used by the proxy comparison. |
| 3–4 | Write `wrk`-driving scripts: discard a warm-up period and record the percentiles emitted by `wrk --latency` plus request throughput at multiple concurrency levels. Raise `ulimit -n` before the connection sweeps. |
| 5 | Stop and restart a backend during proxy traffic; record failed probes and the first probe routed back to the recovered backend. |
| 6–7 | Write a report with methodology, raw data tables, and a careful discussion of what this particular comparison can and cannot show. |

**Milestone:** A dataset for the local round-robin throughput comparison and a separate proxy failover experiment, with methodology and limitations in the report.

---

## 5. Status Update

This proposal records the intended learning goals; it is not a claim that every requirement has been exhaustively validated. The repository now contains a blocking TCP proxy, an `epoll` proxy, three routing algorithms, health checks, CLI configuration, and benchmark harnesses. Current benchmark evidence and its limits are reported separately in [`benchmark_report.md`](./benchmark_report.md), generated from the checked-in run artifacts.

The implementation remains a small systems-learning exercise. A passing compile or local benchmark does not establish general correctness, production readiness, or equivalence to a mature proxy.

---

## 6. Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `epoll` rewrite over-runs the schedule | Medium | High | Preserve thread-per-connection in a separate Git branch as a ready fallback at all times |
| Subtle race condition in Least Connections | High | Medium | Use `std::mutex` (correct, simple) before considering atomics; add concurrent stress tests early |
| Benchmark results invalidated by `ulimit` | Medium | High | Always raise `ulimit -n` before any load test; document baseline system limits |
| Inconsistent Nginx vs. ours comparison | Medium | Medium | Record configuration and machine settings with each benchmark run |
| Consistent Hashing churn analysis over-runs | Low | Low | Keep the algorithm; drop deep churn analysis if time is short |

---

## 7. What This Project Demonstrates

Beyond producing a functional artifact, this project demonstrates competency across four areas:

1. **Deep Systems Understanding:** Building from raw POSIX socket syscalls requires understanding what `bind()`, `listen()`, `accept()`, and `connect()` actually do at the kernel level. This surfaces the exact mental models needed to debug real production networking issues.
2. **Concurrency Engineering:** Designing safe shared state (mutex-protected connection counters, health status tables) and then optimizing the concurrency model with `epoll` demonstrates the full spectrum of concurrent systems design — from correctness first, to performance second.
3. **Empirical Thinking:** Recording local benchmark settings, warm-up periods, latency percentiles, and failure-injection outcomes provides practice in reading evidence and its limits.
4. **Incremental Learning:** Building a simpler blocking version before the event loop provides a concrete way to compare two concurrency models and examine their trade-offs.
