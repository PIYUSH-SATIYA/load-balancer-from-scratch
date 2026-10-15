#!/usr/bin/env bash
# ==============================================================================
# High-Performance Load Balancer: Comprehensive Automated Benchmark Suite
# Evaluates Active Throughput, Tail Latency, Context Switches, and CPU Core Usage
# Across 6 Architectures and 5 Concurrency Levels (with statistical confidence)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results"
mkdir -p "$RESULTS_DIR"

SUMMARY_CSV="$RESULTS_DIR/summary.csv"
WARMUP_SEC="${WARMUP_SEC:-3}"
DURATION_SEC="${DURATION_SEC:-10}"
N_RUNS="${N_RUNS:-5}"
DEFAULT_CONNS=(50 100 200 400 800)
CONNS=("${CONNS_OVERRIDE:-${DEFAULT_CONNS[@]}}")

# CPU Pinning Configuration (Intel 12-Core Topology: Cores 0-3 P-Cores, 4-7 Loadgen, 8-11 Backends)
PIN_PROXY_1CORE="0"
PIN_PROXY_4CORE="0-3"
PIN_LOADGEN="4-7"
PIN_BACKENDS="8-11"

# Binary paths
LB_BIN="${LB_BIN:-$PROJECT_ROOT/build/loadbalancer}"
BACKEND_BIN="${BACKEND_BIN:-$PROJECT_ROOT/build/dummy_backend}"
WRK_BIN="$(command -v wrk || echo "/usr/local/bin/wrk")"

if [ ! -x "$WRK_BIN" ]; then
    echo "ERROR: 'wrk' binary not found at $WRK_BIN. Please install wrk or run inside benchmark container." >&2
    exit 1
fi

# Ensure summary.csv has valid headers
if [ ! -f "$SUMMARY_CSV" ] || [ ! -s "$SUMMARY_CSV" ]; then
    echo "Mode,Connections,Repeat,RPS,P50_ms,P99_ms,SocketErrors,Non2xx,ListenOverflows,RetransSegs,Core_Busy,Core_Sys,Core_Soft,Proc_CPU,CPU_Per_kRPS,CSW_per_s,NVCSW_per_s,Survived" > "$SUMMARY_CSV"
fi

# Helper: stop any leftover proxies
cleanup_processes() {
    pkill -9 -f "loadbalancer" 2>/dev/null || true
    pkill -9 -f "nginx" 2>/dev/null || true
}

# Helper: sample CPU times from /proc/stat for a specific core
get_core_stat() {
    local core=$1
    awk -v c="cpu$core" '$1 == c {print $2, $3, $4, $5, $6, $7, $8}' /proc/stat 2>/dev/null || echo "0 0 0 0 0 0 0"
}

