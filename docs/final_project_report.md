# TCP Load Balancer from Scratch — Final Project Report

**Project branch:** `fix/audit-completion`  
**Code used for the final benchmark run:** `a0e2d9c`  
**Benchmark harness and result artifacts:** `d4cc31f`  
**Benchmark date:** 2026-10-24

Raw results are included in [`summary.csv`](../results/summary.csv), [`idle_sweep_isolated.csv`](../results/idle_sweep_isolated.csv), [`failover_trials.json`](../results/failover_trials.json), [`algorithm_trials.json`](../results/algorithm_trials.json), and [`half_close.json`](../results/half_close.json). Throughput `wrk` output is under [`raw_final_20261024`](../results/raw_final_20261024/).

## 1. Project summary

This project implements a small Layer 4 TCP load balancer in C++17. It accepts a client TCP connection, chooses one configured backend, opens an upstream TCP connection, and relays bytes in both directions. The same routing and backend-health components are used by two proxy architectures:

1. A blocking, thread-per-connection implementation (`ProxyCore`).
2. A nonblocking Linux `epoll` implementation (`EpollCore`), optionally run as several `SO_REUSEPORT` workers.

The three routing choices are round robin, least connections, and consistent hashing by client IP. The process also probes backend TCP availability, marks unavailable backends down, can retry a failed initial backend connection, and exposes backend counters through `SIGUSR1`.

This is a systems-learning project, not a production-ready replacement for NGINX or HAProxy. It does not parse or modify HTTP requests in the proxy path; HTTP is used only by the included test backend and benchmark clients. The results below describe this code, this host, and the run settings recorded in the repository.

## 2. Features and command line

### 2.1 Configuration and defaults

The command-line parser in `src/config/config.cpp` supports:

| Flag | Meaning | Default |
| --- | --- | --- |
| `-p PORT` | Client-facing listen port | `8080` |
| `-a ALGORITHM` | `round_robin`, `least_connections`, or `consistent_hashing` | `round_robin` |
| `-h SECONDS` | Active TCP health-check interval | `5` |
| `-b IP:PORT` | Add a backend; may be repeated | `backend1:9001`, `backend2:9002`, `backend3:9003` |
| `-e` | Use the epoll proxy | Disabled; blocking/threaded mode is default |
| `-w COUNT` | Number of epoll workers | `1` |
| `-F` | Disable passive retries on initial backend connection failures | Passive retries enabled |

An example starting a four-worker epoll proxy with explicit host backends is:

```bash
./build-host/loadbalancer -e -w 4 -p 8080 \
  -b 127.0.0.1:9001 -b 127.0.0.1:9002 -b 127.0.0.1:9003
```

### 2.2 Shared backend state and routing

Each `Backend` stores its address, port, atomic `UP`/`DOWN` status, active-connection count, backend-selection count, and resolved IPv4 socket address. Resolution uses `getaddrinfo`. The selection counter records a routing choice; it does not count HTTP requests and can include a choice whose connection attempt subsequently fails.

All three policies implement the same `LbAlgorithm` interface:

| Algorithm | Implementation | Behavior and limits |
| --- | --- | --- |
| Round robin | Atomic counter; tries backends in successive vector positions and skips `DOWN` backends | Simple connection distribution. With equal backends and no failures it cycles evenly. |
| Least connections | Mutex-protected scan of atomic active-connection counts; skips `DOWN`; ties resolve to the first backend in the configured vector | Selects the backend with the smallest current connection count. Active count increments after the upstream connection succeeds and decrements when the session closes. |
| Consistent hashing | `std::hash` of client IPv4 string; ordered `std::map` ring with 100 virtual nodes per backend (`IP:port#node`) | Uses `lower_bound` and wraps around the ring. If a selected backend is down, scans forward for the next up node. The ring is built at startup; dynamic membership changes are not implemented. `std::hash` is not a cross-toolchain persistent hash format. |

The algorithm tests verify a few explicit properties; they are not a statistical proof of every traffic distribution or a proof of cross-process consistent-hash stability.

### 2.3 Active health checks, passive retry, and statistics

`HealthChecker` runs a background thread. It performs sequential nonblocking TCP connects to each backend and uses `poll` with a two-second wait for an in-progress connect. It updates atomic backend state and logs only state changes. After checking the configured backends, it sleeps for the configured interval. This checks whether a TCP listener accepts connections; it does not issue an HTTP health request or verify application-level correctness.

