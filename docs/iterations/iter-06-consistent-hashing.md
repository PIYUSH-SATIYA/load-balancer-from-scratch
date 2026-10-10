# Iteration 6: Consistent Hashing

## Objective
Implement an algorithm that consistently maps the same client IP to the same backend server. This is critical for distributed systems where backends maintain local state (like sessions or in-memory caches).

## Key Changes & Why They Matter

### 1. The Hash Ring (`std::map`)
In a naive hash approach, you might do `hash(client_ip) % backends.size()`. The problem? If one backend crashes (size changes from 3 to 2), *every single mapping* breaks because the modulo changes. You lose 100% of your cached data.

Consistent Hashing solves this by mapping both the servers and the clients onto a circle (the Hash Ring). 
We used `std::map<size_t, size_t> hash_ring`. A `std::map` is backed by a Red-Black Tree, which automatically keeps its keys sorted. The keys are the hash values, and the values are the backend index.

### 2. Virtual Nodes (Replicas)
If we only place 3 servers on a 64-bit hash ring, the distance between them is highly unpredictable. One server might accidentally own 80% of the ring!
To fix this, we create "Virtual Nodes." For each backend, we hash 100 virtual variations of its name (e.g. `backend1:9001#0` through `backend1:9001#99`). This scatters 300 points across the ring, ensuring perfectly even traffic distribution.

### 3. `lower_bound()` for Routing
When a client connects, we hash its IP: `size_t client_hash = hasher(client_ip);`.
To find which server owns this hash, we just need to move clockwise around the ring until we hit a server node. 
C++ makes this incredibly efficient: `hash_ring.lower_bound(client_hash)` does a binary tree search (O(log N)) to find the first node whose hash is $\ge$ the client's hash! If we reach the end of the map, we simply wrap around to `hash_ring.begin()`.

## How to Verify This Iteration

Run the proxy with `-a consistent_hashing`.

Normally, we'd test this from multiple different machines (different IPs). Because we are testing locally via `nc 127.0.0.1 8080`, your client IP is *always* `127.0.0.1`!
This means that if you open 5 connections, **all 5 connections should route to the exact same backend**!

Try it out, check `SIGUSR1`, and verify that one backend has absorbed all the requests because they all originated from the same IP.
