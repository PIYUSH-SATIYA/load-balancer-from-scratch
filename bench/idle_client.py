#!/usr/bin/env python3
import asyncio
import sys
import time
import statistics

async def open_idle_conn(host, port):
    try:
        reader, writer = await asyncio.open_connection(host, port)
        # Send one keepalive HTTP GET
        req = b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        writer.write(req)
        await writer.drain()
        await reader.read(256)
        return reader, writer
    except Exception:
        return None

async def measure_request_latency(host, port, samples=15):
    try:
        reader, writer = await asyncio.open_connection(host, port)
        req = b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        # 1 warmup
        writer.write(req)
        await writer.drain()
        await reader.read(256)

        latencies = []
        for _ in range(samples):
            t0 = time.perf_counter()
            writer.write(req)
            await writer.drain()
            await reader.read(256)
            latencies.append((time.perf_counter() - t0) * 1000.0)

        writer.close()
        await writer.wait_closed()
        return statistics.median(latencies) if latencies else 0.0
    except Exception:
        return 0.0

async def run(target_conns, host="loadbalancer", port=8080, hold_seconds=5):
    print(f"Connecting {target_conns} idle connections to {host}:{port}...")
    connections = []
    batch_size = 500

    start_time = time.time()
    for i in range(0, target_conns, batch_size):
        cur_batch = min(batch_size, target_conns - i)
        tasks = [open_idle_conn(host, port) for _ in range(cur_batch)]
        results = await asyncio.gather(*tasks)
        for r in results:
            if r is not None:
                connections.append(r)
        print(f"  Established {len(connections)} / {target_conns}...", end="\r", flush=True)
        await asyncio.sleep(0.02)

    duration = time.time() - start_time
    established = len(connections)
    rate = established / max(duration, 0.001)
    print(f"\nHandshake Result: {established}/{target_conns} established in {duration:.2f}s ({rate:.0f} conn/s)")

    print("Measuring active request latency over separate keep-alive connection (15 samples)...")
    lat_ms = await measure_request_latency(host, port, samples=15)
    print(f"Active Request Latency (median): {lat_ms:.2f} ms")

    print(f"Holding connections for {hold_seconds} seconds for server memory inspection...")
    await asyncio.sleep(hold_seconds)

    print("Tearing down connections...")
    for reader, writer in connections:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

    print(f"FINAL_METRICS: Established={established} HandshakeSec={duration:.2f} ConnRate={rate:.0f} ActiveLatMs={lat_ms:.2f}")

if __name__ == "__main__":
    conns = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    host = sys.argv[2] if len(sys.argv) > 2 else "loadbalancer"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8080
    hold = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    asyncio.run(run(conns, host, port, hold))
