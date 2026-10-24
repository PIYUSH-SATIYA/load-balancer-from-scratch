# Iteration 9: Benchmarking the Learning Proxy

## Goal

Use small, repeatable experiments to compare the blocking and `epoll` implementations and to practice reading concurrency, latency, and failure data. This work is an educational exercise; it does not establish production readiness or NGINX/HAProxy equivalence.

## Suites

- `bench/run_bench.sh` drives the proxy and NGINX stream configuration with `wrk` and records one CSV row per run.
- `bench/run_idle_test.sh` attempts progressively larger sets of idle TCP sessions and samples memory while those sessions are still open. It records achieved counts, so reaching fewer connections than requested is visible as a partial result.
- `bench/test_failover.py` kills and restarts a local backend while sending requests in active-only and passive-retry modes. It keeps timestamped probe outcomes for each trial.
- `bench/test_algorithms.py` checks observed routing behavior and saves the per-run backend mapping.
- `bench/test_half_close.py` checks large payload preservation when the client half-closes its write side in both proxy modes.

## Running and reporting

Start the Docker Compose services and build the binaries into the shared build volume. Run throughput inside the container where `wrk` is installed; the remaining checks run from the host:

```bash
cmake -B build-host -S .
cmake --build build-host
docker compose exec -T loadbalancer bash -c '/app/bench/run_bench.sh'
HOLD_SEC=5 ./bench/run_idle_test.sh
python3 bench/test_failover.py 20 --port 18081
python3 bench/test_algorithms.py --runs 5 --port 18080
python3 bench/test_half_close.py
python3 bench/generate_report.py
```

The throughput suite requires `wrk`. The idle sweep needs large descriptor limits and may produce partial results, especially for the thread-per-connection implementation. Those results are not promoted to successful target runs. The authoritative current tables and per-trial artifacts are linked from [`benchmark_report.md`](../benchmark_report.md).

## Learning notes

The comparison helps make the costs of blocking threads, event loops, backend connection handling, and TCP lifecycle behavior visible. Results are specific to the host, kernel, compiler, response size, and load generator. A loopback microbenchmark is useful for learning and debugging, but it does not predict production behavior across machines or workloads.
