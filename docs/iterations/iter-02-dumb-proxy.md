# Iteration 2: Dumb Single-Backend Proxy

## Objective
Build a transparent Layer 4 TCP proxy that accepts a client connection, connects to a single hardcoded backend, and blindly relays bytes back and forth until the connection closes.

## Key Changes & Why They Matter

### 1. The Kernel View of TCP Connections
We are not using high-level networking abstractions. We use the raw POSIX socket API. Here is what we just implemented, explained at the kernel level:

*   **`socket()`**: Asks the kernel to allocate a file descriptor (FD) and a set of internal TCP state tracking buffers.
*   **`bind()`**: Associates that FD with a specific IP interface and Port (e.g., `0.0.0.0:8080`).
*   **`listen()`**: Tells the kernel that this socket is passive — it will receive connections rather than initiate them. The kernel prepares a "backlog" queue for pending handshakes.
*   **`accept()`**: Plucks a fully-completed TCP 3-way handshake from the backlog queue, allocates a *brand new socket FD* specifically for this connection, and hands it to us.
*   **`connect()`**: The opposite of listen/accept. It blocks our thread, sends a `SYN` packet to the backend, waits for `SYN-ACK`, and replies with `ACK`.

### 2. Detecting Connection Closure vs. Error
The most crucial piece of logic in `proxy_core.cpp` is this block:
```cpp
ssize_t bytes_read = recv(from_fd, buffer, sizeof(buffer), 0);
if (bytes_read == 0) {
    // TCP FIN received (graceful close)
} else if (bytes_read < 0) {
    // Error or TCP RST received (hard close)
}
```
*Why this matters:* A TCP connection can be closed nicely (`recv()` returns 0) or it can be brutally killed mid-flight (`recv()` returns -1, `errno == ECONNRESET`). If we get this wrong, our load balancer will crash every time a user closes their browser tab aggressively.

### 3. Suppressing SIGPIPE
We added `signal(SIGPIPE, SIG_IGN);` in the proxy constructor.
*Why this matters:* If our proxy thread tries to `send()` bytes to a client that has already closed their connection, the Linux kernel sends our process a `SIGPIPE` signal. The default action for `SIGPIPE` is to instantly kill the entire process. By ignoring it, `send()` simply returns `-1` and sets `errno = EPIPE`, allowing us to handle it gracefully in the code.

### 4. Two-Thread Relay Model
For every accepted connection, we spawn two detached `std::thread` instances:
*   Thread 1: reads from Client → writes to Backend
*   Thread 2: reads from Backend → writes to Client

*Why this is good right now:* It is incredibly simple to reason about. The code blocks on `recv()` safely because it's running in its own thread.
*Why it won't survive:* Each thread takes memory (usually 2MB or 8MB stack size by default). Handling 10,000 concurrent connections would require 20,000 threads. The OS scheduler would spend all its time context switching. (We will solve this in Iteration 8 with `epoll`).

## How to Verify This Iteration

Run the proxy and send traffic through it! Because we hardcoded `backend1:9001`, traffic hitting port 8080 on the proxy will be forwarded directly to the echo server on backend1.
