#!/usr/bin/env bash
# ==============================================================================
# TCP Proxy: Automated Throughput and Latency Suite
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
if [ -n "${CONNS_OVERRIDE:-}" ]; then
    read -r -a CONNS <<< "$CONNS_OVERRIDE"
else
    CONNS=("${DEFAULT_CONNS[@]}")
fi

# CPU Pinning Configuration (Intel 12-Core Topology: Cores 0-3 P-Cores, 4-7 Loadgen, 8-11 Backends)
PIN_PROXY_1CORE="${PIN_PROXY_1CORE:-0}"
PIN_PROXY_4CORE="${PIN_PROXY_4CORE:-0-3}"
PIN_LOADGEN="${PIN_LOADGEN:-4-7}"
PIN_BACKENDS="${PIN_BACKENDS:-8-11}"
PIN_NGINX="${PIN_NGINX:-0}"
export WARMUP_SEC DURATION_SEC N_RUNS PIN_PROXY_1CORE PIN_PROXY_4CORE PIN_LOADGEN PIN_BACKENDS PIN_NGINX

# Binary paths
LB_BIN="${LB_BIN:-$PROJECT_ROOT/build/loadbalancer}"
BACKEND_BIN="${BACKEND_BIN:-$PROJECT_ROOT/build/dummy_backend}"
WRK_BIN="$(command -v wrk || echo "/usr/local/bin/wrk")"
export WRK_BIN

if [ ! -x "$WRK_BIN" ]; then
    echo "ERROR: 'wrk' binary not found at $WRK_BIN. Please install wrk or run inside benchmark container." >&2
    exit 1
fi

mkdir -p "$RESULTS_DIR/raw"
echo "Mode,Connections,Repeat,RPS,P50_ms,P75_ms,P90_ms,P99_ms,Requests,SocketErrors,Non2xx,Core_Busy,Core_Sys,Core_Soft,CPU_Per_kRPS,CSW_per_s,NVCSW_per_s,CSW_Threads_Sampled,Status" > "$SUMMARY_CSV"
CURRENT_PROXY_PID=""
CURRENT_CSW_PID=""

# Helper: stop any leftover proxies
cleanup_processes() {
    if [ -n "$CURRENT_CSW_PID" ] && kill -0 "$CURRENT_CSW_PID" 2>/dev/null; then
        kill -TERM "$CURRENT_CSW_PID" 2>/dev/null || true
        wait "$CURRENT_CSW_PID" 2>/dev/null || true
    fi
    CURRENT_CSW_PID=""
    if [ -n "$CURRENT_PROXY_PID" ] && kill -0 "$CURRENT_PROXY_PID" 2>/dev/null; then
        kill -TERM "$CURRENT_PROXY_PID" 2>/dev/null || true
        wait "$CURRENT_PROXY_PID" 2>/dev/null || true
    fi
    CURRENT_PROXY_PID=""
}
trap cleanup_processes EXIT INT TERM

# Helper: aggregate CPU times for the CPUs assigned to the proxy.
get_core_stat() {
    local cores=" $* "
    awk -v selected="$cores" '$1 ~ /^cpu[0-9]+$/ {
        cpu = substr($1, 4)
        if (index(selected, " " cpu " "))
            for (i = 2; i <= 8; i++) total[i-1] += $i
    } END {
        for (i = 1; i <= 7; i++) printf "%s%d", (i == 1 ? "" : " "), total[i]
        print ""
    }' /proc/stat 2>/dev/null || echo "0 0 0 0 0 0 0"
}

