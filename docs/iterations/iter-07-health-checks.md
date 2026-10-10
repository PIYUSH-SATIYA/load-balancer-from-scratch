# Iteration 7: Active Health Checks

## Objective
A load balancer isn't very useful if it blindly sends traffic to a server that has crashed. We need a background mechanism that periodically checks the health of every backend and removes them from the rotation if they fail.

## Key Changes & Why They Matter

### 1. The Background Thread
We created the `HealthChecker` class, which spawns its own `std::thread` running a `while` loop that sleeps for `config.health_check_interval` seconds. 
*Why this matters:* We cannot check backend health on the main proxy thread, because the main thread is intentionally blocked waiting for incoming client connections (`accept()`). By isolating health checks to a separate thread, the proxy can route traffic at maximum speed without stuttering every 5 seconds.

### 2. Thread-Safe Enums (`std::atomic<BackendStatus>`)
The health checker thread writes to `backend.status`, while the proxy core thread reads from `backend.status` simultaneously. 
*Why this matters:* In C++, reading and writing the same non-atomic variable from two different threads at the exact same time is a "Data Race", which is undefined behavior and can crash the program. We changed `BackendStatus` to `std::atomic<BackendStatus>`. C++11 and later allow you to wrap enums in `std::atomic`, ensuring perfectly safe cross-thread communication!

### 3. Non-Blocking Sockets (`O_NONBLOCK`)
When `HealthChecker` tries to connect to a backend to see if it's alive, we set the socket to `O_NONBLOCK` using `fcntl`.
*Why this matters:* If a backend server completely dies or its network cable is cut, a standard blocking `connect()` call can hang for the operating system's default TCP timeout (which is often *over 2 minutes*!). If our health checker hangs for 2 minutes on one backend, it can't check the others. 
By making it non-blocking, `connect()` immediately returns `EINPROGRESS`. We then use the POSIX `select()` system call with a strict 2-second timeout. If it doesn't succeed in 2 seconds, we aggressively mark the server as DOWN.

## How to Verify This Iteration

1. Start the proxy.
2. In a separate terminal, forcefully kill one of the backends using Docker! 
   `docker compose kill backend2`
3. Watch the proxy logs. Within a few seconds, the background thread will notice the failure and print:
   `[WARN] HealthCheck: backend2:9002 is now DOWN`
4. Send a few requests to the proxy:
   `echo 'A' | nc -q 0 127.0.0.1 8080`
5. You will see that the algorithm seamlessly skips `backend2` and continues routing traffic to the healthy nodes!
