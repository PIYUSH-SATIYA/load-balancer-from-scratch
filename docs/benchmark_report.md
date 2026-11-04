# Benchmark report

**Generated:** 2026-10-24 13:58 UTC<br>
**Scope:** educational comparison of a small TCP proxy’s blocking and `epoll` implementations. These measurements do not claim production readiness or feature parity with NGINX/HAProxy.

## How to read this report

Every table below is generated from the current files in `results/`. The CSV and JSON files are the raw trial records. A row marked `PARTIAL` or `FAILED` is not a successful target run. Missing artifacts mean that suite has not been run; no estimate is substituted.

`wrk` measures HTTP request throughput over the TCP proxy. This is a short loopback microbenchmark using the included small HTTP responder, not a general proxy-performance result or production capacity test. It has no long soak, overload recovery, or process-level CPU/memory profile. `wrk 4.2.0 --latency` emits P50, P75, P90, and P99 in this setup; it does not emit P95 or P99.9. Runs with different environment or concurrency settings should be treated as separate experiments. “Pinned CPU set busy” is host CPU time for the selected CPU IDs, not CPU time attributed to the proxy process.

Final-suite coverage (2026-10-24): idle capacity, failover (20 trials per mode), routing algorithms (5 runs), half-close checks, and active throughput were rerun. Throughput coverage: One 3-second measurement after a 1-second warmup at 50, 200, and 800 connections for direct-backend control, single-worker epoll, four-worker epoll, thread-per-connection, and four-core thread-per-connection. All 15 rows passed. Nginx was omitted because Docker and the Nginx service were unavailable.

Settings: 1 repetitions; 1 s warmup; 3 s measured; concurrency 50, 200, 800; proxy CPUs `0` / `0-3`; load-generator CPUs `4-7`; backend CPUs `8-11`; Nginx CPUs `0`. Platform: `Linux-7.2.9-arch1-1-x86_64-with-glibc2.44`; kernel `7.2.9-arch1-1`; CPUs `12`; wrk `wrk 4.2.0 [epoll] Copyright (C) 2012 Will Glozer`; raw logs `results/raw_final_20261024`.

## Active throughput and latency

`bench/run_bench.sh` records one row per mode, concurrency, and repetition. The table reports medians for RPS, latency, pinned CPU set use, and context switches; ranges show RPS spread. Per-thread context switches are sampled during each measurement and summed for thread IDs observed in that interval. The manifest records where the raw `wrk` output was written.

| Mode | Connections | Samples | Median RPS (range) | Median P50 (ms) | Median P90 (ms) | Median P99 (ms) | Pinned CPU set busy % | Voluntary CSW/s | Nonvoluntary CSW/s | Socket errors | Non-PASS runs |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Control | 50 | 1 | 144,596 (144,596–144,596) | 0.311 | 0.367 | 2.710 | 63.2 | — | — | 0 | 0 |
| Control | 200 | 1 | 150,009 (150,009–150,009) | 1.280 | 1.480 | 3.490 | 62.2 | — | — | 0 | 0 |
| Control | 800 | 1 | 130,427 (130,427–130,427) | 5.920 | 6.770 | 8.170 | 62.3 | — | — | 0 | 0 |
| Epoll_4Workers | 50 | 1 | 292,052 (292,052–292,052) | 0.120 | 1.830 | 4.220 | 92.2 | 23,461.0 | 149.0 | 0 | 0 |
| Epoll_4Workers | 200 | 1 | 301,864 (301,864–301,864) | 0.518 | 2.610 | 5.610 | 95.2 | 12,609.0 | 169.0 | 0 | 0 |
| Epoll_4Workers | 800 | 1 | 253,129 (253,129–253,129) | 2.600 | 5.320 | 8.810 | 95.5 | 3,808.0 | 199.0 | 0 | 0 |
| Epoll_Optimized | 50 | 1 | 102,771 (102,771–102,771) | 0.375 | 1.270 | 3.380 | 94.6 | 54.0 | 128.0 | 0 | 0 |
| Epoll_Optimized | 200 | 1 | 98,499 (98,499–98,499) | 1.830 | 3.370 | 5.540 | 97.1 | 36.0 | 148.0 | 0 | 0 |
| Epoll_Optimized | 800 | 1 | 82,423 (82,423–82,423) | 9.420 | 11.090 | 13.280 | 98.4 | 3.0 | 145.0 | 0 | 0 |
| Threaded_4Cores | 50 | 1 | 234,714 (234,714–234,714) | 0.151 | 1.850 | 4.370 | 89.7 | 458,047.0 | 20,263.0 | 0 | 0 |
| Threaded_4Cores | 200 | 1 | 216,858 (216,858–216,858) | 0.685 | 3.520 | 11.690 | 91.4 | 421,718.0 | 6,280.0 | 0 | 0 |
| Threaded_4Cores | 800 | 1 | 165,938 (165,938–165,938) | 3.820 | 11.300 | 22.030 | 91.7 | 318,147.0 | 4,119.0 | 0 | 0 |
| Threaded_Proxy | 50 | 1 | 72,157 (72,157–72,157) | 0.525 | 2.340 | 5.080 | 86.6 | 140,851.0 | 508.0 | 0 | 0 |
| Threaded_Proxy | 200 | 1 | 59,958 (59,958–59,958) | 2.870 | 6.180 | 10.270 | 87.4 | 116,602.0 | 570.0 | 0 | 0 |
| Threaded_Proxy | 800 | 1 | 56,715 (56,715–56,715) | 13.890 | 20.670 | 29.390 | 96.2 | 107,708.0 | 725.0 | 0 | 0 |

## Idle connection capacity