Passive failover is separate from the active checker. When enabled, an initial backend resolution/connect failure marks that backend down and attempts another backend, up to the backend count. In epoll mode, backend connects are asynchronous and the pending-connect timeout is 250 ms. In the threaded mode, a nonblocking backend connect is waited on with `poll` for up to 250 ms, after which its original blocking mode is restored. `-F` disables these passive initial-connect retries; active health checks still run.

The implementation does not retry or replay a stream after an established upstream connection fails. Retrying arbitrary TCP data after bytes have flowed could duplicate or lose application data, so the current failover scope is initial backend connection establishment.

`SIGUSR1` requests a status dump from a signal-waiting thread. It reports backend status, active connections, and backend selections. The process blocks this signal before starting other threads so the normal thread can safely print the counters.

### 2.4 Blocking thread-per-connection proxy

`ProxyCore` creates a listening IPv4 TCP socket and accepts clients in a blocking loop. For each accepted connection it starts a detached handler thread. The handler selects a backend and connects upstream. After a successful connect, it starts a second detached thread for the reverse relay and uses the handler thread for the other relay direction. Therefore the implementation uses approximately **two relay threads per proxied connection**, in addition to process-level health/statistics threads.

The relay loops use blocking `recv`/`send` and an 8 KiB stack buffer. Each loop handles partial sends. On orderly EOF, it calls `shutdown(..., SHUT_WR)` on the opposite socket and allows the other relay direction to drain, preserving TCP half-close behavior. On fatal I/O errors, it shuts down both socket directions. A shared `ConnectionTracker` owns both file descriptors and decrements the backend's active-connection counter after both relay threads release it.

This model is straightforward to follow, but its per-connection threads increase scheduler work and reserve a thread stack address range for every thread. The latter is visible in `VmSize`; it is not equivalent to resident RAM.

### 2.5 Epoll proxy

`EpollCore` makes the listener, accepted client sockets, and backend sockets nonblocking. A level-triggered epoll loop waits for readiness; `MAX_EVENTS=1024` is the number of events returned in one `epoll_wait` batch, **not** a total connection limit.

Each session tracks the client and backend descriptors, backend state, half-close state, pending-connect timeout, and two optional relay buffers. Each buffer is 16 KiB and is allocated only when that direction has bytes to retain. It is freed after its contents drain. The event mask enables reads while there is buffer space, and enables writes only while a connect is pending or buffered data remains. This provides bounded per-direction user-space buffering and applies backpressure by pausing reads when a buffer fills.

Backend connect completion is handled asynchronously by watching `EPOLLOUT` and reading `SO_ERROR`; pending connects are checked for timeout while the loop runs. The event loop stores sessions by both client and backend descriptor so readiness events can locate the shared session. When either side sends FIN, the proxy drains buffered bytes and half-closes the opposite write direction. It closes the whole session after both directions finish or on a fatal error.

The final audit also fixed a real error path: failed epoll client registration or event-mask modification is now logged and the affected session is closed instead of silently continuing with an unmonitored socket.

### 2.6 Multiple epoll workers

With `-e -w 4`, `main.cpp` starts four worker threads. Each creates its own `EpollCore` and epoll descriptor, and each binds the same listen port with `SO_REUSEPORT`. The current code pins worker `i` to CPU `i` (CPUs 0 through 3 for four workers). A single health-check thread and a single stats thread are shared at process level. This is a multi-loop configuration, not one epoll loop that migrates between cores.

### 2.7 Included dummy backend and Compose setup

`backends/dummy_backend.cpp` is an event-driven TCP HTTP responder used for local experiments. It returns a small `200 OK` response with a `Content-Length` and `X-Backend-Port` header. The latter lets tests identify which backend handled a connection. The benchmark backend is intentionally small; it is not a realistic application workload.

The Compose file defines the load balancer, three dummy backends, and an Nginx comparison service. The Compose load-balancer open-file limit is configurable through `LB_NOFILE_LIMIT` and defaults to 131,072. The final measurements in this report ran in host mode because the Docker daemon was unavailable; host-mode cgroup memory fields are therefore blank.

## 3. Benchmark methodology

### 3.1 Host and build used