parse_cpu_set() {
    local item first last cpu
    local -a cpus=()
    local -a items=()
    IFS=',' read -r -a items <<< "$1"
    for item in "${items[@]}"; do
        if [[ "$item" == *-* ]]; then
            first=${item%-*}
            last=${item#*-}
            for ((cpu=first; cpu<=last; cpu++)); do cpus+=("$cpu"); done
        else
            cpus+=("$item")
        fi
    done
    printf '%s ' "${cpus[@]}"
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
            CURRENT_PROXY_PID=$proxy_pid
            target_url="http://127.0.0.1:8080/"
            ;;
        "Epoll_4Workers")
            taskset -c "$PIN_PROXY_4CORE" "$LB_BIN" -e -w 4 -p 8080 > /dev/null 2>&1 &
            proxy_pid=$!
            CURRENT_PROXY_PID=$proxy_pid
            target_url="http://127.0.0.1:8080/"
            ;;
        "Threaded_Proxy")
            taskset -c "$PIN_PROXY_1CORE" "$LB_BIN" -p 8080 > /dev/null 2>&1 &
            proxy_pid=$!
            CURRENT_PROXY_PID=$proxy_pid
            target_url="http://127.0.0.1:8080/"
            ;;
        "Threaded_4Cores")
            taskset -c "$PIN_PROXY_4CORE" "$LB_BIN" -p 8080 > /dev/null 2>&1 &
            proxy_pid=$!
            CURRENT_PROXY_PID=$proxy_pid
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
    if [ -n "$proxy_pid" ] && { ! kill -0 "$proxy_pid" 2>/dev/null || ! curl -fsS --max-time 2 "$target_url" >/dev/null; }; then
        echo "ERROR: $mode failed its readiness request" >&2
        cleanup_processes
        return 1
    fi

    # Warmup phase (let TCP connection pool stabilize)
    echo "  > Warmup (${WARMUP_SEC}s @ $conns conns)..."
    taskset -c "$PIN_LOADGEN" "$WRK_BIN" -t 2 -c "$conns" -d "${WARMUP_SEC}s" "$target_url" > /dev/null 2>&1 || true

    # Pre-measurement state
    local cpu_set="$PIN_PROXY_1CORE"
    case "$mode" in
        Epoll_4Workers|Threaded_4Cores) cpu_set="$PIN_PROXY_4CORE" ;;
        Control) cpu_set="$PIN_BACKENDS" ;;
        Nginx_Stream) cpu_set="$PIN_NGINX" ;;
    esac
    local -a proxy_cores
    read -r -a proxy_cores <<< "$(parse_cpu_set "$cpu_set")"
    local core_before=$(get_core_stat "${proxy_cores[@]}")
    local csw_output="$RESULTS_DIR/raw/${mode}_c${conns}_r${rep}_csw.json"
    local vol_rate="NA" nvol_rate="NA" sampled_threads=0
    if [ -n "$proxy_pid" ]; then
        rm -f "$csw_output"
        taskset -c "$PIN_LOADGEN" python3 "$SCRIPT_DIR/lib/sample_proc_csw.py" "$proxy_pid" "$csw_output" &
        CURRENT_CSW_PID=$!
        sleep 0.05
    fi
    local t0=$(date +%s%N)

    # Measurement phase
    echo "  > Measuring (${DURATION_SEC}s @ $conns conns)..."
    local wrk_raw
    wrk_raw=$(taskset -c "$PIN_LOADGEN" "$WRK_BIN" -t 4 -c "$conns" -d "${DURATION_SEC}s" --latency "$target_url" 2>&1 || true)
    printf '%s\n' "$wrk_raw" > "$RESULTS_DIR/raw/${mode}_c${conns}_r${rep}.txt"

    local t1=$(date +%s%N)
    local elapsed_sec=$(awk -v t0="$t0" -v t1="$t1" 'BEGIN {printf "%.3f", (t1 - t0)/1000000000}')

    # Post-measurement state
    local core_after=$(get_core_stat "${proxy_cores[@]}")
    if [ -n "$CURRENT_CSW_PID" ]; then
        kill -TERM "$CURRENT_CSW_PID" 2>/dev/null || true
        wait "$CURRENT_CSW_PID" 2>/dev/null || true
        CURRENT_CSW_PID=""
        if [ -s "$csw_output" ]; then
            local sampled=($(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["voluntary_context_switches"], d["nonvoluntary_context_switches"], d["threads_sampled"])' "$csw_output"))
            vol_rate=$(awk -v d="${sampled[0]}" -v e="$elapsed_sec" 'BEGIN {printf "%.0f", d/e}')
            nvol_rate=$(awk -v d="${sampled[1]}" -v e="$elapsed_sec" 'BEGIN {printf "%.0f", d/e}')
            sampled_threads="${sampled[2]}"
        fi
    fi

    local survived="PASS"
    if [ -n "$proxy_pid" ]; then
        if ! kill -0 "$proxy_pid" 2>/dev/null; then
            survived="FAILED"
        fi
    fi

    # Parse metrics via python helper
    local parsed_metrics
    parsed_metrics=$(echo "$wrk_raw" | python3 "$SCRIPT_DIR/lib/parse_wrk.py")

    # Extract key variables
    local rps=$(echo "$parsed_metrics" | sed -n 's/.*rps=\([0-9.]*\).*/\1/p')
    local p50=$(echo "$parsed_metrics" | sed -n 's/.*p50_ms=\([0-9.]*\).*/\1/p')
    local p75=$(echo "$parsed_metrics" | sed -n 's/.*p75_ms=\([0-9.]*\).*/\1/p')
    local p90=$(echo "$parsed_metrics" | sed -n 's/.*p90_ms=\([0-9.]*\).*/\1/p')
    local p99=$(echo "$parsed_metrics" | sed -n 's/.*p99_ms=\([0-9.]*\).*/\1/p')
    local requests=$(echo "$parsed_metrics" | sed -n 's/.*requests=\([0-9.]*\).*/\1/p')
    local err_connect=$(echo "$parsed_metrics" | sed -n 's/.*socket_errors_connect=\([0-9.]*\).*/\1/p')
    local err_read=$(echo "$parsed_metrics" | sed -n 's/.*socket_errors_read=\([0-9.]*\).*/\1/p')
    local err_write=$(echo "$parsed_metrics" | sed -n 's/.*socket_errors_write=\([0-9.]*\).*/\1/p')
    local err_timeout=$(echo "$parsed_metrics" | sed -n 's/.*socket_errors_timeout=\([0-9.]*\).*/\1/p')
    local non2xx=$(echo "$parsed_metrics" | sed -n 's/.*non_2xx=\([0-9.]*\).*/\1/p')
    
    rps=${rps:-0.0}
    p50=${p50:-0.0}
    p75=${p75:-0.0}
    p90=${p90:-0.0}
    p99=${p99:-0.0}
    requests=${requests:-0}
    err_connect=${err_connect:-0}
    err_read=${err_read:-0}
    err_write=${err_write:-0}
    err_timeout=${err_timeout:-0}
    local total_errors=$((err_connect + err_read + err_write + err_timeout))
    non2xx=${non2xx:-0}
    if [ "$requests" -eq 0 ] || [ "$rps" = "0.0" ]; then
        survived="FAILED"
    elif [ "$total_errors" -gt 0 ] || [ "$non2xx" -gt 0 ]; then
        survived="PARTIAL"
    fi

    # Core utilization
    local core_metrics=($(calc_core_busy "$core_before" "$core_after"))
    local core_busy="${core_metrics[0]}"
    local core_sys="${core_metrics[1]}"
    local core_soft="${core_metrics[2]}"

    local cpu_per_krps=$(awk -v b="$core_busy" -v r="$rps" 'BEGIN {if (r > 0) printf "%.4f", b / (r / 1000); else print "0.0"}')

    echo "  > Result: RPS=$rps | P50=${p50}ms | P99=${p99}ms | CoreBusy=${core_busy}% | VolCSW/s=${vol_rate} | SampledThreads=$sampled_threads | Status=$survived"

    # Append to summary.csv
    echo "$mode,$conns,$rep,$rps,$p50,$p75,$p90,$p99,$requests,$total_errors,$non2xx,$core_busy,$core_sys,$core_soft,$cpu_per_krps,$vol_rate,$nvol_rate,$sampled_threads,$survived" >> "$SUMMARY_CSV"

    cleanup_processes
    sleep 0.5
}

