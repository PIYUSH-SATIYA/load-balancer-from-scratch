# Iteration 8: Epoll Event Loop (The C10k Problem)

## Objective
Replace the multi-threaded proxy engine with a high-performance, single-threaded asynchronous Event Loop using Linux's `epoll` API. This is the exact architecture used by Nginx, Redis, and Node.js to handle tens of thousands of concurrent connections (the C10k problem) without crashing the kernel.

## Key Changes & Why They Matter

### 1. `O_NONBLOCK` Everywhere
In our old `ProxyCore`, `recv()` and `send()` would block the thread until data arrived. 
In `EpollCore`, we use `fcntl` to set every single socket to `O_NONBLOCK`. If we call `recv()` and there's no data, it immediately returns `-1` with `errno == EAGAIN`. This ensures our single thread *never* gets stuck waiting for a slow client.

### 2. The `epoll` Event Loop
Instead of spawning a thread per connection, we create a single `epoll` instance. 
1. We tell `epoll` which file descriptors we care about using `epoll_ctl(EPOLL_CTL_ADD)`.
2. The single thread loops infinitely, calling `epoll_wait()`. This function blocks the thread until the Linux kernel wakes it up, providing an array of exactly which sockets have data ready to read or write!

### 3. Buffers and State Machines (`Session`)
In the threaded model, local variables (`char buffer[8192]`) were maintained safely inside each thread. 
In a single-threaded event loop, the thread rapidly jumps between handling 10,000 different clients every millisecond. We had to create a `Session` struct to explicitly store the state (`c2b_buffer` and `b2c_buffer`) for every active connection.

### 4. Toggling `EPOLLOUT`
We always listen for `EPOLLIN` (data available to read). But we only listen for `EPOLLOUT` (socket ready to be written to) if our buffer actually has data in it! If we listened for `EPOLLOUT` when our buffer was empty, `epoll_wait` would immediately wake up repeatedly (spinning the CPU to 100%) because the socket is practically *always* ready to be written to.

## How to Verify This Iteration

We kept both architectures! You can toggle between them using the `-e` flag.

Run the new high-performance core:
```bash
docker compose exec loadbalancer /app/build/loadbalancer -a round_robin -e
```

Open some connections, verify they still route perfectly. 
In the final step of this project, we will benchmark the Threaded Core vs the Epoll Core using a load testing tool to prove that Epoll absolutely destroys the threaded model under heavy concurrency!