The final run used a 12-logical-CPU Linux host (kernel `7.2.9-arch1-1`, glibc 2.44), with 15 GiB total RAM. The host soft/hard `RLIMIT_NOFILE` recorded by the idle sampler was 524,288. Docker was unavailable. The throughput suite pinned proxy CPUs to `0` or `0-3`, load generator CPUs to `4-7`, and dummy backends to `8-11`.

The `build-host` compiler flags were `-g -std=gnu++17 -Wall -Wextra -Werror -Wpedantic` with **no `-O` optimization flag**. The throughput numbers below are from that default/debug-style build. Comparing modes built the same way is useful for this learning exercise, but the absolute RPS values are not a Release-build capacity claim. The throughput sweep also used one sample per cell, so no variability estimate can be computed.

### 3.2 Active HTTP throughput and latency

The load generator was `wrk 4.2.0` (`wrk` source checkout `a211dd5`). It used four worker threads for each measurement, with a one-second warmup and three-second measured period at connection concurrency 50, 200, and 800. There was one repetition for each cell.

The five measured modes were direct backend control, one-worker epoll, four-worker epoll, thread-per-connection, and four-core thread-per-connection. Each request went to the same local small HTTP responder. The final run omitted Nginx because its container could not be started without Docker.

Recorded fields include RPS, P50/P75/P90/P99 latency, request count, socket errors, non-2xx responses, and whether the proxy survived. CPU utilization is host CPU time for the selected CPU set from `/proc/stat`, not CPU time attributed only to the proxy. Context-switch rates are sampled from per-thread `/proc` counters during the measurement and summed across observed threads. The direct-backend control has no proxy thread-switch sample.

### 3.3 Idle connection capacity and memory

`bench/idle_client.py` opens connections in batches of 500. It sends an HTTP keep-alive request and reads the complete response headers and declared body before counting a connection as established. After connection setup it sends 20 active probe requests on a separate keep-alive connection, records their median latency, and holds the idle connections for five seconds.

The shell harness starts the sampler before connection creation. It samples the exact proxy PID every 50 ms and keeps the snapshot with the most open FDs, so the harness does not need to create a new sampler process at peak thread count. Each cell receives a fresh loopback source-IP base to avoid reusing the same local address between runs and to reduce `TIME_WAIT`-related client-port interference.

The report records:

- **RSS:** resident process memory reported by `/proc/<pid>/status`.
- **PSS:** proportional resident memory from `smaps_rollup`.
- **VmSize:** virtual address space, including reserved thread stacks; not RAM in use.
- **Threads, file descriptors, and nofile limits.**
- **Cgroup memory/pids:** collected only for Docker-mode runs. These final host-mode rows do not have cgroup values.
- **Established connections, connection-establishment rate, client errors, and active-probe median.**

Process RSS/PSS does not include kernel TCP socket memory. The host-mode result is therefore not total system memory cost per connection.

### 3.4 Backend failure/recovery

`bench/test_failover.py` runs the epoll proxy against three local backends and sends 20 probes per second for seven seconds per trial. It kills backend 9002 at a randomized offset, holds it down for 2.5 seconds, restarts it, and continues the probe stream. It runs 20 trials in each mode:

- `active_only`: passive retry disabled with `-F`; periodic TCP health checks remain active.
- `passive`: passive retry enabled, as in the default configuration.

The recorded dropped count includes failed post-kill probes through recovery. The last failure before backend restoration and first successful probe routed to backend 9002 after restoration are observations at the request level; they are not direct measurements of an internal timer.

### 3.5 Routing-algorithm behavior

`bench/test_algorithms.py` ran five trials for each policy. Each round-robin trial sent 30 new connections and checked for a 10/10/10 split. Each least-connections trial established and held a baseline, retained two sessions on backend 9001, then opened eight more held sessions and checked that the new placement remained balanced around that load. Each consistent-hash trial used 30 source IPs and checked that each IP mapped to the same backend across two reconnects. This is a functional property test, not a goodness-of-fit or broad distribution study.

### 3.6 Half-close and buffered response

`bench/test_half_close.py` sent a 1 MiB request, shut down only the client write side, and verified the entire backend response after the proxy had to drain buffered data. It checked byte equality and the response hash for both threaded and epoll modes.

## 4. Exact reproduction commands

Run the following from the repository root on Linux. The commands below reproduce the **short final suite settings**, not a long soak. Ensure ports 8080, 9001–9003, 18080, and 18081 are free.

