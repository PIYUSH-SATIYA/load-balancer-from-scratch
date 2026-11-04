#!/usr/bin/env bash
# Measure idle proxied connections while keeping the load generator outside the
# proxy container and sampling the exact proxy PID.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="${RESULTS_DIR:-$PROJECT_ROOT/results}"
OUT_CSV="$RESULTS_DIR/idle_sweep_isolated.csv"
# HOLD_SEC keeps established sessions open while sampling. Separate target
# overrides let us compare architectures at sensible resource limits.
HOLD_SEC="${HOLD_SEC:-5}"
PIN_PROXY="${PIN_PROXY:-0}"
TARGETS=(${TARGETS_OVERRIDE:-10000 20000 30000})
if [ "${EPOLL_TARGETS_OVERRIDE+x}" ]; then read -r -a EPOLL_TARGETS <<< "$EPOLL_TARGETS_OVERRIDE"; else EPOLL_TARGETS=("${TARGETS[@]}"); fi
if [ "${THREADED_TARGETS_OVERRIDE+x}" ]; then read -r -a THREADED_TARGETS <<< "$THREADED_TARGETS_OVERRIDE"; else THREADED_TARGETS=("${TARGETS[@]}"); fi
LB_BIN="${LB_BIN:-$PROJECT_ROOT/build/loadbalancer}"
BACKEND_BIN="${BACKEND_BIN:-$PROJECT_ROOT/build/dummy_backend}"
[ -x "$LB_BIN" ] || LB_BIN="$PROJECT_ROOT/build-host/loadbalancer"
[ -x "$BACKEND_BIN" ] || BACKEND_BIN="$PROJECT_ROOT/build-host/dummy_backend"
DOCKER_MODE=false
HOST_PROXY_PID=""
BACKEND_PIDS=()
CELL_INDEX=0
ACTIVE_MONITOR_PID=""
ACTIVE_CLIENT_PID=""
ACTIVE_STOP_FILE=""

mkdir -p "$RESULTS_DIR"
if docker compose ps --status running -q loadbalancer 2>/dev/null | grep -q .; then
    # Container mode can report cgroup memory and pid limits in addition to
    # process metrics. The client still runs on the host in both modes.
    DOCKER_MODE=true
fi

python3 - "$RESULTS_DIR/idle_manifest.json" "$HOLD_SEC" "${EPOLL_TARGETS[*]}" "${THREADED_TARGETS[*]}" "$PIN_PROXY" "$DOCKER_MODE" <<'PY'
import json, platform, sys
from pathlib import Path
path, hold, epoll_targets, threaded_targets, pin, docker_mode = sys.argv[1:]
Path(path).write_text(json.dumps({
    "hold_seconds": int(hold), "epoll_targets": [int(x) for x in epoll_targets.split()],
    "threaded_targets": [int(x) for x in threaded_targets.split()],
    "proxy_cpu": pin, "platform": platform.platform(),
    "proxy_container": docker_mode == "true",
    "client_location": "host",
    "source_ips_rotated_per_cell": True,
    "metrics": ["process RSS", "process PSS", "process VmSize", "process threads",
                "process file descriptors", "proxy cgroup memory and pids"]
}, indent=2) + "\n")
PY

cat > "$OUT_CSV" <<'CSV'
Architecture,TargetConns,EstablishedConns,HandshakeSec,ConnRate,ThreadsBaseline,ThreadsLoad,RSSBaselineMB,RSSLoadMB,RSSDeltaMB,PSSBaselineMB,PSSLoadMB,PSSDeltaMB,VmSizeBaselineMB,VmSizeLoadMB,FdsBaseline,FdsLoad,NofileSoft,NofileHard,CgroupBaselineMB,CgroupLoadMB,CgroupDeltaMB,CgroupPidsCurrent,CgroupPidsMax,ActiveLatencyP50ms,ClientFailures,SamplerError,Status,SourceIPBase
CSV

port_open() {
    python3 - "$1" <<'PY'
import socket, sys
try:
    with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.15): pass
except OSError: raise SystemExit(1)
PY
}

start_local_backends() {
    for port in 9001 9002 9003; do
        if ! port_open "$port"; then
            "$BACKEND_BIN" "$port" >"/tmp/lb_idle_backend_${port}.log" 2>&1 &
            BACKEND_PIDS+=("$!")
            for _ in $(seq 1 50); do
                port_open "$port" && break
                sleep 0.1
            done
            if ! port_open "$port"; then
                echo "Backend on port $port did not become ready" >&2
                return 1
            fi
        fi
    done
}

