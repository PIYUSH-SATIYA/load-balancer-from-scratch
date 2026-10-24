#!/usr/bin/env python3
"""Check byte preservation and both TCP half-closes in the proxy modes."""
import hashlib
import json
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESPONSE_SIZE = 1 * 1024 * 1024


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def backend_process(port: int) -> None:
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen()

    def serve(client: socket.socket) -> None:
        with client:
            chunks = []
            while True:
                chunk = client.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
            request = b"".join(chunks)
            print(f"test backend received {len(request)} bytes", flush=True)
            if not request:
                return
            response = hashlib.sha256(request).digest() + b"R" * RESPONSE_SIZE
            try:
                client.sendall(response)
                print(f"test backend sent {len(response)} bytes", flush=True)
                client.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    while True:
        client, _ = listener.accept()
        threading.Thread(target=serve, args=(client,), daemon=True).start()


def wait_ready(process: subprocess.Popen, port: int, timeout: float = 5) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("proxy exited before opening its listener")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"proxy did not listen on {port}")


def stop(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def run_mode(binary: Path, mode: str) -> dict:
    backend_port, proxy_port = free_port(), free_port()
    backend = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--backend", str(backend_port)])
    proxy = None
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if backend.poll() is not None:
                raise RuntimeError("test backend exited during startup")
            try:
                with socket.create_connection(("127.0.0.1", backend_port), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise TimeoutError("test backend did not start")

        command = [str(binary), "-p", str(proxy_port), "-b", f"127.0.0.1:{backend_port}"]
        if mode == "epoll":
            command.append("-e")
        proxy = subprocess.Popen(command)
        wait_ready(proxy, proxy_port)
        time.sleep(0.05)

        request = bytes(range(256)) * 4096
        expected = hashlib.sha256(request).digest() + b"R" * RESPONSE_SIZE
        with socket.create_connection(("127.0.0.1", proxy_port), timeout=5) as client:
            deadline = time.monotonic() + 30
            client.settimeout(10)
            client.sendall(request)
            client.shutdown(socket.SHUT_WR)
            time.sleep(0.1)  # Let the backend fill the relay's bounded output buffer.
            chunks = []
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    received = sum(map(len, chunks))
                    raise TimeoutError(f"response did not drain within 30 seconds ({received}/{len(expected)} bytes)")
                client.settimeout(remaining)
                try:
                    chunk = client.recv(65536)
                except TimeoutError as error:
                    received = sum(map(len, chunks))
                    raise TimeoutError(f"response stalled at {received}/{len(expected)} bytes") from error
                if not chunk:
                    break
                chunks.append(chunk)
        actual = b"".join(chunks)
        if actual != expected:
            raise AssertionError(f"{mode}: response mismatch ({len(actual)} of {len(expected)} bytes)")
        print(f"PASS {mode}: preserved 1 MiB request and drained {len(actual)} response bytes after half-close", flush=True)
        return {
            "mode": mode,
            "request_bytes": len(request),
            "response_bytes": len(actual),
            "response_sha256_matches": hashlib.sha256(actual).hexdigest() == hashlib.sha256(expected).hexdigest(),
            "passed": True,
        }
    finally:
        stop(proxy)
        stop(backend)


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--backend":
        backend_process(int(sys.argv[2]))
        return 0
    build_dir = ROOT / "build"
    binary = build_dir / "loadbalancer"
    if not binary.exists():
        binary = ROOT / "build-host" / "loadbalancer"
    if not binary.exists():
        raise SystemExit("Build loadbalancer before running this check")
    results = []
    for mode in ("threaded", "epoll"):
        print(f"Checking {mode} half-close behavior", flush=True)
        results.append(run_mode(binary, mode))
    (ROOT / "results" / "half_close.json").write_text(json.dumps({"trials": results}, indent=2) + "\n")
    print(f"Wrote {ROOT / 'results' / 'half_close.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
