#!/usr/bin/env python3
"""Measure request loss while one local backend is killed and restarted.

The harness runs without Docker, owns every child process it starts, saves raw
trial timestamps/results as JSON, and returns failure if the requested sample
count or request outcomes cannot be measured.
"""
import argparse
import asyncio
import json
import os
import random
import re
import signal
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT))
from bench.lib.stats import compute_stats, format_stats
BACKEND_PORTS = (9001, 9002, 9003)


def start_backend(binary: Path, port: int) -> subprocess.Popen:
    process = subprocess.Popen([str(binary), str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"backend {port} exited during startup")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return process
        except OSError:
            time.sleep(0.05)
    process.kill()
    raise TimeoutError(f"backend {port} failed readiness")


def stop_process(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def start_lb(binary: Path, lb_port: int, passive: bool) -> subprocess.Popen:
    # -h 1 makes active detection visible in this short trial. In active_only
    # mode -F isolates health-check detection; passive mode uses default retries.
    command = [str(binary), "-e", "-p", str(lb_port), "-h", "1"]
    if not passive:
        command.append("-F")
    for backend_port in BACKEND_PORTS:
        command.extend(["-b", f"127.0.0.1:{backend_port}"])
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("load balancer exited during startup")
        try:
            with socket.create_connection(("127.0.0.1", lb_port), timeout=0.1):
                return process
        except OSError:
            time.sleep(0.05)
    stop_process(process)
    raise TimeoutError("load balancer failed readiness")


async def send_probe(index: int, host: str, port: int) -> dict:
    sent_at = time.monotonic()
    writer = None
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=1.0)
        writer.write(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        await writer.drain()
        response = await asyncio.wait_for(reader.read(1024), timeout=1.0)
        success = b"200 OK" in response
        backend = re.search(rb"(?im)^X-Backend-Port:\s*(\d+)", response)
        return {"index": index, "sent_at": sent_at, "success": success,
                "backend_port": int(backend.group(1)) if backend else None,
                "error": "" if success else "non-200 or empty response"}
    except Exception as error:
        return {"index": index, "sent_at": sent_at, "success": False,
                "backend_port": None, "error": str(error)}
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass


async def stream_probes(host: str, port: int, rate: int, duration: float) -> list[dict]:
    # Schedule against monotonic deadlines rather than sleeping after each
    # response, so probe latency does not progressively lower the target rate.
    start = time.monotonic()
    tasks = []
    index = 0
    while True:
        tick = start + index / rate
        now = time.monotonic()
        if tick >= start + duration:
            break
        if tick > now:
            await asyncio.sleep(tick - now)
        tasks.append(asyncio.create_task(send_probe(index, host, port)))
        index += 1
    return await asyncio.gather(*tasks)


def run_trial(binary: Path, backend_binary: Path, lb_port: int, passive: bool,
              run_index: int, rate: int, duration: float, kill_offset: float,
              outage_duration: float) -> dict:
    backends = []
    lb = None
    try:
        for port in BACKEND_PORTS:
            backends.append(start_backend(backend_binary, port))
        lb = start_lb(binary, lb_port, passive)

        # Keep requests flowing while backend2 is down, and timestamp each
        # phase so drops/re-entry can be interpreted from actual observations.
        async def trial():
            probe_task = asyncio.create_task(stream_probes("127.0.0.1", lb_port, rate, duration))
            start = time.monotonic()
            await asyncio.sleep(kill_offset)
            kill_at = time.monotonic()
            backends[1].kill()
            backends[1].wait(timeout=2)
            await asyncio.sleep(outage_duration)
            restore_at = time.monotonic()
            backends[1] = start_backend(backend_binary, BACKEND_PORTS[1])
            results = await probe_task
            return start, kill_at, restore_at, results

        start, kill_at, restore_at, results = asyncio.run(trial())
        post_kill = [item for item in results if item["sent_at"] >= kill_at]
        pre_restore_failures = [item for item in post_kill if not item["success"] and item["sent_at"] < restore_at]
        all_failures = [item for item in post_kill if not item["success"]]
        detection_window = (max(item["sent_at"] for item in pre_restore_failures) - kill_at) if pre_restore_failures else None
        backend_returns = [item for item in results
                           if item["success"] and item["backend_port"] == BACKEND_PORTS[1]
                           and item["sent_at"] >= restore_at]
        backend_return_delay = (min(item["sent_at"] for item in backend_returns) - restore_at
                                if backend_returns else None)
        return {
            "trial": run_index,
            "mode": "passive" if passive else "active_only",
            "rate_per_second": rate,
            "duration_seconds": duration,
            "kill_offset_seconds": kill_at - start,
            "outage_seconds": restore_at - kill_at,
            "requests_total": len(results),
            "requests_after_kill": len(post_kill),
            "dropped_after_kill": len(all_failures),
            "dropped_before_restore": len(pre_restore_failures),
            "last_failure_before_restore_seconds": detection_window,
            "first_failure_seconds": (min(item["sent_at"] for item in pre_restore_failures) - kill_at) if pre_restore_failures else None,
            "first_success_to_restored_backend_seconds": backend_return_delay,
            "probe_results": [{**item, "sent_at": round(item["sent_at"] - kill_at, 6)} for item in results],
        }
    finally:
        stop_process(lb)
        for process in backends:
            stop_process(process)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trials", nargs="?", type=int, default=20)
    parser.add_argument("--port", type=int, default=8080)
    # Defaults keep this repeatable experiment short: 20 probes/s over 7 s.
    parser.add_argument("--rate", type=int, default=20)
    parser.add_argument("--duration", type=float, default=7.0)
    parser.add_argument("--outage", type=float, default=2.5)
    parser.add_argument("--seed", type=int, default=20261023)
    parser.add_argument("--output", default=str(ROOT / "results/failover_trials.json"))
    args = parser.parse_args()
    if args.trials < 1 or args.rate < 1 or args.duration <= 0 or args.outage <= 0:
        parser.error("trials/rate must be positive and durations must be > 0")

    build_dir = ROOT / "build"
    if not (build_dir / "loadbalancer").exists():
        build_dir = ROOT / "build-host"
    binary, backend_binary = build_dir / "loadbalancer", build_dir / "dummy_backend"
    random.seed(args.seed)
    data = {"seed": args.seed, "rate_per_second": args.rate, "duration_seconds": args.duration,
            "outage_seconds": args.outage, "trials_per_mode": args.trials, "modes": {}}

    for mode_index, passive in enumerate((False, True)):
        mode = "passive" if passive else "active_only"
        rows = []
        for i in range(args.trials):
            offset = random.uniform(0.8, min(2.0, args.duration - args.outage - 0.1))
            row = run_trial(binary, backend_binary, args.port, passive, i + 1, args.rate,
                            args.duration, offset, args.outage)
            rows.append(row)
            print(f"{mode} trial {i + 1}/{args.trials}: drops={row['dropped_after_kill']} "
                  f"pre_restore={row['dropped_before_restore']} "
                  f"last_failure={row['last_failure_before_restore_seconds']}")
        expected = round(args.rate * args.duration)
        if any(row["requests_total"] < expected * 0.9 for row in rows):
            raise RuntimeError(f"{mode}: one or more trials produced fewer than 90% of scheduled probes")
        data["modes"][mode] = rows

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(data, indent=2) + "\n")
    for mode, rows in data["modes"].items():
        drops = [row["dropped_after_kill"] for row in rows]
        windows = [row["last_failure_before_restore_seconds"] for row in rows
                   if row["last_failure_before_restore_seconds"] is not None]
        print(f"{mode}: drops {format_stats(compute_stats(drops), unit='requests')}; "
              f"measured failure window samples={len(windows)}")
    print(f"Raw failover trials written to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
