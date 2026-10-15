#!/usr/bin/env python3
"""
High-Availability Failover & Recovery Benchmark Suite.
Executes 20 consecutive automated Monte Carlo trials:
  - Streams continuous concurrent HTTP requests at 20 req/s using asyncio
  - Injects SIGKILL fault into backend2 at randomized offsets
  - Evaluates both Active-Only failover (-F) and Passive Failover modes
  - Measures failure convergence duration, dropped requests, and recovery restoration
  - Computes audit-grade statistical distribution (Min, Median, Mean ± StdDev, Max)
"""
import sys
import os
import time
import random
import asyncio
import subprocess
from typing import Dict, List, Tuple, Any

# Add bench directory to path for bench.lib.stats
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bench.lib.stats import compute_stats, format_stats

def run_cmd(cmd: str) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)

async def send_single_probe(session_idx: int, target_url: str, timeout: float = 1.0) -> Tuple[int, float, bool, str, float]:
    """
    Sends a single HTTP GET probe.
    Returns: (session_idx, send_timestamp, success, error_message, latency_ms)
    """
    host = "127.0.0.1"
    port = 8080
    t0 = time.time()
    try:
        # Use low-level asyncio TCP socket stream for zero-overhead non-blocking I/O
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=timeout)
        req = b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        writer.write(req)
        await writer.drain()
        resp = await asyncio.wait_for(reader.read(512), timeout=timeout)
        writer.close()
        await writer.wait_closed()

        lat_ms = (time.time() - t0) * 1000.0
        success = b"200 OK" in resp
        err = "" if success else "Non-200 Response"
        return (session_idx, t0, success, err, lat_ms)
    except Exception as e:
        lat_ms = (time.time() - t0) * 1000.0
        return (session_idx, t0, False, str(e), lat_ms)

async def stream_probes(target_url: str, rate_per_sec: int, duration_sec: float) -> List[Tuple[int, float, bool, str, float]]:
    """Streams requests at a strictly constant rate (e.g. 20 req/s) concurrently."""
    interval = 1.0 / rate_per_sec
    tasks = []
    start_time = time.time()
    idx = 0

    while (time.time() - start_time) < duration_sec:
        next_tick = start_time + (idx * interval)
        now = time.time()
        if next_tick > now:
            await asyncio.sleep(next_tick - now)
        tasks.append(asyncio.create_task(send_single_probe(idx, target_url)))
        idx += 1

    results = await asyncio.gather(*tasks)
    return results

def run_single_failover_trial(trial_idx: int, use_passive: bool = True, project_root: str = "") -> Dict[str, Any]:
    """Executes a single randomized HA failover trial."""
    lb_bin = os.path.join(project_root, "build", "loadbalancer")
    if not os.path.exists(lb_bin):
        lb_bin = os.path.join(project_root, "build-host", "loadbalancer")

    # 1. Clean previous state and start all 3 backends if using docker
    run_cmd("docker compose start backend1 backend2 backend3 2>/dev/null || true")
    run_cmd("docker compose exec -T loadbalancer pkill -9 loadbalancer 2>/dev/null || true")
    run_cmd("pkill -9 -f loadbalancer 2>/dev/null || true")
    time.sleep(0.5)

    # 2. Start load balancer with 1s health check interval (-h 1)
    pass_flag = "" if use_passive else "-F"
    is_docker = run_cmd("docker compose ps -q loadbalancer 2>/dev/null").stdout.strip() != ""
    if is_docker:
        run_cmd(f"docker compose exec -d loadbalancer taskset -c 0 /app/build/loadbalancer -e -h 1 {pass_flag} -p 8080")
        lb_proc = None
    else:
        lb_proc = subprocess.Popen(
            f"taskset -c 0 {lb_bin} -e -h 1 {pass_flag} -p 8080 > /dev/null 2>&1",
            shell=True, preexec_fn=os.setsid
        )
    time.sleep(1.0)

    # Randomized kill offset in [1.2s, 2.0s] to avoid phase locking with health check tick
    kill_offset = random.uniform(1.2, 2.0)
    total_duration = 7.0
    rate = 20

    async def trial_coro():
        # Stream probes in background
        probe_task = asyncio.create_task(stream_probes("http://127.0.0.1:8080/", rate_per_sec=rate, duration_sec=total_duration))
        
        # Wait until kill_offset
        await asyncio.sleep(kill_offset)
        t_kill = time.time()
        
        # Kill backend2 container (or local process)
        run_cmd("docker compose kill -s SIGKILL backend2 2>/dev/null || pkill -9 -f 'dummy_backend 9002' 2>/dev/null || true")
        
        # Wait 2.5s and then restore backend2 to test recovery
        await asyncio.sleep(2.5)
        t_restore = time.time()
        run_cmd("docker compose start backend2 2>/dev/null || true")

        results = await probe_task
        return t_kill, t_restore, results

    t_kill, t_restore, results = asyncio.run(trial_coro())

    # Stop load balancer
    if lb_proc:
        try:
            os.killpg(os.getpgid(lb_proc.pid), 9)
        except Exception:
            pass
    run_cmd("docker compose exec -T loadbalancer pkill -9 loadbalancer 2>/dev/null || true")
    run_cmd("pkill -9 -f loadbalancer 2>/dev/null || true")

    # Post-kill analysis
    post_kill = [r for r in results if r[1] >= t_kill]
    failures = [r for r in post_kill if not r[2]]

    if failures:
        first_fail_t = failures[0][1] - t_kill
        last_fail_t = failures[-1][1] - t_kill
        convergence = max(0.0, last_fail_t + (1.0 / rate))
    else:
        first_fail_t = 0.0
        last_fail_t = 0.0
        convergence = 0.0

    return {
        "trial": trial_idx,
        "use_passive": use_passive,
        "kill_offset": kill_offset,
        "total_requests": len(results),
        "post_kill_requests": len(post_kill),
        "failed_requests": len(failures),
        "convergence_sec": convergence,
        "first_fail_sec": first_fail_t
    }

