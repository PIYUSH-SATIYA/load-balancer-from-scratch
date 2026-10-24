#!/bin/bash
# ==============================================================================
# C10K / C30K Idle Scalability & Memory Footprint Sweep
# Measures:
#   - Connection establishment rate
#   - Process RSS, PSS (Proportional Set Size), and Cgroup v2 memory
#   - Thread counts and OS limits
#   - Active request probe latency under massive idle load
# Uses event-driven handshake synchronization and samples while clients hold
# their sockets open. The client rotates loopback source IPs above 15k sockets.
# ==============================================================================
set -euo pipefail
ulimit -n 65536 2>/dev/null || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results"
mkdir -p "$RESULTS_DIR"

OUT_CSV="$RESULTS_DIR/idle_sweep_isolated.csv"
echo "Architecture,TargetConns,EstablishedConns,HandshakeSec,ConnRate,Threads,RSS_MB,PSS_MB,Cgroup_MB,ActiveLatency_ms,Status" > "$OUT_CSV"

HOLD_SEC="${HOLD_SEC:-30}"
PIN_PROXY="${PIN_PROXY:-0}"
TARGETS=(${TARGETS_OVERRIDE:-10000 20000 30000})
HOLD_SEC="$HOLD_SEC" PIN_PROXY="$PIN_PROXY" TARGETS_LIST="${TARGETS[*]}" python3 - "$RESULTS_DIR/idle_manifest.json" <<'PY'
import json, platform, sys
import os
with open(sys.argv[1], "w") as output:
    json.dump({"hold_seconds": int(os.environ["HOLD_SEC"]), "targets": [int(x) for x in os.environ["TARGETS_LIST"].split()],
               "proxy_cpu": os.environ["PIN_PROXY"], "platform": platform.platform()}, output, indent=2)
    output.write("\n")
PY