stop_proxy() {
    if [ "$DOCKER_MODE" = true ]; then
        local pid
        pid=$(docker compose exec -T loadbalancer sh -c 'cat /tmp/lb_idle_proxy.pid 2>/dev/null' 2>/dev/null || true)
        if [[ "$pid" =~ ^[0-9]+$ ]]; then
            docker compose exec -T loadbalancer kill -TERM "$pid" 2>/dev/null || true
            for _ in $(seq 1 20); do
                docker compose exec -T loadbalancer kill -0 "$pid" 2>/dev/null || break
                sleep 0.1
            done
            docker compose exec -T loadbalancer rm -f /tmp/lb_idle_proxy.pid 2>/dev/null || true
        fi
    elif [ -n "$HOST_PROXY_PID" ]; then
        kill -TERM "$HOST_PROXY_PID" 2>/dev/null || true
        wait "$HOST_PROXY_PID" 2>/dev/null || true
        HOST_PROXY_PID=""
    fi
}

cleanup() {
    if [ -n "$ACTIVE_STOP_FILE" ]; then : > "$ACTIVE_STOP_FILE" 2>/dev/null || true; fi
    if [ -n "$ACTIVE_CLIENT_PID" ]; then
        kill -TERM "$ACTIVE_CLIENT_PID" 2>/dev/null || true
        wait "$ACTIVE_CLIENT_PID" 2>/dev/null || true
    fi
    if [ -n "$ACTIVE_MONITOR_PID" ]; then
        kill -TERM "$ACTIVE_MONITOR_PID" 2>/dev/null || true
        wait "$ACTIVE_MONITOR_PID" 2>/dev/null || true
    fi
    stop_proxy
    for pid in "${BACKEND_PIDS[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

proxy_snapshot() {
    local pid="$1"
    if [ "$DOCKER_MODE" = true ]; then
        docker compose exec -T loadbalancer python3 /app/bench/lib/sample_proc_metrics.py "$pid" --include-cgroup
    else
        python3 "$SCRIPT_DIR/lib/sample_proc_metrics.py" "$pid"
    fi
}

metric() {
    python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get(sys.argv[2]) if json.loads(sys.argv[1]).get(sys.argv[2]) is not None else "")' "$1" "$2"
}

mb_kb() {
    awk -v b="${1:-}" 'BEGIN {if (b == "") print ""; else printf "%.2f", b/1024}'
}

mb_bytes() {
    awk -v b="${1:-}" 'BEGIN {if (b == "") print ""; else printf "%.2f", b/1048576}'
}

append_row() {
    local mode="$1" target="$2" established="$3" duration="$4" rate="$5"
    local base_json="$6" load_json="$7" latency="$8" failures="$9" status="${10}" source_ip="${11}" sampler_error="${12}"
    local b_rss l_rss b_pss l_pss b_vms l_vms b_cg l_cg
    b_rss=$(metric "$base_json" rss_kb); l_rss=$(metric "$load_json" rss_kb)
    b_pss=$(metric "$base_json" pss_kb); l_pss=$(metric "$load_json" pss_kb)
    b_vms=$(metric "$base_json" vmsize_kb); l_vms=$(metric "$load_json" vmsize_kb)
    b_cg=$(metric "$base_json" cgroup_memory_bytes); l_cg=$(metric "$load_json" cgroup_memory_bytes)
    local values=()
    values+=("$mode" "$target" "$established" "$duration" "$rate")
    values+=("$(metric "$base_json" threads)" "$(metric "$load_json" threads)")
    values+=("$(mb_kb "$b_rss")" "$(mb_kb "$l_rss")")
    values+=("$(awk -v a="$b_rss" -v b="$l_rss" 'BEGIN {if (a=="" || b=="") print ""; else printf "%.2f", (b-a)/1024}')")
    values+=("$(mb_kb "$b_pss")" "$(mb_kb "$l_pss")")
    values+=("$(awk -v a="$b_pss" -v b="$l_pss" 'BEGIN {if (a=="" || b=="") print ""; else printf "%.2f", (b-a)/1024}')")
    values+=("$(mb_kb "$b_vms")" "$(mb_kb "$l_vms")")
    values+=("$(metric "$base_json" fd_count)" "$(metric "$load_json" fd_count)")
    values+=("$(metric "$load_json" nofile_soft)" "$(metric "$load_json" nofile_hard)")
    values+=("$(mb_bytes "$b_cg")" "$(mb_bytes "$l_cg")")
    values+=("$(awk -v a="$b_cg" -v b="$l_cg" 'BEGIN {if (a=="" || b=="") print ""; else printf "%.2f", (b-a)/1048576}')")
    values+=("$(metric "$load_json" cgroup_pids_current)" "$(metric "$load_json" cgroup_pids_max)" "$latency" "$failures" "$sampler_error" "$status" "$source_ip")
    python3 - "$OUT_CSV" "${values[@]}" <<'PY'
import csv, sys
with open(sys.argv[1], "a", newline="") as stream:
    csv.writer(stream, lineterminator="\n").writerow(sys.argv[2:])
PY
}

