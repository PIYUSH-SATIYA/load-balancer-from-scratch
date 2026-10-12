#!/bin/bash
set -e

RESULTS_DIR="./results"
mkdir -p "$RESULTS_DIR"
OUT_CSV="$RESULTS_DIR/idle_sweep_isolated.csv"
echo "Architecture,TargetConns,EstablishedConns,HandshakeSec,ConnRate,Threads,RSS_MB,PSS_MB,Cgroup_MB,ActiveLatency_ms,Status" > "$OUT_CSV"

get_lb_stats() {
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
}

run_idle_cell() {
    local arch=$1
    local is_epoll=$2
    local target=$3

    echo ""
    echo "========================================================================"
    echo " IDLE TEST: $arch | Target Connections: $target"
    echo "========================================================================"

    # Stop and start proxy cleanly inside loadbalancer container
    docker compose exec -T loadbalancer bash -c "kill -9 \$(pidof loadbalancer) 2>/dev/null || true"
    sleep 1

    if [ "$is_epoll" = "true" ]; then
        docker compose exec -d loadbalancer taskset -c 0 /app/build/loadbalancer -e
    else
        docker compose exec -d loadbalancer taskset -c 0 /app/build/loadbalancer
    fi
    sleep 1.5

    local init_stats=($(get_lb_stats))
    echo "Initial State: Threads=${init_stats[0]} | RSS=${init_stats[1]}MB | PSS=${init_stats[2]}MB | Cgroup=${init_stats[3]}MB"

    # Run client from backend1 container so proxy container memory.current has ZERO client memory!
    echo "Spawning client in backend1 container targeting loadbalancer:8080..."
    local client_log="/tmp/idle_client_${arch}_${target}.log"
    docker compose exec -T backend1 python3 /app/bench/idle_client.py $target loadbalancer 8080 8 > "$client_log" 2>&1 &
    local client_bg=$!

    # Wait for ramp up and hold
    sleep 6

    # Sample proxy memory under active hold
    local peak_stats=($(get_lb_stats))
    echo "Under-Load   : Threads=${peak_stats[0]} | RSS=${peak_stats[1]}MB | PSS=${peak_stats[2]}MB | Cgroup=${peak_stats[3]}MB"

    wait $client_bg || true

    # Parse client metrics
    local client_out=$(cat "$client_log" || true)
    local est=$(echo "$client_out" | grep "FINAL_METRICS" | sed -n 's/.*Established=\([0-9]*\).*/\1/p' || echo "0")
    local handshake=$(echo "$client_out" | grep "FINAL_METRICS" | sed -n 's/.*HandshakeSec=\([0-9.]*\).*/\1/p' || echo "0")
    local rate=$(echo "$client_out" | grep "FINAL_METRICS" | sed -n 's/.*ConnRate=\([0-9.]*\).*/\1/p' || echo "0")
    local lat=$(echo "$client_out" | grep "FINAL_METRICS" | sed -n 's/.*ActiveLatMs=\([0-9.]*\).*/\1/p' || echo "0")
    est=${est:-0}
    handshake=${handshake:-0}
    rate=${rate:-0}
    lat=${lat:-0}

    local lb_running=$(docker compose exec -T loadbalancer pidof loadbalancer 2>/dev/null || echo "")
    local status="SURVIVED"
    if [ -z "$lb_running" ]; then
        status="CRASHED"
    fi

    echo "Result Summary: Est=$est/$target | Handshake=${handshake}s | Threads=${peak_stats[0]} | RSS=${peak_stats[1]}MB | Cgroup=${peak_stats[3]}MB | ActiveLat=${lat}ms | Status=$status"
    echo "$arch,$target,$est,$handshake,$rate,${peak_stats[0]},${peak_stats[1]},${peak_stats[2]},${peak_stats[3]},$lat,$status" >> "$OUT_CSV"

    docker compose exec -T loadbalancer bash -c "kill -9 \$(pidof loadbalancer) 2>/dev/null || true"
    sleep 1
}

echo "Starting isolated idle connection evaluation..."
for target in 10000 20000 30000; do
    run_idle_cell "Epoll_Optimized" "true" $target
    run_idle_cell "Threaded_Proxy" "false" $target
done

echo ""
echo "========================================================================"
echo " Idle Sweep Complete! Results saved to $OUT_CSV"
echo "========================================================================"
