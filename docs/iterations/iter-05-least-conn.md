# Iteration 5: Least Connections

## Objective
Implement a smarter routing algorithm that tracks the active load on each backend and sends traffic to the least busy server.

## Key Changes & Why They Matter

### 1. `std::lock_guard<std::mutex>` (The first real concurrency synchronization)
Our `LeastConnections` algorithm scans the backend list to find the one with the lowest connection count. 
We introduced a `std::mutex` inside the algorithm class and used a `std::lock_guard`.
*Why this matters:* While our counter is `std::atomic` (preventing corrupted reads), scanning an array of changing values means the state could shift out from under us while we iterate. By locking a mutex, we force all incoming connections to line up and make their routing decision based on a perfectly static, consistent snapshot of the server states.

*What is RAII?* We used `std::lock_guard<std::mutex> lock(mtx);`. RAII (Resource Acquisition Is Initialization) is C++'s greatest feature. When `lock` is created, it locks the mutex. When `lock` goes out of scope (e.g., when the function returns), its destructor automatically unlocks the mutex. You will never accidentally leave a mutex locked because you forgot an `unlock()` before a `return`.

### 2. The Shared Pointer Double-Decrement Problem
Here was the tricky part of this iteration: We spawn *two* detached threads for every connection (one for reading, one for writing). We needed a way to decrement the `active_connections` counter exactly *once* when the connection fully closes. If we decremented it at the end of both threads, we'd double-decrement.

*The Solution:* We created a tiny struct called `ConnectionTracker`.
```cpp
struct ConnectionTracker {
    Backend* backend;
    ~ConnectionTracker() { backend->active_connections.fetch_sub(1); }
};
```
We wrap this in a `std::shared_ptr` and pass it by value into both detached threads. 
A `shared_ptr` maintains a reference count. When the first thread finishes, the count drops from 2 to 1. When the second thread finishes, the count drops from 1 to 0. At exactly 0, the `shared_ptr` deletes the `ConnectionTracker`, triggering its destructor and decrementing the connection count. 
This is an incredibly elegant, race-condition-free way to manage shared lifecycle state across multiple detached threads.

## How to Verify This Iteration

Start the load balancer using `-a least_connections`. Keep multiple `nc` sessions open simultaneously without pressing `Ctrl+C`. Use `SIGUSR1` to check the stats, and you'll see the active connections are perfectly balanced across your backends!
