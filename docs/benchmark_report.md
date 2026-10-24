# Benchmark report

**Generated:** 2026-10-24 12:49 UTC<br>
**Scope:** educational comparison of a small TCP proxy’s blocking and `epoll` implementations. These measurements do not claim production readiness or feature parity with NGINX/HAProxy.

## How to read this report

Every table below is generated from the current files in `results/`. The CSV and JSON files are the raw trial records. A row marked `PARTIAL` or `FAILED` is not a successful target run. Missing artifacts mean that suite has not been run; no estimate is substituted.

`wrk` measures HTTP request throughput over the TCP proxy. This is a short loopback microbenchmark using the included small HTTP responder, not a general proxy-performance result or production capacity test. It has no long soak, overload recovery, or process-level CPU/memory profile. `wrk 4.2.0 --latency` emits P50, P75, P90, and P99 in this setup; it does not emit P95 or P99.9. Runs with different environment or concurrency settings should be treated as separate experiments. “Pinned CPU set busy” is host CPU time for the selected CPU IDs, not CPU time attributed to the proxy process.

Settings: 5 repetitions; 2 s warmup; 5 s measured; concurrency 50, 100, 200, 400, 800; proxy CPUs `0` / `0-3`; load-generator CPUs `4-7`; backend CPUs `8-11`; Nginx CPUs `0`. Platform: `Linux-7.2.9-arch1-1-x86_64-with-glibc2.39`; kernel `7.2.9-arch1-1`; CPUs `12`; wrk `wrk 4.2.0 [epoll] Copyright (C) 2012 Will Glozer`.

## Active throughput and latency

`bench/run_bench.sh` records one row per mode, concurrency, and repetition. The table reports medians for RPS, latency, pinned CPU set use, and context switches; ranges show RPS spread. Per-thread context switches are sampled during each measurement and summed for thread IDs observed in that interval. The harness records failures and raw `wrk` output under `results/raw/`.

| Mode | Connections | Samples | Median RPS (range) | Median P50 (ms) | Median P90 (ms) | Median P99 (ms) | Pinned CPU set busy % | Voluntary CSW/s | Nonvoluntary CSW/s | Socket errors | Non-PASS runs |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Control | 50 | 5 | 144,667 (144,045–145,352) | 0.318 | 0.355 | 2.520 | 64.6 | — | — | 0 | 0 |
| Control | 100 | 5 | 146,119 (144,408–146,811) | 0.658 | 0.754 | 2.840 | 64.2 | — | — | 0 | 0 |
| Control | 200 | 5 | 143,090 (142,872–143,536) | 1.370 | 1.560 | 3.350 | 65.4 | — | — | 0 | 0 |
| Control | 400 | 5 | 132,933 (132,162–133,355) | 2.970 | 3.380 | 4.920 | 64.7 | — | — | 0 | 0 |
| Control | 800 | 5 | 120,218 (119,163–121,560) | 6.560 | 7.210 | 8.690 | 64.5 | — | — | 0 | 0 |
| Epoll_4Workers | 50 | 5 | 320,888 (317,653–323,890) | 0.109 | 1.820 | 4.350 | 92.8 | 51,380.0 | 325.0 | 0 | 0 |
| Epoll_4Workers | 100 | 5 | 328,103 (324,979–331,244) | 0.228 | 2.170 | 5.040 | 93.4 | 38,947.0 | 339.0 | 0 | 0 |
| Epoll_4Workers | 200 | 5 | 332,645 (323,499–336,993) | 0.440 | 2.480 | 5.620 | 94.0 | 26,641.0 | 393.0 | 0 | 0 |
| Epoll_4Workers | 400 | 5 | 313,105 (309,361–315,533) | 0.990 | 3.360 | 6.820 | 95.3 | 13,587.0 | 369.0 | 0 | 0 |
| Epoll_4Workers | 800 | 5 | 264,735 (261,974–269,194) | 2.470 | 5.280 | 9.650 | 96.4 | 5,885.0 | 385.0 | 0 | 0 |
| Epoll_Optimized | 50 | 5 | 122,381 (121,168–125,817) | 0.319 | 1.130 | 3.250 | 95.8 | 91.0 | 112.0 | 0 | 0 |
| Epoll_Optimized | 100 | 5 | 118,756 (117,116–119,232) | 0.723 | 1.680 | 3.820 | 96.0 | 64.0 | 120.0 | 0 | 0 |
| Epoll_Optimized | 200 | 5 | 109,515 (108,678–112,358) | 1.640 | 2.740 | 5.170 | 97.6 | 41.0 | 114.0 | 0 | 0 |
| Epoll_Optimized | 400 | 5 | 98,350 (97,688–99,498) | 3.810 | 5.150 | 7.200 | 98.8 | 19.0 | 123.0 | 0 | 0 |
| Epoll_Optimized | 800 | 5 | 86,888 (86,393–87,303) | 8.930 | 10.620 | 12.270 | 99.0 | 2.0 | 128.0 | 0 | 0 |
| Nginx_Stream | 50 | 5 | 115,047 (113,785–119,781) | 0.340 | 1.130 | 3.270 | 96.2 | — | — | 0 | 0 |
| Nginx_Stream | 100 | 5 | 108,764 (108,420–111,727) | 0.799 | 1.730 | 3.900 | 97.0 | — | — | 0 | 0 |
| Nginx_Stream | 200 | 5 | 95,752 (95,409–98,300) | 1.910 | 2.880 | 5.100 | 98.2 | — | — | 0 | 0 |
| Nginx_Stream | 400 | 5 | 84,164 (83,693–84,742) | 4.510 | 5.730 | 7.650 | 99.6 | — | — | 0 | 0 |
| Nginx_Stream | 800 | 5 | 75,641 (74,801–76,276) | 10.350 | 11.930 | 13.420 | 99.8 | — | — | 0 | 0 |
| Threaded_4Cores | 50 | 5 | 252,544 (250,216–254,193) | 0.143 | 1.620 | 3.650 | 92.4 | 489,232.0 | 21,935.0 | 0 | 0 |
| Threaded_4Cores | 100 | 5 | 248,373 (246,571–250,215) | 0.306 | 2.110 | 5.070 | 93.6 | 487,378.0 | 11,632.0 | 0 | 0 |
| Threaded_4Cores | 200 | 5 | 221,687 (219,931–226,139) | 0.695 | 3.410 | 11.790 | 94.6 | 433,395.0 | 4,323.0 | 0 | 0 |
| Threaded_4Cores | 400 | 5 | 191,407 (190,331–194,222) | 1.700 | 5.230 | 15.180 | 95.4 | 374,330.0 | 2,828.0 | 0 | 0 |
| Threaded_4Cores | 800 | 5 | 172,794 (170,415–173,367) | 3.960 | 9.010 | 19.980 | 96.5 | 334,867.0 | 2,014.0 | 0 | 0 |
| Threaded_Proxy | 50 | 5 | 75,780 (74,074–76,527) | 0.531 | 1.970 | 3.450 | 89.7 | 147,868.0 | 455.0 | 0 | 0 |
| Threaded_Proxy | 100 | 5 | 66,648 (65,975–66,947) | 1.340 | 3.110 | 5.610 | 90.6 | 130,487.0 | 204.0 | 0 | 0 |
| Threaded_Proxy | 200 | 5 | 60,564 (60,390–61,025) | 3.110 | 5.660 | 10.870 | 92.4 | 118,362.0 | 246.0 | 0 | 0 |
| Threaded_Proxy | 400 | 5 | 58,214 (57,926–58,891) | 6.740 | 10.110 | 16.850 | 95.7 | 114,263.0 | 360.0 | 0 | 0 |
| Threaded_Proxy | 800 | 5 | 57,475 (57,248–58,084) | 13.780 | 20.270 | 29.250 | 99.0 | 111,998.0 | 398.0 | 0 | 0 |