get_lb_stats() {
    # Check if loadbalancer is running in docker or locally
    local in_docker
    in_docker=$(docker compose ps -q loadbalancer 2>/dev/null || echo "")

    if [ -n "$in_docker" ] && docker compose exec -T loadbalancer true 2>/dev/null; then
        docker compose exec -T loadbalancer bash -c '
        pid=$(pidof loadbalancer 2>/dev/null || echo "")
        if [ -z "$pid" ]; then
            echo "0 0 0 0"
            exit 0
        fi
        threads=$(awk "/^Threads:/ {print \$2}" /proc/$pid/status 2>/dev/null || echo 0)
        rss_kb=$(awk "/^VmRSS:/ {print \$2}" /proc/$pid/status 2>/dev/null || echo 0)
        pss_kb=$(awk "/^Pss:/ {sum += \$2} END {print sum+0}" /proc/$pid/smaps_rollup 2>/dev/null || echo $rss_kb)
        cg_bytes=$(cat /sys/fs/cgroup/memory.current 2>/dev/null || echo 0)
        
        rss_mb=$(awk -v k="$rss_kb" "BEGIN {printf \"%.2f\", k/1024}")
        pss_mb=$(awk -v k="$pss_kb" "BEGIN {printf \"%.2f\", k/1024}")
        cg_mb=$(awk -v b="$cg_bytes" "BEGIN {printf \"%.2f\", b/(1024*1024)}")
        
        echo "$threads $rss_mb $pss_mb $cg_mb"
        '
    else
        # Host profiling
        local pid
        pid=$(pidof loadbalancer 2>/dev/null || echo "")
        if [ -z "$pid" ]; then
            echo "0 0 0 0"
            return
        fi
        local threads=$(awk '/^Threads:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
        local rss_kb=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
        local pss_kb=$(awk '/^Pss:/ {sum += $2} END {print sum+0}' "/proc/$pid/smaps_rollup" 2>/dev/null || echo $rss_kb)
        local rss_mb=$(awk -v k="$rss_kb" 'BEGIN {printf "%.2f", k/1024}')
        local pss_mb=$(awk -v k="$pss_kb" 'BEGIN {printf "%.2f", k/1024}')
        echo "$threads $rss_mb $pss_mb 0.0"
    fi
}

stop_proxy() {
    if docker compose ps --status running -q loadbalancer 2>/dev/null | grep -q .; then
        docker compose exec -T loadbalancer pkill -x loadbalancer 2>/dev/null || true
    elif [ -n "${HOST_PROXY_PID:-}" ]; then
        kill -TERM "$HOST_PROXY_PID" 2>/dev/null || true
        wait "$HOST_PROXY_PID" 2>/dev/null || true
        HOST_PROXY_PID=""
    fi
}

run_idle_cell() {
    local arch=$1
    local is_epoll=$2
    local target=$3
    local in_container="false"

    echo ""
    echo "========================================================================"
    echo " IDLE TEST: $arch | Target Connections: $target | Hold: ${HOLD_SEC}s"
    echo "========================================================================"

    # 1. Stop any existing load balancer
    stop_proxy
    sleep 1

    # 2. Launch proxy under taskset Core 0
    if docker compose ps --status running -q loadbalancer 2>/dev/null | grep -q .; then
        in_container="true"
        if [ "$is_epoll" = "true" ]; then
            docker compose exec -d loadbalancer taskset -c "$PIN_PROXY" /app/build/loadbalancer -e -p 8080
        else
            docker compose exec -d loadbalancer taskset -c "$PIN_PROXY" /app/build/loadbalancer -p 8080
        fi
    else
        local lb_bin="$PROJECT_ROOT/build/loadbalancer"
        [ ! -x "$lb_bin" ] && lb_bin="$PROJECT_ROOT/build-host/loadbalancer"
        if [ "$is_epoll" = "true" ]; then
            taskset -c "$PIN_PROXY" "$lb_bin" -e -p 8080 > /dev/null 2>&1 &
            HOST_PROXY_PID=$!
        else
            taskset -c "$PIN_PROXY" "$lb_bin" -p 8080 > /dev/null 2>&1 &
            HOST_PROXY_PID=$!
        fi
    fi
    sleep 1.5

    local init_stats=($(get_lb_stats))
    echo "Initial State: Threads=${init_stats[0]} | RSS=${init_stats[1]}MB | PSS=${init_stats[2]}MB | Cgroup=${init_stats[3]}MB"

    local log1="/tmp/idle_client_${arch}_${target}_1.log"
    local log2="/tmp/idle_client_${arch}_${target}_2.log"
    rm -f "$log1" "$log2"

    echo "Spawning client targeting loopback:8080 ($target connections)..."
    if [ "$in_container" = "true" ]; then
        docker compose exec -T loadbalancer python3 /app/bench/idle_client.py "$target" 127.0.0.1 8080 "$HOLD_SEC" > "$log1" 2>&1 &
    else
        python3 "$SCRIPT_DIR/idle_client.py" "$target" 127.0.0.1 8080 "$HOLD_SEC" > "$log1" 2>&1 &
    fi
    local client_bg=$!

    # Wait for handshake completion
    echo "Waiting for connection handshakes to complete..."
    local wait_sec=0
    while true; do
        if grep -q "HOLD_START" "$log1" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$client_bg" 2>/dev/null; then
            echo "Client exited before reaching the hold phase"
            break
        fi
        sleep 1
        wait_sec=$((wait_sec + 1))
        if [ $wait_sec -gt 150 ]; then
            echo "Warning: Timeout waiting for handshake"
            break
        fi
    done
    # Sample resources while the client process still holds its connections.
    local peak_stats=($(get_lb_stats))
    echo "Under-Load   : Threads=${peak_stats[0]} | RSS=${peak_stats[1]}MB | PSS=${peak_stats[2]}MB | Cgroup=${peak_stats[3]}MB"

    # Parse metrics from logs
    local est1=$(grep "HOLD_START" "$log1" 2>/dev/null | sed -n 's/.*Established=\([0-9]*\).*/\1/p' || echo "0")
    local est2=$(grep "FINAL_METRICS" "$log2" 2>/dev/null | sed -n 's/.*Established=\([0-9]*\).*/\1/p' || echo "0")
    local est=$(( ${est1:-0} + ${est2:-0} ))

    local dur=$(grep "HOLD_START" "$log1" 2>/dev/null | sed -n 's/.*HandshakeSec=\([0-9.]*\).*/\1/p' || echo "0.0")
    local rate=$(awk -v e="$est" -v d="${dur:-1}" 'BEGIN {if (d>0) printf "%.0f", e/d; else print "0"}')
    local lat=$(grep "HOLD_START" "$log1" 2>/dev/null | sed -n 's/.*ActiveLatMs=\([0-9.]*\).*/\1/p' || echo "0.0")

    local status="PASS"
    if [ "$est" -lt "$target" ]; then
        status="PARTIAL"
    fi
    if [ "${peak_stats[0]}" -eq 0 ] || awk -v latency="${lat:-0}" 'BEGIN {exit !(latency <= 0)}'; then
        status="FAILED"
    fi

    echo "Result Summary: Est=$est/$target | Handshake=${dur}s (${rate} conn/s) | Threads=${peak_stats[0]} | RSS=${peak_stats[1]}MB | PSS=${peak_stats[2]}MB | Cgroup=${peak_stats[3]}MB | ActiveLat=${lat}ms | Status=$status"
    echo "$arch,$target,$est,$dur,$rate,${peak_stats[0]},${peak_stats[1]},${peak_stats[2]},${peak_stats[3]},$lat,$status" >> "$OUT_CSV"

    wait "$client_bg" || true

    stop_proxy
    sleep 1
}

main() {
    echo "Starting isolated idle connection evaluation..."
    for target in "${TARGETS[@]}"; do
        run_idle_cell "Epoll_Optimized" "true" "$target"
        run_idle_cell "Threaded_Proxy" "false" "$target"
    done

    echo ""
    echo "========================================================================"
    echo " Idle Sweep Complete! Results saved to $OUT_CSV"
    echo "========================================================================"
}

main "$@"