### 4.1 Build the binaries

```bash
cmake -S . -B build-host
cmake --build build-host -j2
```

With the project's current CMake defaults, this produces the debug-style flags noted above. A Release build will produce different performance numbers and should be recorded as a separate run.

### 4.2 Prepare `wrk`

The final run used `wrk 4.2.0` from source checkout `a211dd5`, with the binary at `/tmp/lb-wrk/wrk`. The checkout can be recreated and built with:

```bash
git clone https://github.com/wg/wrk.git /tmp/lb-wrk
git -C /tmp/lb-wrk checkout a211dd5
make -C /tmp/lb-wrk
/tmp/lb-wrk/wrk --version
```

If `wrk` is installed elsewhere, set `WRK_BIN` in the throughput command to that binary. The harness records the version string in `results/benchmark_manifest.json`.

### 4.3 Run throughput cells

The host-mode run starts its three local dummy backends in the same shell as the benchmark, so they remain alive for all cells and are stopped afterward:

```bash
set -euo pipefail
taskset -c 8-11 ./build-host/dummy_backend 9001 >/tmp/lb-bench-backend1.log 2>&1 &
backend_one_pid=$!
taskset -c 8-11 ./build-host/dummy_backend 9002 >/tmp/lb-bench-backend2.log 2>&1 &
backend_two_pid=$!
taskset -c 8-11 ./build-host/dummy_backend 9003 >/tmp/lb-bench-backend3.log 2>&1 &
backend_three_pid=$!
cleanup_backends() {
  kill "$backend_one_pid" "$backend_two_pid" "$backend_three_pid" 2>/dev/null || true
  wait "$backend_one_pid" "$backend_two_pid" "$backend_three_pid" 2>/dev/null || true
}
trap cleanup_backends EXIT
sleep 1

WRK_BIN=/tmp/lb-wrk/wrk \
LB_BIN="$PWD/build-host/loadbalancer" \
LB_BACKENDS='127.0.0.1:9001 127.0.0.1:9002 127.0.0.1:9003' \
RAW_DIR=results/raw_final_20261024 \
CONNS_OVERRIDE='50 200 800' \
N_RUNS=1 WARMUP_SEC=1 DURATION_SEC=3 \
PIN_PROXY_1CORE=0 PIN_PROXY_4CORE=0-3 PIN_LOADGEN=4-7 PIN_BACKENDS=8-11 \
bash bench/run_bench.sh \
  Control Epoll_Optimized Epoll_4Workers Threaded_Proxy Threaded_4Cores
```

Nginx was not included because this host had no Docker daemon or Nginx service. This command writes the 15-cell summary to `results/summary.csv`, raw `wrk` output and context-switch snapshots to `results/raw_final_20261024/`, and settings to `results/benchmark_manifest.json`.

### 4.4 Run the idle connection sweep

```bash
EPOLL_TARGETS_OVERRIDE='10000 20000 30000' \
THREADED_TARGETS_OVERRIDE='5000 10000' \
HOLD_SEC=5 \
bash bench/run_idle_test.sh
```

This writes `results/idle_sweep_isolated.csv` and `results/idle_manifest.json`. The threaded 10k cell is expected to be reported as failed if it again misses the target or active probe; do not convert a failed or partial row to a pass.

### 4.5 Run failover, routing, and half-close checks

```bash
python3 bench/test_failover.py 20 --port 18081 --output results/failover_trials.json
python3 bench/test_algorithms.py --runs 5 --port 18080 --output results/algorithm_trials.json
python3 bench/test_half_close.py
```

The failover script's default settings are 20 probes/s, seven seconds per trial, a 2.5-second outage, and seed `20261023`. The algorithm script starts its own backends. The half-close script starts its own test backend and selects free ports.

### 4.6 Regenerate the reports

```bash
python3 bench/generate_report.py
```

The generator writes `docs/benchmark_report.md` and `results/parsed_report.md`. `results/final_suite_manifest.json` records which suites were rerun and which comparison was unavailable.

## 5. Final benchmark results

### 5.1 Throughput, latency, and host CPU

All 15 cells passed with zero socket errors and zero non-2xx responses. Each cell has one repetition, so the displayed range has one value and is not a variability interval. RPS and latency below are the observed values from this short run.

