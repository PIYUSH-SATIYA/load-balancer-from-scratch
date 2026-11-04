#!/usr/bin/env python3
"""Emit one JSON snapshot for the exact proxy PID and its cgroup."""
import argparse
import json
import re
import time
from pathlib import Path


def status_kb(status: str, name: str):
    match = re.search(rf"^{re.escape(name)}:\s+(\d+)", status, re.MULTILINE)
    return int(match.group(1)) if match else None


def cgroup_path(pid: int):
    text = Path(f"/proc/{pid}/cgroup").read_text()
    for line in text.splitlines():
        if line.startswith("0::"):
            return Path("/sys/fs/cgroup") / line[3:].lstrip("/")
    return None


def read_int(path: Path):
    try:
        value = path.read_text().strip()
        return int(value) if value != "max" else None
    except (OSError, ValueError):
        return None


def snapshot(pid: int, include_cgroup: bool):
    # Process metrics describe the exact proxy PID. Optional cgroup values add
    # container-wide memory/pid pressure; host-mode runs leave them unavailable.
    proc = Path(f"/proc/{pid}")
    status = (proc / "status").read_text()
    pss = status_kb((proc / "smaps_rollup").read_text(), "Pss")
    limits = (proc / "limits").read_text()
    nofile = re.search(r"^Max open files\s+(\d+)\s+(\d+)", limits, re.MULTILINE)
    cg = cgroup_path(pid) if include_cgroup else None
    try:
        fd_count = len(list((proc / "fd").iterdir()))
    except OSError:
        fd_count = None
    return {
        "pid": pid,
        "threads": status_kb(status, "Threads"),
        "rss_kb": status_kb(status, "VmRSS"),
        "pss_kb": pss,
        "vmsize_kb": status_kb(status, "VmSize"),
        "fd_count": fd_count,
        "nofile_soft": int(nofile.group(1)) if nofile else None,
        "nofile_hard": int(nofile.group(2)) if nofile else None,
        "cgroup_memory_bytes": read_int(cg / "memory.current") if cg else None,
        "cgroup_pids_current": read_int(cg / "pids.current") if cg else None,
        "cgroup_pids_max": read_int(cg / "pids.max") if cg else None,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pid", type=int)
    parser.add_argument("--include-cgroup", action="store_true")
    parser.add_argument("--watch", type=Path, help="sample until --stop-file appears, keeping the snapshot with most open FDs")
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--interval", type=float, default=0.05)
    args = parser.parse_args()
    if not args.watch:
        print(json.dumps(snapshot(args.pid, args.include_cgroup)))
        return
    if not args.stop_file:
        parser.error("--watch requires --stop-file")
    best = None
    while True:
        try:
            current = snapshot(args.pid, args.include_cgroup)
            if best is None or (current.get("fd_count") or 0) >= (best.get("fd_count") or 0):
                best = current
                args.watch.parent.mkdir(parents=True, exist_ok=True)
                temporary = args.watch.with_suffix(args.watch.suffix + ".tmp")
                temporary.write_text(json.dumps(best) + "\n")
                temporary.replace(args.watch)
        except OSError:
            pass
        if args.stop_file.exists():
            break
        time.sleep(max(args.interval, 0.01))
    if best is not None and not args.watch.exists():
        args.watch.parent.mkdir(parents=True, exist_ok=True)
        args.watch.write_text(json.dumps(best) + "\n")


if __name__ == "__main__":
    main()