`bench/run_idle_test.sh` measures the exact proxy PID before and during the hold. The client runs outside the proxy container, each cell uses fresh loopback source addresses to avoid earlier sockets in `TIME_WAIT`, and the harness records process threads, RSS, PSS, virtual size, file descriptors, open-file limits, client failures and proxy-container cgroup deltas. Host-mode cgroup fields are blank because the proxy and client share a host cgroup. `Established` is the actual number opened. `PASS` requires reaching the target and passing the active probe; `PARTIAL` means the target was missed while the probe worked; `FAILED` means the probe or proxy measurement failed. The per-connection PSS delta is process PSS growth divided by the established count, not total kernel memory per socket. A sampler issue is reported separately so resource exhaustion is not hidden as a proxy measurement.

| Mode | Target | Established | Conn/s | Threads base→load | RSS base→load (Δ MB) | PSS base→load (Δ MB) | VmSize base→load (GB) | FDs base→load | PSS Δ/connection (KB) | Cgroup Δ MB | nofile soft/hard | Probe P50 ms | Client failures | Sampler issue | Status |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- | :--- | :--- |
| Epoll_Optimized | 10000 | 10000 | 7297 | 3→3 | 4.44→6.77 (2.33) | 0.59→2.92 (2.33) | 0.09→0.09 | 5→20005 | 0.24 | — | 524288/524288 | 0.03 | {} |  | PASS |
| Epoll_Optimized | 20000 | 20000 | 4042 | 3→3 | 4.44→9.04 (4.60) | 0.59→5.19 (4.60) | 0.09→0.09 | 5→40005 | 0.24 | — | 524288/524288 | 0.04 | {} |  | PASS |
| Epoll_Optimized | 30000 | 30000 | 3483 | 3→3 | 4.44→11.54 (7.10) | 0.59→7.69 (7.10) | 0.09→0.09 | 5→60005 | 0.24 | — | 524288/524288 | 0.04 | {} |  | PASS |
| Threaded_Proxy | 5000 | 5000 | 6844 | 3→10003 | 4.51→125.54 (121.03) | 0.60→121.62 (121.03) | 0.16→78.89 | 4→10004 | 24.79 | — | 524288/524288 | 0.03 | {} |  | PASS |
| Threaded_Proxy | 10000 | 9213 | 6182 | 3→18429 | 4.50→231.41 (226.90) | 0.59→227.41 (226.82) | 0.16→150.90 | 4→18430 | 25.21 | — | 524288/524288 | 0.00 | {"IncompleteReadError:":787} |  | FAILED |

At the 10,000 target, epoll established 10000 connections (RSS 6.77 MB, PSS 2.92 MB, VmSize 0.09 GB); thread-per-connection established 9213 (RSS 231.41 MB, PSS 227.41 MB, VmSize 150.90 GB). That threaded VmSize increase averages about 8.38 MB per added thread, consistent with per-thread stack reservations. VmSize is virtual address space; it is not physical RAM usage. RSS and PSS are resident process memory, and neither includes kernel TCP socket memory. The threaded row is FAILED; its active-probe result and incomplete responses are shown in the table.

## Backend failure and recovery

`bench/test_failover.py` kills and restarts backend 2 during a scheduled stream of requests. `Dropped` counts failed probes after the kill, including recovery. The latest failed probe time is shown only for trials with a failure before backend restart; the first successful request routed back to backend 2 is measured after restart. These are request-observation times, not direct health-check timer readings. The JSON contains each probe timestamp and outcome.

| Mode | Trials | Mean dropped probes (SD) | Drop range | Median last failed probe after kill (s) | Median first probe to recovered backend after restart (s) | Trials with pre-restore failures | Trials with observed re-entry |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| active_only | 20 | 3.65 ± 2.16 | 0–7 | 0.548 | 0.467 | 19 | 20 |
| passive | 20 | 0.00 ± 0.00 | 0–0 | not observed | 0.709 | 0 | 20 |

## Routing algorithm checks

`bench/test_algorithms.py` records backend identity in the responder header and asserts the observed properties. These are behavior checks, not statistical proof of universal distribution quality.

| Algorithm | Trials | Observed outcome |
| :--- | ---: | :--- |
| Round robin | 5 | 30 new connections split 10/10/10: True |
| Least connections | 5 | New held sessions balanced around two existing sessions: True |
| Consistent hashing | 5 | Same source IP retained its backend over reconnects: True |

## TCP half-close and buffered response check

`bench/test_half_close.py` sends a 1 MiB request, shuts down only the client write side, and verifies that the proxy drains the complete backend response before closing. This checks byte preservation for this test case; it is not a broad protocol-conformance suite.

| Proxy mode | Request bytes preserved | Response bytes drained | Hash matched | Result |
| :--- | ---: | ---: | :---: | :---: |
| threaded | 1,048,576 | 1,048,608 | True | PASS |
| epoll | 1,048,576 | 1,048,608 | True | PASS |

## Reproduce

Start the Compose services and build the binaries into their shared volume first. Run throughput inside the container where `wrk` is installed; run the remaining checks from the repository root:

```bash
docker compose exec -T loadbalancer bash -c '/app/bench/run_bench.sh'
./bench/run_idle_test.sh
python3 bench/test_failover.py 20 --port 18081
python3 bench/test_algorithms.py --runs 5 --port 18080
python3 bench/test_half_close.py
python3 bench/generate_report.py
```

The throughput suite needs `wrk`; the idle and fault-injection suites need Linux socket support and adequate file-descriptor limits. CPU pinning can be changed with `PIN_PROXY_1CORE`, `PIN_PROXY_4CORE`, and `PIN_LOADGEN`.