# Helper: sample context switches from /proc/$PID/status
get_proc_csw() {
    local pid=$1
    if [ -z "$pid" ] || [ ! -d "/proc/$pid" ]; then
        echo "0 0"
        return
    fi
    local vol=$(awk '/^voluntary_ctxt_switches:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
    local nonvol=$(awk '/^nonvoluntary_ctxt_switches:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
    echo "${vol:-0} ${nonvol:-0}"
}

# Helper: calculate delta and percentage core busy
calc_core_busy() {
    local s1=($1)
    local s2=($2)
    # Fields: user nice system idle iowait irq softirq
    local user_diff=$(( ${s2[0]} - ${s1[0]} ))
    local sys_diff=$(( ${s2[2]} - ${s1[2]} ))
    local idle_diff=$(( ${s2[3]} - ${s1[3]} ))
    local soft_diff=$(( ${s2[6]} - ${s1[6]} ))

    local total_diff=$(( user_diff + sys_diff + idle_diff + soft_diff ))
    if [ $total_diff -le 0 ]; then
        echo "0.0 0.0 0.0"
        return
    fi

    local busy_diff=$(( total_diff - idle_diff ))
    local busy_pct=$(awk -v b="$busy_diff" -v t="$total_diff" 'BEGIN {printf "%.1f", (b/t)*100}')
    local sys_pct=$(awk -v s="$sys_diff" -v t="$total_diff" 'BEGIN {printf "%.1f", (s/t)*100}')
    local soft_pct=$(awk -v sf="$soft_diff" -v t="$total_diff" 'BEGIN {printf "%.1f", (sf/t)*100}')
    echo "$busy_pct $sys_pct $soft_pct"
}

# Run a single cell benchmark
run_benchmark_cell() {
    local mode=$1
    local conns=$2
    local rep=$3
    local target_url=""
    local proxy_pid=""

    cleanup_processes
    sleep 0.5

    echo "------------------------------------------------------------------------"
    echo " Running: Mode=$mode | Conns=$conns | Repeat=$rep/$N_RUNS"
    echo "------------------------------------------------------------------------"

    case "$mode" in
        "Control")
            # Direct backend bypassing load balancer
            if getent hosts backend1 >/dev/null 2>&1; then
                target_url="http://backend1:9001/"
            else
                target_url="http://127.0.0.1:9001/"
            fi
            ;;
        "Epoll_Optimized")
            taskset -c "$PIN_PROXY_1CORE" "$LB_BIN" -e -p 8080 > /dev/null 2>&1 &
            proxy_pid=$!
            target_url="http://127.0.0.1:8080/"
            ;;
        "Epoll_4Workers")
            taskset -c "$PIN_PROXY_4CORE" "$LB_BIN" -e -w 4 -p 8080 > /dev/null 2>&1 &
            proxy_pid=$!
            target_url="http://127.0.0.1:8080/"
            ;;
        "Threaded_Proxy")
            taskset -c "$PIN_PROXY_1CORE" "$LB_BIN" -p 8080 > /dev/null 2>&1 &
            proxy_pid=$!
            target_url="http://127.0.0.1:8080/"
            ;;
        "Threaded_4Cores")
            taskset -c "$PIN_PROXY_4CORE" "$LB_BIN" -p 8080 > /dev/null 2>&1 &
            proxy_pid=$!
            target_url="http://127.0.0.1:8080/"
            ;;
        "Nginx_Stream")
            if getent hosts nginx-comparison >/dev/null 2>&1; then
                target_url="http://nginx-comparison:8080/"
            else
                target_url="http://127.0.0.1:8081/"
            fi
            ;;
        *)
            echo "Unknown architecture mode: $mode" >&2
            return 1
            ;;
    esac

    sleep 1

    # Warmup phase (let TCP connection pool stabilize)
    echo "  > Warmup (${WARMUP_SEC}s @ $conns conns)..."
    taskset -c "$PIN_LOADGEN" "$WRK_BIN" -t 2 -c "$conns" -d "${WARMUP_SEC}s" "$target_url" > /dev/null 2>&1 || true

    # Pre-measurement state
    local core_before=$(get_core_stat 0)
    local csw_before=($(get_proc_csw "$proxy_pid"))
    local t0=$(date +%s%N)

    # Measurement phase
    echo "  > Measuring (${DURATION_SEC}s @ $conns conns)..."
    local wrk_raw
    wrk_raw=$(taskset -c "$PIN_LOADGEN" "$WRK_BIN" -t 4 -c "$conns" -d "${DURATION_SEC}s" --latency "$target_url" 2>&1 || true)

    local t1=$(date +%s%N)
    local elapsed_sec=$(awk -v t0="$t0" -v t1="$t1" 'BEGIN {printf "%.3f", (t1 - t0)/1000000000}')

    # Post-measurement state
    local core_after=$(get_core_stat 0)
    local csw_after=($(get_proc_csw "$proxy_pid"))

    local survived="YES"
    if [ -n "$proxy_pid" ]; then
        if ! kill -0 "$proxy_pid" 2>/dev/null; then
            survived="NO"
        fi
    fi

    # Parse metrics via python helper
    local parsed_metrics
    parsed_metrics=$(echo "$wrk_raw" | python3 "$SCRIPT_DIR/lib/parse_wrk.py")

    # Extract key variables
    local rps=$(echo "$parsed_metrics" | sed -n 's/.*rps=\([0-9.]*\).*/\1/p')
    local p50=$(echo "$parsed_metrics" | sed -n 's/.*p50_ms=\([0-9.]*\).*/\1/p')
    local p99=$(echo "$parsed_metrics" | sed -n 's/.*p99_ms=\([0-9.]*\).*/\1/p')
    local non2xx=$(echo "$parsed_metrics" | sed -n 's/.*non_2xx=\([0-9.]*\).*/\1/p')
    local err_summary=$(echo "$parsed_metrics" | sed -n 's/.*error_summary=\([^ ]*\).*/\1/p')
    
    rps=${rps:-0.0}
    p50=${p50:-0.0}
    p99=${p99:-0.0}
    non2xx=${non2xx:-0}
    err_summary=${err_summary:-0}

    # Context switches delta rate
    local vol_delta=$(( ${csw_after[0]} - ${csw_before[0]} ))
    local nvol_delta=$(( ${csw_after[1]} - ${csw_before[1]} ))
    local vol_rate=$(awk -v d="$vol_delta" -v e="$elapsed_sec" 'BEGIN {printf "%.0f", d/e}')
    local nvol_rate=$(awk -v d="$nvol_delta" -v e="$elapsed_sec" 'BEGIN {printf "%.0f", d/e}')

    # Core utilization
    local core_metrics=($(calc_core_busy "$core_before" "$core_after"))
    local core_busy="${core_metrics[0]}"
    local core_sys="${core_metrics[1]}"
    local core_soft="${core_metrics[2]}"

    local cpu_per_krps=$(awk -v b="$core_busy" -v r="$rps" 'BEGIN {if (r > 0) printf "%.4f", b / (r / 1000); else print "0.0"}')

    echo "  > Result: RPS=$rps | P50=${p50}ms | P99=${p99}ms | CoreBusy=${core_busy}% | VolCSW/s=${vol_rate} | Status=$survived"

    # Append to summary.csv
    echo "$mode,$conns,$rep,$rps,$p50,$p99,\"$err_summary\",$non2xx,0,0,$core_busy,$core_sys,$core_soft,0,$cpu_per_krps,$vol_rate,$nvol_rate,$survived" >> "$SUMMARY_CSV"

    cleanup_processes
    sleep 0.5
}