## Idle connection capacity

`bench/run_idle_test.sh` samples proxy process memory while the client still holds the established connections. `Established` is the number actually opened, not the requested target. Container cgroup memory includes the load generator as well as the proxy. `PASS` requires reaching the target and a successful active probe; `PARTIAL` means the target was not reached while the proxy remained responsive; `FAILED` means the proxy was absent or the active probe failed.

| Mode | Target | Established during hold | Handshake s | Connections/s | Threads | RSS MB | PSS MB | Cgroup MB | Active probe ms | Status |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| Epoll_Optimized | 10000 | 10000 | 1.44 | 6944 | 3 | 6.52 | 3.52 | 180.11 | 0.04 | PASS |
| Threaded_Proxy | 10000 | 9265 | 13.47 | 688 | 3 | 17.79 | 14.80 | 26.14 | 0.00 | FAILED |
| Epoll_Optimized | 20000 | 7344 | 32.05 | 229 | 3 | 5.80 | 2.85 | 140.41 | 0.00 | FAILED |
| Threaded_Proxy | 20000 | 9265 | 6.07 | 1526 | 3 | 17.97 | 14.95 | 28.62 | 0.00 | FAILED |
| Epoll_Optimized | 30000 | 10769 | 26.27 | 410 | 3 | 6.71 | 3.76 | 199.04 | 0.00 | FAILED |
| Threaded_Proxy | 30000 | 9265 | 20.68 | 448 | 3 | 17.87 | 14.86 | 26.01 | 0.00 | FAILED |

## Backend failure and recovery

`bench/test_failover.py` kills and restarts backend 2 during a scheduled stream of requests. `Dropped` counts failed probes after the kill, including recovery. The latest failed probe time is shown only for trials with a failure before backend restart; the first successful request routed back to backend 2 is measured after restart. These are request-observation times, not direct health-check timer readings. The JSON contains each probe timestamp and outcome.

| Mode | Trials | Mean dropped probes (SD) | Drop range | Median last failed probe after kill (s) | Median first probe to recovered backend after restart (s) | Trials with pre-restore failures | Trials with observed re-entry |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| active_only | 20 | 3.80 ± 2.04 | 1–7 | 0.587 | 0.487 | 20 | 20 |
| passive | 20 | 0.00 ± 0.00 | 0–0 | not observed | 0.708 | 0 | 20 |

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
