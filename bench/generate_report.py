#!/usr/bin/env python3
"""Build the benchmark report from the checked-in CSV/JSON run artifacts."""
import csv
import json
import statistics
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"


def fnum(value, digits=2):
    try:
        return f"{float(value):,.{digits}f}"
    except (ValueError, TypeError):
        return "—"


def summarize_rows(rows, key):
    values = [float(row[key]) for row in rows if row.get(key) not in (None, "")]
    if not values:
        return "—"
    if len(values) == 1:
        return fnum(values[0])
    return f"{fnum(statistics.mean(values))} ± {fnum(statistics.stdev(values))}"


def median_metric(rows, key, digits=1):
    values = []
    for row in rows:
        try:
            values.append(float(row[key]))
        except (KeyError, ValueError, TypeError):
            continue
    return fnum(statistics.median(values), digits) if values else "—"


def throughput_section():
    path = RESULTS / "summary.csv"
    if not path.exists():
        return "No throughput CSV is present."
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        return "No throughput rows were recorded."
    out = ["| Mode | Connections | Samples | Median RPS (range) | Median P50 (ms) | Median P90 (ms) | Median P99 (ms) | Pinned CPU set busy % | Voluntary CSW/s | Nonvoluntary CSW/s | Socket errors | Non-PASS runs |",
           "| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    groups = {}
    for row in rows:
        groups.setdefault((row["Mode"], int(row["Connections"])), []).append(row)
    for (mode, conns), group in sorted(groups.items()):
        rps = sorted(float(r["RPS"]) for r in group)
        errors = sum(int(r.get("SocketErrors") or 0) for r in group)
        failed = sum(r.get("Status") != "PASS" for r in group)
        median = statistics.median(rps)
        spread = f"{fnum(rps[0], 0)}–{fnum(rps[-1], 0)}"
        p50 = statistics.median(float(r["P50_ms"]) for r in group)
        p90 = statistics.median(float(r["P90_ms"]) for r in group)
        p99 = statistics.median(float(r["P99_ms"]) for r in group)
        core_busy = fnum(statistics.median(float(r["Core_Busy"]) for r in group), 1)
        out.append(f"| {mode} | {conns} | {len(group)} | {fnum(median, 0)} ({spread}) | {fnum(p50, 3)} | {fnum(p90, 3)} | {fnum(p99, 3)} | {core_busy} | {median_metric(group, 'CSW_per_s')} | {median_metric(group, 'NVCSW_per_s')} | {errors} | {failed} |")
    return "\n".join(out)


def idle_section():
    path = RESULTS / "idle_sweep_isolated.csv"
    if not path.exists():
        return "No idle-sweep CSV is present."
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        return "No idle-sweep rows were recorded."
    out = ["| Mode | Target | Established during hold | Handshake s | Connections/s | Threads | RSS MB | PSS MB | Cgroup MB | Active probe ms | Status |",
           "| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |"]
    for row in rows:
        out.append("| {Architecture} | {TargetConns} | {EstablishedConns} | {HandshakeSec} | {ConnRate} | {Threads} | {RSS_MB} | {PSS_MB} | {Cgroup_MB} | {ActiveLatency_ms} | {Status} |".format(**row))
    return "\n".join(out)


def throughput_metadata():
    path = RESULTS / "benchmark_manifest.json"
    if not path.exists():
        return "Run settings are not recorded."
    data = json.loads(path.read_text())
    wrk_version = data.get("wrk_version", "unknown").splitlines()[0]
    return (f"Settings: {data['repetitions']} repetitions; {data['warmup_seconds']} s warmup; "
            f"{data['measurement_seconds']} s measured; concurrency {', '.join(data['concurrency_levels'])}; "
            f"proxy CPUs `{data['proxy_single_cpu_set']}` / `{data['proxy_multi_cpu_set']}`; "
            f"load-generator CPUs `{data['load_generator_cpu_set']}`; backend CPUs `{data.get('backend_cpu_set', 'unknown')}`; "
            f"Nginx CPUs `{data.get('nginx_cpu_set', 'unknown')}`. Platform: `{data['platform']}`; "
            f"kernel `{data.get('kernel_release', 'unknown')}`; CPUs `{data.get('cpu_count', 'unknown')}`; "
            f"wrk `{wrk_version}`.")


def failover_section():
    path = RESULTS / "failover_trials.json"
    if not path.exists():
        return "No failover trial JSON is present."
    data = json.loads(path.read_text())
    out = ["| Mode | Trials | Mean dropped probes (SD) | Drop range | Median last failed probe after kill (s) | Median first probe to recovered backend after restart (s) | Trials with pre-restore failures | Trials with observed re-entry |",
           "| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for mode, trials in data["modes"].items():
        drops = [row["dropped_after_kill"] for row in trials]
        windows = [row["last_failure_before_restore_seconds"] for row in trials
                   if row["last_failure_before_restore_seconds"] is not None]
        recovery = []
        for row in trials:
            delay = row.get("first_success_to_restored_backend_seconds")
            if delay is None:
                restore_at = float(row["outage_seconds"])
                returned = [probe["sent_at"] - restore_at for probe in row.get("probe_results", [])
                            if probe["success"] and probe["backend_port"] == 9002
                            and probe["sent_at"] >= restore_at]
                delay = min(returned) if returned else None
            if delay is not None:
                recovery.append(delay)
        spread = f"{min(drops)}–{max(drops)}"
        latest_failure = fnum(statistics.median(windows), 3) if windows else "not observed"
        reentry = fnum(statistics.median(recovery), 3) if recovery else "not observed"
        out.append(f"| {mode} | {len(trials)} | {summarize_rows([{'drops': x} for x in drops], 'drops')} | {spread} | {latest_failure} | {reentry} | {len(windows)} | {len(recovery)} |")
    return "\n".join(out)


def algorithm_section():
    path = RESULTS / "algorithm_trials.json"
    if not path.exists():
        return "No algorithm trial JSON is present."
    data = json.loads(path.read_text())
    trials = data["trials"]
    rr = trials["round_robin"]
    lc = trials["least_connections"]
    ch = trials["consistent_hashing"]
    lc_balanced = all(
        max((int(v) for v in t["added_counts"].values()), default=0)
        - min((int(v) for v in t["added_counts"].values()), default=0) <= 1
        and int(t["added_counts"].get("9001", t["added_counts"].get(9001, 0))) <= 2
        for t in lc
    )
    out = ["| Algorithm | Trials | Observed outcome |",
           "| :--- | ---: | :--- |",
           f"| Round robin | {len(rr)} | 30 new connections split 10/10/10: {all(sorted(t['counts'].values()) == [10, 10, 10] for t in rr)} |",
           f"| Least connections | {len(lc)} | New held sessions balanced around two existing sessions: {lc_balanced} |",
           f"| Consistent hashing | {len(ch)} | Same source IP retained its backend over reconnects: {all(len(t['mapping']) == 30 for t in ch)} |"]
    return "\n".join(out)


def half_close_section():
    path = RESULTS / "half_close.json"
    if not path.exists():
        return "No half-close verification JSON is present."
    data = json.loads(path.read_text())
    out = ["| Proxy mode | Request bytes preserved | Response bytes drained | Hash matched | Result |",
           "| :--- | ---: | ---: | :---: | :---: |"]
    for trial in data["trials"]:
        out.append(f"| {trial['mode']} | {trial['request_bytes']:,} | {trial['response_bytes']:,} | {trial['response_sha256_matches']} | {'PASS' if trial['passed'] else 'FAIL'} |")
    return "\n".join(out)


def main():
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    report = f"""# Benchmark report

**Generated:** {generated}<br>
**Scope:** educational comparison of a small TCP proxy’s blocking and `epoll` implementations. These measurements do not claim production readiness or feature parity with NGINX/HAProxy.

## How to read this report

Every table below is generated from the current files in `results/`. The CSV and JSON files are the raw trial records. A row marked `PARTIAL` or `FAILED` is not a successful target run. Missing artifacts mean that suite has not been run; no estimate is substituted.

`wrk` measures HTTP request throughput over the TCP proxy. This is a short loopback microbenchmark using the included small HTTP responder, not a general proxy-performance result or production capacity test. It has no long soak, overload recovery, or process-level CPU/memory profile. `wrk 4.2.0 --latency` emits P50, P75, P90, and P99 in this setup; it does not emit P95 or P99.9. Runs with different environment or concurrency settings should be treated as separate experiments. “Pinned CPU set busy” is host CPU time for the selected CPU IDs, not CPU time attributed to the proxy process.

{throughput_metadata()}

## Active throughput and latency

`bench/run_bench.sh` records one row per mode, concurrency, and repetition. The table reports medians for RPS, latency, pinned CPU set use, and context switches; ranges show RPS spread. Per-thread context switches are sampled during each measurement and summed for thread IDs observed in that interval. The harness records failures and raw `wrk` output under `results/raw/`.

{throughput_section()}

## Idle connection capacity

`bench/run_idle_test.sh` samples proxy process memory while the client still holds the established connections. `Established` is the number actually opened, not the requested target. Container cgroup memory includes the load generator as well as the proxy. `PASS` requires reaching the target and a successful active probe; `PARTIAL` means the target was not reached while the proxy remained responsive; `FAILED` means the proxy was absent or the active probe failed.

{idle_section()}

## Backend failure and recovery

`bench/test_failover.py` kills and restarts backend 2 during a scheduled stream of requests. `Dropped` counts failed probes after the kill, including recovery. The latest failed probe time is shown only for trials with a failure before backend restart; the first successful request routed back to backend 2 is measured after restart. These are request-observation times, not direct health-check timer readings. The JSON contains each probe timestamp and outcome.

{failover_section()}

## Routing algorithm checks

`bench/test_algorithms.py` records backend identity in the responder header and asserts the observed properties. These are behavior checks, not statistical proof of universal distribution quality.

{algorithm_section()}

## TCP half-close and buffered response check

`bench/test_half_close.py` sends a 1 MiB request, shuts down only the client write side, and verifies that the proxy drains the complete backend response before closing. This checks byte preservation for this test case; it is not a broad protocol-conformance suite.

{half_close_section()}

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
"""
    (ROOT / "docs/benchmark_report.md").write_text(report)
    (RESULTS / "parsed_report.md").write_text(
        "# Parsed throughput summary\n\n"
        "Generated from the current `summary.csv`; see [`docs/benchmark_report.md`](../docs/benchmark_report.md) "
        "for methodology and all suites.\n\n" + throughput_section() + "\n"
    )
    print("Wrote docs/benchmark_report.md and results/parsed_report.md from current run artifacts")


if __name__ == "__main__":
    main()
