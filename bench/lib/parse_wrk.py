#!/usr/bin/env python3
"""
Robust parser for `wrk --latency` output.
Normalizes latencies to milliseconds (ms) and extracts error counts.
"""
import sys
import re
import json

def parse_time_to_ms(time_str: str) -> float:
    """Convert time string like '10.50us', '1.20ms', '2.50s' to milliseconds."""
    time_str = time_str.strip()
    match = re.match(r"^([0-9.]+)\s*([a-zA-Z]+)?$", time_str)
    if not match:
        return 0.0
    val = float(match.group(1))
    unit = match.group(2).lower() if match.group(2) else "ms"
    if unit in ("us", "µs"):
        return val / 1000.0
    elif unit == "ms":
        return val
    elif unit == "s":
        return val * 1000.0
    elif unit == "m":
        return val * 60000.0
    return val

def parse_wrk_output(text: str) -> dict:
    """Parse complete wrk stdout text and return structured metrics dictionary."""
    metrics = {
        "rps": 0.0,
        "p50_ms": 0.0,
        "p75_ms": 0.0,
        "p90_ms": 0.0,
        "p99_ms": 0.0,
        "p99_9_ms": 0.0,
        "mean_lat_ms": 0.0,
        "std_lat_ms": 0.0,
        "max_lat_ms": 0.0,
        "requests": 0,
        "duration_sec": 0.0,
        "socket_errors_connect": 0,
        "socket_errors_read": 0,
        "socket_errors_write": 0,
        "socket_errors_timeout": 0,
        "non_2xx": 0,
        "error_summary": "0"
    }

    # 1. Requests/sec
    rps_match = re.search(r"Requests/sec:\s*([0-9.]+)", text)
    if rps_match:
        metrics["rps"] = float(rps_match.group(1))

    # 2. Total requests & duration
    req_match = re.search(r"([0-9]+)\s+requests in\s+([0-9.]+)([a-zA-Z]+)", text)
    if req_match:
        metrics["requests"] = int(req_match.group(1))
        dur_val = float(req_match.group(2))
        dur_unit = req_match.group(3).lower()
        if dur_unit == "s":
            metrics["duration_sec"] = dur_val
        elif dur_unit == "m":
            metrics["duration_sec"] = dur_val * 60.0
        elif dur_unit == "ms":
            metrics["duration_sec"] = dur_val / 1000.0

    # 3. Latency Percentiles from distribution table
    # Example format:
    #   50%    1.02ms
    #   75%    1.50ms
    #   90%    2.10ms
    #   99%    4.30ms
    p_matches = re.findall(r"^\s*([0-9.]+)%\s+([0-9.]+[a-zA-Z]+)", text, re.MULTILINE)
    for pct, val_str in p_matches:
        p_float = float(pct)
        ms_val = parse_time_to_ms(val_str)
        if abs(p_float - 50.0) < 0.1:
            metrics["p50_ms"] = ms_val
        elif abs(p_float - 75.0) < 0.1:
            metrics["p75_ms"] = ms_val
        elif abs(p_float - 90.0) < 0.1:
            metrics["p90_ms"] = ms_val
        elif abs(p_float - 99.0) < 0.1:
            metrics["p99_ms"] = ms_val
        elif abs(p_float - 99.9) < 0.1:
            metrics["p99_9_ms"] = ms_val

    # 4. Thread stats summary (Mean, Stdev, Max)
    # Thread Stats   Avg      Stdev     Max       +/- Stdev
    #   Latency     1.20ms    1.00ms   25.00ms    85.00%
    lat_summary = re.search(r"Latency\s+([0-9.]+[a-zA-Z]+)\s+([0-9.]+[a-zA-Z]+)\s+([0-9.]+[a-zA-Z]+)", text)
    if lat_summary:
        metrics["mean_lat_ms"] = parse_time_to_ms(lat_summary.group(1))
        metrics["std_lat_ms"] = parse_time_to_ms(lat_summary.group(2))
        metrics["max_lat_ms"] = parse_time_to_ms(lat_summary.group(3))

    # 5. Socket Errors
    # Socket errors: connect 0, read 1, write 0, timeout 0
    err_match = re.search(r"Socket errors:\s*connect\s+([0-9]+),\s*read\s+([0-9]+),\s*write\s+([0-9]+),\s*timeout\s+([0-9]+)", text)
    if err_match:
        c = int(err_match.group(1))
        r = int(err_match.group(2))
        w = int(err_match.group(3))
        t = int(err_match.group(4))
        metrics["socket_errors_connect"] = c
        metrics["socket_errors_read"] = r
        metrics["socket_errors_write"] = w
        metrics["socket_errors_timeout"] = t
        if (c + r + w + t) > 0:
            metrics["error_summary"] = f"connect {c}; read {r}; write {w}; timeout {t}"

    # 6. Non-2xx responses
    # Non-2xx or 3xx responses: 42
    non2xx_match = re.search(r"Non-2xx or 3xx responses:\s*([0-9]+)", text)
    if non2xx_match:
        metrics["non_2xx"] = int(non2xx_match.group(1))

    return metrics

if __name__ == "__main__":
    content = sys.stdin.read()
    res = parse_wrk_output(content)
    if "--json" in sys.argv:
        print(json.dumps(res, indent=2))
    else:
        # Output csv-friendly key=val format
        pairs = [f"{k}={v}" for k, v in res.items()]
        print(" ".join(pairs))
