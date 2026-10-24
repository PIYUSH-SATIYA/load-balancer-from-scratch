#!/usr/bin/env python3
"""
Scalable Async Idle Connection Load Generator.
Maintains thousands of persistent HTTP keep-alive connections to evaluate C10K/C30K capacity.
Provides stage synchronization markers for exact memory and resource profiling.
"""
import sys
import time
import socket
import asyncio
import statistics
import re
from typing import List, Tuple, Optional

async def read_http_response(reader: asyncio.StreamReader) -> bytes:
    headers = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=5.0)
    match = re.search(rb"(?im)^Content-Length:\s*(\d+)\s*$", headers)
    if not match:
        raise ValueError("response has no Content-Length")
    body = await asyncio.wait_for(reader.readexactly(int(match.group(1))), timeout=5.0)
    return headers + body


async def open_idle_conn(host: str, port: int, local_ip: Optional[str] = None):
    """Establishes an HTTP keep-alive connection and keeps the socket idle."""
    try:
        local_addr = (local_ip, 0) if local_ip else None
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port, local_addr=local_addr),
            timeout=5.0
        )
        req = b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        writer.write(req)
        await writer.drain()
        resp = await read_http_response(reader)
        if b"200 OK" in resp:
            return (reader, writer), None
        writer.close()
        await writer.wait_closed()
        return None, "invalid_http_response"
    except Exception as error:
        return None, f"{type(error).__name__}:{getattr(error, 'errno', '')}"

async def measure_active_latency(host: str, port: int, samples: int = 20) -> float:
    """Measures request-response latency under concurrent idle load."""
    latencies = []
    try:
        reader, writer = await asyncio.open_connection(host, port)
        req = b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        # 1 warmup transaction
        writer.write(req)
        await writer.drain()
        response = await read_http_response(reader)
        if b"200 OK" not in response:
            writer.close()
            await writer.wait_closed()
            return 0.0

        for _ in range(samples):
            t0 = time.perf_counter()
            writer.write(req)
            await writer.drain()
            response = await read_http_response(reader)
            if b"200 OK" not in response:
                writer.close()
                await writer.wait_closed()
                return 0.0
            latencies.append((time.perf_counter() - t0) * 1000.0)

        writer.close()
        await writer.wait_closed()
        return statistics.median(latencies) if latencies else 0.0
    except Exception:
        return 0.0

async def run(target_conns: int, host: str = "127.0.0.1", port: int = 8080,
              hold_seconds: int = 30, source_ip_base: str = "127.0.0.1",
              marker_path: Optional[str] = None):
    print(f"[STAGE] HANDSHAKE_START Target={target_conns} Host={host}:{port}")
    connections: List[Tuple[asyncio.StreamReader, asyncio.StreamWriter]] = []
    failures = {}
    batch_size = 500

    start_time = time.time()
    for i in range(0, target_conns, batch_size):
        cur_batch = min(batch_size, target_conns - i)
        tasks = []
        for j in range(cur_batch):
            conn_idx = i + j
            # If targeting loopback and target > 15000, rotate local loopback IP across 127.0.0.1, 127.0.0.2, etc.
            if host in ("127.0.0.1", "localhost"):
                octets = source_ip_base.split(".")
                octets[3] = str(int(octets[3]) + (conn_idx // 15000))
                local_ip = ".".join(octets)
            else:
                local_ip = None
            tasks.append(open_idle_conn(host, port, local_ip=local_ip))
        results = await asyncio.gather(*tasks)
        for connection, error in results:
            if connection is not None:
                connections.append(connection)
            else:
                failures[error] = failures.get(error, 0) + 1
        print(f"  Established {len(connections)} / {target_conns}...", end="\r", flush=True)
        await asyncio.sleep(0.01)

    duration = time.time() - start_time
    established = len(connections)
    rate = established / max(duration, 0.001)
    
    print(f"\n[STAGE] HANDSHAKE_COMPLETE Established={established}/{target_conns} DurationSec={duration:.2f} ConnRate={rate:.0f} Failures={failures}", flush=True)

    print(f"[STAGE] PROBING_ACTIVE_LATENCY (20 samples under idle load)...")
    active_lat_ms = await measure_active_latency(host, port, samples=20)
    print(f"Active Request Latency: {active_lat_ms:.3f} ms")

    if marker_path:
        from pathlib import Path
        Path(marker_path).touch()
    print(f"[STAGE] HOLD_START DurationSec={hold_seconds} Established={established} HandshakeSec={duration:.2f} ConnRate={rate:.0f} ActiveLatMs={active_lat_ms:.2f}", flush=True)
    await asyncio.sleep(hold_seconds)
    print(f"[STAGE] HOLD_COMPLETE")

    print("[STAGE] TEARDOWN_START Closing connections...")
    for reader, writer in connections:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

    print(f"[STAGE] TEARDOWN_COMPLETE")
    print(f"FINAL_METRICS: Established={established} HandshakeSec={duration:.2f} ConnRate={rate:.0f} ActiveLatMs={active_lat_ms:.2f} Failures={failures}")

if __name__ == "__main__":
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        target_limit = min(65536, hard)
        resource.setrlimit(resource.RLIMIT_NOFILE, (target_limit, target_limit))
    except Exception:
        pass

    conns = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8080
    hold = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    source_ip_base = sys.argv[5] if len(sys.argv) > 5 else "127.0.0.1"
    marker_path = sys.argv[6] if len(sys.argv) > 6 else None
    asyncio.run(run(conns, host, port, hold, source_ip_base, marker_path))
