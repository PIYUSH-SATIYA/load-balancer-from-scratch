#!/usr/bin/env python3
"""Sample per-thread /proc context-switch counters during one wrk interval."""
import json
import signal
import sys
import time
from pathlib import Path


stopping = False


def stop(_signum, _frame):
    global stopping
    stopping = True


def snapshot(pid: int) -> dict[int, tuple[int, int]]:
    # Read each proxy thread's counters, not the wrk process's counters, so
    # scheduler activity can be compared for the service implementation.
    result = {}
    task_root = Path(f"/proc/{pid}/task")
    try:
        tids = list(task_root.iterdir())
    except OSError:
        return result
    for task in tids:
        try:
            voluntary = nonvoluntary = 0
            with (task / "status").open() as status:
                for line in status:
                    if line.startswith("voluntary_ctxt_switches:"):
                        voluntary = int(line.split()[1])
                    elif line.startswith("nonvoluntary_ctxt_switches:"):
                        nonvoluntary = int(line.split()[1])
            result[int(task.name)] = (voluntary, nonvoluntary)
        except (OSError, ValueError, IndexError):
            # The thread can exit between listing /task and opening status.
            continue
    return result


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: sample_proc_csw.py PID OUTPUT.json")
    pid = int(sys.argv[1])
    output = Path(sys.argv[2])
    interval = 0.1
    first: dict[int, tuple[int, int]] = {}
    last: dict[int, tuple[int, int]] = {}
    sample_count = 0
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    while not stopping:
        current = snapshot(pid)
        if not current:
            break
        sample_count += 1
        for tid, counters in current.items():
            first.setdefault(tid, counters)
            last[tid] = counters
        time.sleep(interval)

    current = snapshot(pid)
    for tid, counters in current.items():
        first.setdefault(tid, counters)
        last[tid] = counters
    common = first.keys() & last.keys()
    voluntary = sum(max(0, last[tid][0] - first[tid][0]) for tid in common)
    nonvoluntary = sum(max(0, last[tid][1] - first[tid][1]) for tid in common)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({
        "pid": pid,
        "interval_seconds": interval,
        "samples": sample_count,
        "threads_sampled": len(common),
        "voluntary_context_switches": voluntary,
        "nonvoluntary_context_switches": nonvoluntary,
    }, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