def main():
    print("========================================================================")
    print(" HIGH-AVAILABILITY FAILOVER & HA CONVERGENCE BENCHMARK")
    print("========================================================================")

    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    trials_count = 20
    if len(sys.argv) > 1:
        trials_count = int(sys.argv[1])

    # 1. Evaluate Passive Failover Enabled (Default)
    print(f"\n[Test Suite A] Passive Failover ENABLED ({trials_count} Monte Carlo trials @ 20 req/s)...")
    passive_trials = []
    for i in range(trials_count):
        res = run_single_failover_trial(i + 1, use_passive=True, project_root=project_root)
        passive_trials.append(res)
        print(f"  Trial {i+1:2d}: Failures={res['failed_requests']:2d} | Convergence={res['convergence_sec']:.3f}s (offset={res['kill_offset']:.2f}s)")

    # 2. Evaluate Active Health Check Only (-F, Passive Disabled)
    print(f"\n[Test Suite B] Active-Only Failover (-F, {trials_count} Monte Carlo trials @ 20 req/s)...")
    active_trials = []
    for i in range(trials_count):
        res = run_single_failover_trial(i + 1, use_passive=False, project_root=project_root)
        active_trials.append(res)
        print(f"  Trial {i+1:2d}: Failures={res['failed_requests']:2d} | Convergence={res['convergence_sec']:.3f}s (offset={res['kill_offset']:.2f}s)")

    # Compute Statistics matching Table 3.5 format
    conv_pass = [t["convergence_sec"] for t in passive_trials]
    drops_pass = [t["failed_requests"] for t in passive_trials]

    conv_act = [t["convergence_sec"] for t in active_trials]
    drops_act = [t["failed_requests"] for t in active_trials]

    s_conv_pass = compute_stats(conv_pass)
    s_drops_pass = compute_stats(drops_pass)

    s_conv_act = compute_stats(conv_act)
    s_drops_act = compute_stats(drops_act)

    print("\n========================================================================")
    print(" EMPIRICAL FAILOVER COMPARISON (Table 3.5)")
    print("========================================================================")
    print("### Mode 1: Active-Only Failover (-F, Probes every 1.0s):")
    print(f"| Metric | Minimum | Median | Mean ± StdDev | Maximum |")
    print(f"| :--- | :--- | :--- | :--- | :--- |")
    print(f"| **Convergence Duration** | **{s_conv_act['min']:.3f} s** | **{s_conv_act['median']:.3f} s** | **{s_conv_act['mean']:.3f} s ± {s_conv_act['stddev']:.3f} s** | **{s_conv_act['max']:.3f} s** |")
    print(f"| **Total Dropped Requests** | **{s_drops_act['min']:.0f}** | **{s_drops_act['median']:.1f}** | **{s_drops_act['mean']:.2f} ± {s_drops_act['stddev']:.2f}** | **{s_drops_act['max']:.0f}** |")

    print("\n### Mode 2: Passive Failover + Active Health Check (Instant Retry):")
    print(f"| Metric | Minimum | Median | Mean ± StdDev | Maximum |")
    print(f"| :--- | :--- | :--- | :--- | :--- |")
    print(f"| **Convergence Duration** | **{s_conv_pass['min']:.3f} s** | **{s_conv_pass['median']:.3f} s** | **{s_conv_pass['mean']:.3f} s ± {s_conv_pass['stddev']:.3f} s** | **{s_conv_pass['max']:.3f} s** |")
    print(f"| **Total Dropped Requests** | **{s_drops_pass['min']:.0f}** | **{s_drops_pass['median']:.1f}** | **{s_drops_pass['mean']:.2f} ± {s_drops_pass['stddev']:.2f}** | **{s_drops_pass['max']:.0f}** |")

if __name__ == "__main__":
    main()