# Main execution loop
main() {
    echo "========================================================================"
    echo " TCP PROXY BENCHMARK SUITE"
    echo " Repetitions: $N_RUNS | Concurrencies: ${CONNS[*]} | Duration: ${DURATION_SEC}s"
    echo " Results file: $SUMMARY_CSV"
    echo "========================================================================"

    local MODES=("Control" "Epoll_Optimized" "Epoll_4Workers" "Threaded_Proxy" "Threaded_4Cores" "Nginx_Stream")
    if [ $# -gt 0 ]; then
        MODES=("$@")
    fi

    python3 - "$RESULTS_DIR/benchmark_manifest.json" "${MODES[@]}" <<'PY'
import json, os, platform, subprocess, sys
version = subprocess.run([os.environ.get("WRK_BIN", "wrk"), "--version"], text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
version = version.splitlines()[0] if version else "unknown"
manifest = {
    "warmup_seconds": int(os.environ["WARMUP_SEC"]),
    "measurement_seconds": int(os.environ["DURATION_SEC"]),
    "repetitions": int(os.environ["N_RUNS"]),
    "concurrency_levels": os.environ.get("CONNS_OVERRIDE", "50 100 200 400 800").split(),
    "modes": sys.argv[2:],
    "proxy_single_cpu_set": os.environ.get("PIN_PROXY_1CORE", "0"),
    "proxy_multi_cpu_set": os.environ.get("PIN_PROXY_4CORE", "0-3"),
    "load_generator_cpu_set": os.environ.get("PIN_LOADGEN", "4-7"),
    "backend_cpu_set": os.environ.get("PIN_BACKENDS", "8-11"),
    "nginx_cpu_set": os.environ.get("PIN_NGINX", "0"),
    "wrk_version": version,
    "cpu_count": os.cpu_count(),
    "kernel_release": platform.release(),
    "platform": platform.platform(),
}
with open(sys.argv[1], "w") as output:
    json.dump(manifest, output, indent=2)
    output.write("\n")
PY

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