run_cell() {
    local mode="$1" epoll="$2" target="$3"
    CELL_INDEX=$((CELL_INDEX + 1))
    # Use a fresh loopback source address for each cell to reduce client-side
    # ephemeral-port/TIME_WAIT collisions in these large sequential sweeps.
    local slot=$(( ( $(date +%s) + CELL_INDEX ) % 240 + 1 ))
    local source_ip="127.0.${slot}.1"
    local proxy_pid="" mode_flags="" monitor_pid=""
    local snapshot_name=".idle_snapshot_${mode}_${target}.json"
    local stop_name=".idle_snapshot_${mode}_${target}.stop"
    local snapshot_file="$RESULTS_DIR/$snapshot_name"
    local stop_file="$RESULTS_DIR/$stop_name"
    ACTIVE_STOP_FILE="$stop_file"

    echo "Idle test: $mode target=$target hold=${HOLD_SEC}s source=$source_ip"
    stop_proxy
    if [ "$DOCKER_MODE" = true ]; then
        if [ "$epoll" = true ]; then mode_flags="-e"; fi
        local command="taskset -c $PIN_PROXY /app/build/loadbalancer $mode_flags -p 8080 -b backend1:9001 -b backend2:9002 -b backend3:9003 >/tmp/lb_idle_proxy.log 2>&1 & echo \$! >/tmp/lb_idle_proxy.pid"
        docker compose exec -T loadbalancer sh -c "$command"
        proxy_pid=$(docker compose exec -T loadbalancer sh -c 'cat /tmp/lb_idle_proxy.pid')
    else
        if [ "$epoll" = true ]; then mode_flags="-e"; fi
        taskset -c "$PIN_PROXY" "$LB_BIN" $mode_flags -p 8080 \
            -b 127.0.0.1:9001 -b 127.0.0.1:9002 -b 127.0.0.1:9003 \
            >"/tmp/lb_idle_proxy.log" 2>&1 &
        HOST_PROXY_PID=$!
        proxy_pid="$HOST_PROXY_PID"
    fi

    local ready=false
    for _ in $(seq 1 50); do
        if port_open 8080; then ready=true; break; fi
        sleep 0.1
    done
    if [ "$ready" != true ]; then
        echo "Proxy failed readiness; see /tmp/lb_idle_proxy.log" >&2
        return 1
    fi

    # Capture process baseline before adding thousands of sessions; deltas make
    # the connection-related footprint comparable across architectures.
    local baseline
    baseline=$(proxy_snapshot "$proxy_pid")
    rm -f "$snapshot_file" "$stop_file"
    # Start sampling before dialing. At high thread counts the OS may refuse a
    # new process, which would make a peak-only sampler disappear exactly when
    # its measurements matter most.
    if [ "$DOCKER_MODE" = true ]; then
        docker compose exec -d loadbalancer python3 /app/bench/lib/sample_proc_metrics.py "$proxy_pid" \
            --include-cgroup --watch "/app/results/$snapshot_name" --stop-file "/app/results/$stop_name"
    else
        python3 "$SCRIPT_DIR/lib/sample_proc_metrics.py" "$proxy_pid" \
            --watch "$snapshot_file" --stop-file "$stop_file" &
        monitor_pid=$!
        ACTIVE_MONITOR_PID="$monitor_pid"
    fi
    for _ in $(seq 1 50); do
        [ -s "$snapshot_file" ] && break
        sleep 0.05
    done
    local log="/tmp/lb_idle_client_${mode}_${target}.log"
    local marker="/tmp/lb_idle_client_${mode}_${target}.hold"
    rm -f "$log"
    rm -f "$marker"
    # Client runs on the host even when the proxy is in Docker. This keeps it
    # out of the proxy container's cgroup memory measurement.
    python3 "$SCRIPT_DIR/idle_client.py" "$target" 127.0.0.1 8080 "$HOLD_SEC" "$source_ip" "$marker" >"$log" 2>&1 &
    local client_pid=$!
    ACTIVE_CLIENT_PID="$client_pid"
    coproc IDLE_DELAY { read -r -t 3600 _; }
    local delay_pid="$IDLE_DELAY_PID" delay_fd="${IDLE_DELAY[0]}"
    local wait_count=0
    while [ ! -e "$marker" ]; do
        if ! kill -0 "$client_pid" 2>/dev/null; then break; fi
        read -r -t 0.1 _ <&"$delay_fd" || true
        wait_count=$((wait_count + 1))
        if [ "$wait_count" -gt 1800 ]; then
            echo "Timed out waiting for idle client handshake" >&2
            kill -TERM "$client_pid" 2>/dev/null || true
            break
        fi
    done
    kill "$delay_pid" 2>/dev/null || true
    wait "$delay_pid" 2>/dev/null || true
    # Stop the prestarted sampler while all intended connections are held. It
    # samples throughout the handshake, so this path still works if creating a
    # new process at peak thread count would fail.
    : > "$stop_file"
    if [ -n "$monitor_pid" ]; then wait "$monitor_pid" 2>/dev/null || true; fi
    ACTIVE_MONITOR_PID=""
    ACTIVE_STOP_FILE=""
    local loaded='{}' sampler_error=""
    if [ -s "$snapshot_file" ]; then loaded=$(<"$snapshot_file"); fi
    if [ "$loaded" = "{}" ]; then sampler_error="snapshot unavailable (process/resource limit)"; fi
    wait "$client_pid" || true
    ACTIVE_CLIENT_PID=""
    stop_proxy

    local established duration rate latency failures
    read -r established duration rate latency failures < <(python3 - "$log" <<'PY'
import ast, json, re, sys
text = open(sys.argv[1], errors="replace").read()
handshake = re.search(r"HANDSHAKE_COMPLETE Established=(\d+)/(\d+) DurationSec=([0-9.]+) ConnRate=([0-9.]+) Failures=(\{.*\})", text)
hold = re.search(r"HOLD_START .*?ActiveLatMs=([0-9.]+)", text)
if handshake:
    count, target, duration, rate, errors = handshake.groups()
    failures = json.dumps(ast.literal_eval(errors), sort_keys=True, separators=(",", ":"))
else:
    count, duration, rate, failures = "0", "0", "0", "unknown"
latency = hold.group(1) if hold else "0"
print(count, duration, rate, latency, failures)
PY
    )
    local status="PASS"
    # Reaching the target is necessary but not sufficient: an active probe and
    # a valid process snapshot are required to call a row successful.
    if [ "$established" -lt "$target" ]; then status="PARTIAL"; fi
    if ! awk -v latency="$latency" 'BEGIN {exit !(latency + 0 > 0)}' || [ "$loaded" = "{}" ]; then status="FAILED"; fi
    append_row "$mode" "$target" "$established" "$duration" "$rate" "$baseline" "$loaded" "$latency" "$failures" "$status" "$source_ip" "$sampler_error"
    echo "  established=$established/$target rate=${rate}/s proxy_threads=$(metric "$loaded" threads) proxy_RSS=$(mb_kb "$(metric "$loaded" rss_kb)") MB PSS=$(mb_kb "$(metric "$loaded" pss_kb)") MB FDs=$(metric "$loaded" fd_count) probe=${latency}ms status=$status"
    rm -f "$snapshot_file" "$stop_file"
}

if [ "$DOCKER_MODE" != true ]; then
    [ -x "$LB_BIN" ] || { echo "loadbalancer binary not found" >&2; exit 1; }
    [ -x "$BACKEND_BIN" ] || { echo "dummy_backend binary not found" >&2; exit 1; }
    start_local_backends
fi

for target in "${EPOLL_TARGETS[@]}"; do
    run_cell Epoll_Optimized true "$target"
done
for target in "${THREADED_TARGETS[@]}"; do
    run_cell Threaded_Proxy false "$target"
done

echo "Wrote idle connection results to $OUT_CSV"
