#!/usr/bin/env python3
"""End-to-end checks for the three routing algorithms.

The suite starts its own local backends and load balancer, records every trial
as JSON, and exits nonzero if a stated property is not observed.
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import tempfile
import time
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BACKENDS = (9001, 9002, 9003)


def wait_port(host: str, port: int, process: subprocess.Popen, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"process exited before {host}:{port} became ready")
        try:
            with socket.create_connection((host, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"{host}:{port} did not become ready")


def response_backend(sock: socket.socket) -> int:
    data = bytearray()
    while b"\r\n\r\n" not in data or len(data) < data.find(b"\r\n\r\n") + 4 + 13:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
    match = re.search(rb"(?im)^X-Backend-Port:\s*(\d+)", data)
    if not match:
        raise RuntimeError(f"backend response missing X-Backend-Port: {data[:200]!r}")
    return int(match.group(1))


def request(host: str, port: int, source_ip: str = "127.0.0.1", keep_open: bool = False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3)
    sock.bind((source_ip, 0))
    sock.connect((host, port))
    connection = "keep-alive" if keep_open else "close"
    sock.sendall(f"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: {connection}\r\n\r\n".encode())
    backend = response_backend(sock)
    if not keep_open:
        sock.close()
    return backend, sock


def run_trial(lb_bin: Path, host: str, listen_port: int, algo: str):
    log = tempfile.NamedTemporaryFile(mode="w+", delete=False)
    command = [str(lb_bin), "-e", "-p", str(listen_port), "-a", algo]
    for backend_port in BACKENDS:
        command += ["-b", f"127.0.0.1:{backend_port}"]
    process = subprocess.Popen(command, stdout=log, stderr=log)
    try:
        wait_port(host, listen_port, process)
        time.sleep(0.2)  # Drain the readiness connection before routing measurements.
        return process, log
    except Exception:
        process.terminate()
        process.wait(timeout=2)
        log.flush()
        log.seek(0)
        raise RuntimeError(log.read())


def stop_lb(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def read_stats(process: subprocess.Popen, log) -> dict:
    os.kill(process.pid, signal.SIGUSR1)
    time.sleep(0.1)
    log.flush()
    log.seek(0)
    text = log.read()
    parsed = {}
    for line in text.splitlines():
        match = re.search(r"(127\.0\.0\.1:\d+) \| Status: (UP|DOWN) \| Active Conns: (\d+) \| Backend Selections: (\d+)", line)
        if match:
            parsed[int(match.group(1).rsplit(":", 1)[1])] = {
                "status": match.group(2), "active": int(match.group(3)), "selections": int(match.group(4))
            }
    if set(parsed) != set(BACKENDS):
        raise RuntimeError(f"could not parse stats snapshot: {text[-1000:]}")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--output", default=str(ROOT / "results/algorithm_trials.json"))
    args = parser.parse_args()
    build = ROOT / "build" / "loadbalancer"
    lb_bin = build if build.exists() else ROOT / "build-host" / "loadbalancer"
    backend_bin = build.parent / "dummy_backend"
    if not backend_bin.exists():
        backend_bin = ROOT / "build-host" / "dummy_backend"
    if not lb_bin.exists() or not backend_bin.exists():
        raise SystemExit("Build loadbalancer and dummy_backend before running this suite")

    backend_processes = []
    trials = {"round_robin": [], "least_connections": [], "consistent_hashing": []}
    try:
        for backend_port in BACKENDS:
            process = subprocess.Popen([str(backend_bin), str(backend_port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            backend_processes.append(process)
            wait_port("127.0.0.1", backend_port, process)

        for run_index in range(args.runs):
            # Round robin: each of 30 new TCP connections must advance one slot.
            lb, log = run_trial(lb_bin, "127.0.0.1", args.port, "round_robin")
            counts = Counter(request("127.0.0.1", args.port)[0] for _ in range(30))
            stats = read_stats(lb, log)
            stop_lb(lb)
            log.close()
            if counts != Counter({p: 10 for p in BACKENDS}):
                raise AssertionError(f"round robin was not 10/10/10: requests={counts}, stats={stats}")
            trials["round_robin"].append({"counts": dict(counts), "stats": stats})

            # Least-connections: build a 2/1/1 baseline, then retain only the
            # two connections on backend 9001. New held sessions should avoid it.
            lb, log = run_trial(lb_bin, "127.0.0.1", args.port, "least_connections")
            initial = [request("127.0.0.1", args.port, keep_open=True) for _ in range(4)]
            held_by_backend = {p: [] for p in BACKENDS}
            for backend_port, sock in initial:
                held_by_backend[backend_port].append(sock)
            if len(held_by_backend[9001]) < 2:
                for sockets in held_by_backend.values():
                    for sock in sockets: sock.close()
                stop_lb(lb)
                raise AssertionError(f"unexpected initial least-connections placement: { {p: len(v) for p, v in held_by_backend.items()} }")
            # Keep exactly two sessions on the first backend; close all other
            # baseline connections. The next selections must prefer idle nodes.
            for p, sockets in held_by_backend.items():
                keep = sockets[:2] if p == 9001 else []
                for sock in sockets:
                    if sock not in keep: sock.close()
                held_by_backend[p] = keep
            time.sleep(0.2)  # Let the proxy observe those FINs before sampling least counts.
            added = []
            added_counts = Counter()
            for _ in range(8):
                backend_port, sock = request("127.0.0.1", args.port, keep_open=True)
                added_counts[backend_port] += 1
                added.append((backend_port, sock))
            stats = read_stats(lb, log)
            for sockets in held_by_backend.values():
                for sock in sockets: sock.close()
            for _, sock in added: sock.close()
            stop_lb(lb)
            log.close()
            if added_counts[9001] > 2 or max(added_counts.values()) - min(added_counts.values()) > 1:
                raise AssertionError(f"least-connections did not rebalance around the held load: initial={Counter(p for p, _ in initial)}, added={added_counts}")
            trials["least_connections"].append({"added_counts": dict(added_counts), "stats": stats})

            # Consistent hashing: each source IP must retain its backend across
            # two separate TCP connections in this process.
            lb, log = run_trial(lb_bin, "127.0.0.1", args.port, "consistent_hashing")
            mapping = {}
            distribution = Counter()
            for source_octet in range(1, 31):
                source_ip = f"127.0.0.{source_octet}"
                first = request("127.0.0.1", args.port, source_ip)[0]
                second = request("127.0.0.1", args.port, source_ip)[0]
                if first != second:
                    stop_lb(lb)
                    raise AssertionError(f"client {source_ip} changed backend: {first} -> {second}")
                mapping[source_ip] = first
                distribution[first] += 1
            stats = read_stats(lb, log)
            stop_lb(lb)
            log.close()
            if sum(distribution.values()) != 30:
                raise AssertionError(f"consistent hashing lost clients: {distribution}")
            trials["consistent_hashing"].append({"distribution": dict(distribution), "mapping": mapping, "stats": stats})
            print(f"run {run_index + 1}/{args.runs}: RR={dict(counts)} LC={dict(added_counts)} CH={dict(distribution)}")
    finally:
        for process in backend_processes:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=2)
                except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=2)

    output = {"runs": args.runs, "backend_ports": list(BACKENDS), "trials": trials}
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(json.dumps(output, indent=2) + "\n")
    print(f"PASS: all algorithm properties verified; raw results: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