# Main execution loop
main() {
    echo "========================================================================"
    echo " HIGH-PERFORMANCE LOAD BALANCER BENCHMARK SUITE"
    echo " Repetitions: $N_RUNS | Concurrencies: ${CONNS[*]} | Duration: ${DURATION_SEC}s"
    echo " Results file: $SUMMARY_CSV"
    echo "========================================================================"

    local MODES=("Control" "Epoll_Optimized" "Epoll_4Workers" "Threaded_Proxy" "Threaded_4Cores" "Nginx_Stream")
    if [ $# -gt 0 ]; then
        MODES=("$@")
    fi

    for conns in "${CONNS[@]}"; do
        for rep in $(seq 1 "$N_RUNS"); do
            for mode in "${MODES[@]}"; do
                run_benchmark_cell "$mode" "$conns" "$rep"
            done
        done
    done

    echo ""
    echo "========================================================================"
    echo " Benchmark Run Complete! Aggregating statistical summary..."
    echo "========================================================================"

    # Statistical summary generation
    python3 -c "
import csv
from collections import defaultdict
from bench.lib.stats import compute_stats, format_stats

results = defaultdict(lambda: defaultdict(list))
with open('$SUMMARY_CSV', 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        m = row['Mode']
        c = int(row['Connections'])
        try:
            rps = float(row['RPS'])
            results[m][c].append(rps)
        except Exception:
            pass

print('\n### STATISTICAL SUMMARY (RPS Mean ± StdDev | 95% Confidence Interval):')
for m, c_dict in sorted(results.items()):
    print(f'\nArchitecture: {m}')
    for c, vals in sorted(c_dict.items()):
        s = compute_stats(vals)
        print(f'  Conns={c:3d}: {format_stats(s, unit=\"RPS\")}')
"
}

main "$@"