| Mode | Connections | RPS | P50 ms | P90 ms | P99 ms | Pinned CPU set busy % | Voluntary CSW/s | Nonvoluntary CSW/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Control | 50 | 144,596 | 0.311 | 0.367 | 2.710 | 63.2 | — | — |
| Epoll_Optimized | 50 | 102,771 | 0.375 | 1.270 | 3.380 | 94.6 | 54 | 128 |
| Epoll_4Workers | 50 | 292,052 | 0.120 | 1.830 | 4.220 | 92.2 | 23,461 | 149 |
| Threaded_Proxy | 50 | 72,157 | 0.525 | 2.340 | 5.080 | 86.6 | 140,851 | 508 |
| Threaded_4Cores | 50 | 234,714 | 0.151 | 1.850 | 4.370 | 89.7 | 458,047 | 20,263 |
| Control | 200 | 150,009 | 1.280 | 1.480 | 3.490 | 62.2 | — | — |
| Epoll_Optimized | 200 | 98,499 | 1.830 | 3.370 | 5.540 | 97.1 | 36 | 148 |
| Epoll_4Workers | 200 | 301,864 | 0.518 | 2.610 | 5.610 | 95.2 | 12,609 | 169 |
| Threaded_Proxy | 200 | 59,958 | 2.870 | 6.180 | 10.270 | 87.4 | 116,602 | 570 |
| Threaded_4Cores | 200 | 216,858 | 0.685 | 3.520 | 11.690 | 91.4 | 421,718 | 6,280 |
| Control | 800 | 130,427 | 5.920 | 6.770 | 8.170 | 62.3 | — | — |
| Epoll_Optimized | 800 | 82,423 | 9.420 | 11.090 | 13.280 | 98.4 | 3 | 145 |
| Epoll_4Workers | 800 | 253,129 | 2.600 | 5.320 | 8.810 | 95.5 | 3,808 | 199 |
| Threaded_Proxy | 800 | 56,715 | 13.890 | 20.670 | 29.390 | 96.2 | 107,708 | 725 |
| Threaded_4Cores | 800 | 165,938 | 3.820 | 11.300 | 22.030 | 91.7 | 318,147 | 4,119 |

### 5.2 Idle connection capacity and process memory

`RSS/PSS` below show the loaded process value and its growth from the baseline. VmSize is shown in GB so the thread-stack reservation is visible. The nofile limit was 524,288 soft/hard for this host run.

| Mode | Target | Established | Conn/s | Threads | RSS MB (Δ) | PSS MB (Δ) | VmSize GB | FDs | PSS growth / connection | Active probe median | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| Epoll_Optimized | 10,000 | 10,000 | 7,297 | 3 | 6.77 (2.33) | 2.92 (2.33) | 0.09 | 20,005 | 0.24 KB | 0.03 ms | PASS |
| Epoll_Optimized | 20,000 | 20,000 | 4,042 | 3 | 9.04 (4.60) | 5.19 (4.60) | 0.09 | 40,005 | 0.24 KB | 0.04 ms | PASS |
| Epoll_Optimized | 30,000 | 30,000 | 3,483 | 3 | 11.54 (7.10) | 7.69 (7.10) | 0.09 | 60,005 | 0.24 KB | 0.04 ms | PASS |
| Threaded_Proxy | 5,000 | 5,000 | 6,844 | 10,003 | 125.54 (121.03) | 121.62 (121.03) | 78.89 | 10,004 | 24.79 KB | 0.03 ms | PASS |
| Threaded_Proxy | 10,000 | 9,213 | 6,182 | 18,429 | 231.41 (226.90) | 227.41 (226.82) | 150.90 | 18,430 | 25.21 KB | 0.00 ms | FAILED |

At the 10k target, epoll established all 10,000 connections. Thread-per-connection established 9,213; its active probe returned 0.00 ms, which is the harness's failure sentinel, and 787 responses were incomplete. The row is a failed target, not a successful 10k threaded result.

The 150.90 GB threaded VmSize is **virtual address space**, not 150 GB of physical memory. The increase is about 8 MB per added thread, consistent with the host's 8 MiB thread-stack reservation and the code's two relay threads per proxied connection. The measured threaded process used 227.41 MB PSS at 9,213 established connections. PSS is a resident process metric and excludes kernel socket memory.

At 30k, epoll kept 60,005 descriptors open while remaining at three process threads. The 1024 event-array size in code is a batch size for `epoll_wait`; it did not cap concurrent connections in this experiment.

