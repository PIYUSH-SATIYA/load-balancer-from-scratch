# Iteration 4: Config & Logging

## Objective
Make the load balancer configurable at runtime (without recompiling) and implement a mechanism to peek into its internal state while it's running.

## Key Changes & Why They Matter

### 1. `getopt` for Command Line Parsing
We implemented `ConfigParser` using the standard POSIX `getopt` function.
*Why this matters:* We no longer need to hardcode the port or backends. We can spawn the load balancer pointing to different backend clusters entirely via the command line:
`./loadbalancer -p 8080 -a round_robin -b backend1:9001 -b backend2:9002`

### 2. POSIX Signals for Observability (`SIGUSR1`)
We block `SIGUSR1` and receive it in a dedicated `sigwait` thread.
*Why this matters:* A normal thread can safely print a snapshot while the proxy continues to serve connections. Standard C++ stream operations are not safe inside an asynchronous signal handler.

### 3. Tracking Backend Selections
The algorithms increment an atomic per-backend counter when they select a backend for a new TCP connection. This counts selections, including selections whose later connection attempt may fail. It is not an HTTP request count: the Layer 4 proxy forwards payload bytes without parsing HTTP.

## How to Verify This Iteration

Run the proxy, open several TCP connections, and send `SIGUSR1` to print its statistics. The output labels this counter as `Backend Selections`.
