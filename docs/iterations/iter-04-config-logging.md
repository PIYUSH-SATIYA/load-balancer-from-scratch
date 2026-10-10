# Iteration 4: Config & Logging

## Objective
Make the load balancer configurable at runtime (without recompiling) and implement a mechanism to peek into its internal state while it's running.

## Key Changes & Why They Matter

### 1. `getopt` for Command Line Parsing
We implemented `ConfigParser` using the standard POSIX `getopt` function.
*Why this matters:* We no longer need to hardcode the port or backends. We can spawn the load balancer pointing to different backend clusters entirely via the command line:
`./loadbalancer -p 8080 -a round_robin -b backend1:9001 -b backend2:9002`

### 2. POSIX Signals for Observability (`SIGUSR1`)
We registered a signal handler for `SIGUSR1`.
```cpp
std::signal(SIGUSR1, handle_sigusr1);
```
*Why this matters:* In a production environment, you can't just stop the load balancer to see how many requests it has processed. By intercepting a Unix signal (like `SIGUSR1`, which is specifically reserved for user-defined purposes), we can tell the process to print its internal state to the console without interrupting traffic.

### 3. Tracking Total Requests
We added an `std::atomic<int> total_requests` to the `Backend` struct, and the Round Robin algorithm increments it every time a backend is selected.
*Why this matters:* We can now objectively verify that our load balancing algorithm is distributing traffic fairly. When we query the stats, we should see `total_requests` evenly split across all backends.

## How to Verify This Iteration

Run the proxy, send it a few requests, and then send it the `SIGUSR1` signal to print its statistics!
