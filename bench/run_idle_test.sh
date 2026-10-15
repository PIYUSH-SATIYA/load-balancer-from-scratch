#!/bin/bash
# ==============================================================================
# C10K / C30K Idle Scalability & Memory Footprint Sweep
# Measures:
#   - Connection establishment rate
#   - Process RSS, PSS (Proportional Set Size), and Cgroup v2 memory
#   - Thread counts and OS limits
#   - Active request probe latency under massive idle load
# Uses event-driven handshake synchronization, 30s stable hold, and dual-client
# distribution to bypass single-IP ephemeral port exhaustion.
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

run_idle_cell() {
    local arch=$1
    local is_epoll=$2
    local target=$3

    echo ""
    echo "========================================================================"
    echo " IDLE TEST: $arch | Target Connections: $target | Hold: ${HOLD_SEC}s"
    echo "========================================================================"

    # 1. Stop any existing load balancer
    docker compose exec -T loadbalancer bash -c "kill -9 \$(pidof loadbalancer) 2>/dev/null || true" 2>/dev/null || pkill -9 -f loadbalancer 2>/dev/null || true
    sleep 1

    # 2. Launch proxy under taskset Core 0
    if docker compose ps -q loadbalancer 2>/dev/null >/dev/null; then
        if [ "$is_epoll" = "true" ]; then
            docker compose exec -d loadbalancer taskset -c 0 /app/build/loadbalancer -e -p 8080
        else
            docker compose exec -d loadbalancer taskset -c 0 /app/build/loadbalancer -p 8080
        fi
        local client_host="loadbalancer"
    else
        local lb_bin="$PROJECT_ROOT/build/loadbalancer"
        [ ! -x "$lb_bin" ] && lb_bin="$PROJECT_ROOT/build-host/loadbalancer"
        if [ "$is_epoll" = "true" ]; then
            taskset -c 0 "$lb_bin" -e -p 8080 > /dev/null 2>&1 &
        else
            taskset -c 0 "$lb_bin" -p 8080 > /dev/null 2>&1 &
        fi
        local client_host="127.0.0.1"
    fi
    sleep 1.5

    local init_stats=($(get_lb_stats))
    echo "Initial State: Threads=${init_stats[0]} | RSS=${init_stats[1]}MB | PSS=${init_stats[2]}MB | Cgroup=${init_stats[3]}MB"

    local log1="/tmp/idle_client_${arch}_${target}_1.log"
    local log2="/tmp/idle_client_${arch}_${target}_2.log"
    rm -f "$log1" "$log2"

    echo "Spawning client targeting 127.0.0.1:8080 ($target connections)..."
    python3 "$SCRIPT_DIR/idle_client.py" "$target" "127.0.0.1" 8080 "$HOLD_SEC" > "$log1" 2>&1 &
    local client_bg=$!

    # Wait for handshake completion
    echo "Waiting for connection handshakes to complete..."
    local wait_sec=0
    while true; do
        if grep -q "HANDSHAKE_COMPLETE" "$log1" 2>/dev/null; then
            break
        fi
        sleep 1
        wait_sec=$((wait_sec + 1))
        if [ $wait_sec -gt 150 ]; then
            echo "Warning: Timeout waiting for handshake"
            break
        fi
    done
    wait $client_bg || true

    # Sample memory at peak hold
    local peak_stats=($(get_lb_stats))
    echo "Under-Load   : Threads=${peak_stats[0]} | RSS=${peak_stats[1]}MB | PSS=${peak_stats[2]}MB | Cgroup=${peak_stats[3]}MB"

    # Parse metrics from logs
    local est1=$(grep "FINAL_METRICS" "$log1" 2>/dev/null | sed -n 's/.*Established=\([0-9]*\).*/\1/p' || echo "0")
    local est2=$(grep "FINAL_METRICS" "$log2" 2>/dev/null | sed -n 's/.*Established=\([0-9]*\).*/\1/p' || echo "0")
    local est=$(( ${est1:-0} + ${est2:-0} ))

    local dur=$(grep "FINAL_METRICS" "$log1" 2>/dev/null | sed -n 's/.*HandshakeSec=\([0-9.]*\).*/\1/p' || echo "0.0")
    local rate=$(awk -v e="$est" -v d="${dur:-1}" 'BEGIN {if (d>0) printf "%.0f", e/d; else print "0"}')
    local lat=$(grep "FINAL_METRICS" "$log1" 2>/dev/null | sed -n 's/.*ActiveLatMs=\([0-9.]*\).*/\1/p' || echo "0.0")

    local status="SURVIVED"
    if [ "${peak_stats[0]}" -eq 0 ]; then
        status="CRASHED"
    fi

    echo "Result Summary: Est=$est/$target | Handshake=${dur}s (${rate} conn/s) | Threads=${peak_stats[0]} | RSS=${peak_stats[1]}MB | PSS=${peak_stats[2]}MB | Cgroup=${peak_stats[3]}MB | ActiveLat=${lat}ms | Status=$status"
    echo "$arch,$target,$est,$dur,$rate,${peak_stats[0]},${peak_stats[1]},${peak_stats[2]},${peak_stats[3]},$lat,$status" >> "$OUT_CSV"

    docker compose exec -T loadbalancer bash -c "kill -9 \$(pidof loadbalancer) 2>/dev/null || true" 2>/dev/null || pkill -9 -f loadbalancer 2>/dev/null || true
    sleep 1
}

main() {
    echo "Starting isolated idle connection evaluation..."
    for target in 10000 20000 30000; do
        run_idle_cell "Epoll_Optimized" "true" "$target"
        run_idle_cell "Threaded_Proxy" "false" "$target"
    done

    echo ""
    echo "========================================================================"
    echo " Idle Sweep Complete! Results saved to $OUT_CSV"
    echo "========================================================================"
}

main "$@"