### 5.3 Failover and recovery

| Mode | Trials | Mean dropped probes ± SD | Range | Median last failed probe before restore | Median first probe to restored backend | Trials with observed re-entry |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Active health checks, passive retry disabled | 20 | 3.65 ± 2.16 | 0–7 | 0.548 s | 0.467 s | 20/20 |
| Active health checks plus passive retry | 20 | 0.00 ± 0.00 | 0–0 | Not observed | 0.709 s | 20/20 |

All passive-mode trials recorded zero dropped probes after the backend kill under this probe schedule. Active-only had no failures in one trial and pre-restore failures in 19. Recovery figures are request-observation times with a 20-probe/s stream, not a general SLA or a direct measurement of health-check detection time.

### 5.4 Routing algorithm checks

| Algorithm | Runs | Observed result |
| --- | ---: | --- |
| Round robin | 5 | All 30-connection trials split 10/10/10 across backends. |
| Least connections | 5 | With two sessions retained on backend 9001, eight new held sessions distributed 3/3/2. |
| Consistent hashing | 5 | Each of 30 client IPs retained its backend across reconnects; observed distribution was 12/9/9. |

### 5.5 TCP half-close check

| Mode | Request accepted | Response drained | Byte/hash check |
| --- | ---: | ---: | :--- |
| Thread-per-connection | 1,048,576 bytes | 1,048,608 bytes | PASS; exact payload/hash match |
| Epoll | 1,048,576 bytes | 1,048,608 bytes | PASS; exact payload/hash match |

## 6. Conclusions

1. **The epoll model delivers the expected idle-connection advantage on this host.** It passed the 30,000-connection target with three threads. The old failures that prompted the audit were not evidence of a 20k/30k epoll capacity limit; the corrected run rotated source addresses and isolated the load client.
2. **Thread-per-connection has much higher per-connection process memory and thread cost.** At 5,000 it used 10,003 threads and grew PSS by 121.03 MB. At the 10,000 target it reached 9,213, used 18,429 threads, and grew PSS by 226.82 MB. Its large 78.89–150.90 GB VmSize values reflect virtual stack reservations; resident PSS was in the hundreds of MB, not GB.
3. **The GB-scale measurement still matters.** Virtual address reservation consumes address space and accompanies a very large thread count, but must not be reported as physical RAM. RSS/PSS are the resident measures captured here; kernel socket memory was not measured.
4. **The short active-throughput run favored epoll in this build, especially with four workers.** At 800 concurrency, single-worker epoll measured 82,423 RPS versus 56,715 for single-core thread-per-connection; four-worker epoll measured 253,129 versus 165,938 for four-core threaded. The run was one three-second sample per cell on a non-optimized build, so this is evidence to investigate, not a production capacity claim.
5. **The behavioral checks passed for the stated cases.** The routing properties, failover probe schedule, and 1 MiB half-close case passed. They do not amount to broad protocol conformance or a production availability guarantee.

## 7. Limitations and work not demonstrated

- The final throughput results are from a debug-style build without `-O`; rerun a separately labeled Release build before making absolute performance claims.
- Throughput used one repetition, one-second warmup, and three seconds of measurement. The idle rows used one five-second hold per target. Neither is a long soak or a statistical capacity study.
- Nginx was omitted because Docker was unavailable. The final throughput comparison includes direct backend and all four proxy modes only.
- Host cgroup memory was unavailable. Neither process RSS nor PSS captures total kernel TCP socket memory.
- The threaded model did not complete its 10k target. The observed failure is clear; this run does not isolate the exact OS resource limit that stopped connection creation.
- The failover test is a local TCP listener kill/restart experiment. It does not test network partitions, half-open sessions, backend overload, multi-host routing, or exactly-once application semantics.
- The proxy has no TLS termination, HTTP parsing/routing, config file/reload, weighted pools, connection limits, admission control, production metrics endpoint, or graceful listener-drain protocol. Health checks are TCP-connect checks. Passive retries cover initial upstream connection failures only.
- Consistent hashing uses the standard library's `std::hash`, whose mapping is not promised stable across implementations or builds. The test verifies reconnect affinity within the run, not persistent affinity across arbitrary toolchains.

These limits are why this report describes a completed educational implementation and an honest benchmark snapshot, rather than certifying production readiness.
