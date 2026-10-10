# Iteration 3: Multi-Backend & Round Robin

## Objective
Evolve the proxy into a true Load Balancer by maintaining a pool of backend servers and distributing connections evenly across them using the Round Robin algorithm.

## Key Changes & Why They Matter

### 1. The Strategy Pattern (Polymorphism)
We introduced `LbAlgorithm`, a base interface with a pure virtual function `select_backend()`.
*Why this matters:* By injecting an algorithm interface into the `ProxyCore` instead of hardcoding the logic, we decouple *how to relay traffic* from *how to select a destination*. When we build Least Connections in a future iteration, we won't need to change `ProxyCore` at all.

### 2. std::atomic
Our Round Robin algorithm maintains a `counter`. Every time a client connects, it calculates `counter % backends.size()` to pick the backend, then increments the counter.
Because `ProxyCore` runs the accept loop on the main thread, the actual selection happens sequentially. *However*, in a highly concurrent environment (like Iteration 8's event loop) or if we were to parallelize connection acceptance, we need to guarantee that no two connections get the exact same counter value at the exact same time (a race condition). 
*Why we use `std::atomic`:* `std::atomic<unsigned int>::fetch_add(1)` is a hardware-level atomic instruction. It increments the value and returns the old value in one indivisible step. We don't need the overhead of a `std::mutex` just to increment an integer safely.

### 3. Thread-per-Connection Model (Recap)
Our proxy still handles concurrency by spawning two detached `std::thread`s per incoming connection. 
*   It's dead simple to reason about (blocking I/O is intuitive).
*   It handles 10-100 connections perfectly.
*   But it fails spectacularly under thousands of connections because of OS thread limit exhaustion and context switching overhead. This serves as our baseline performance before Iteration 8.

## How to Verify This Iteration

Run the proxy and send multiple requests to it. You should see it round-robin exactly across `backend1`, `backend2`, and `backend3`:
```bash
echo 'A' | nc -q 0 127.0.0.1 8080
echo 'B' | nc -q 0 127.0.0.1 8080
echo 'C' | nc -q 0 127.0.0.1 8080
echo 'D' | nc -q 0 127.0.0.1 8080
```
In the proxy logs, you should see:
*   Routed to `backend1`
*   Routed to `backend2`
*   Routed to `backend3`
*   Routed to `backend1`
