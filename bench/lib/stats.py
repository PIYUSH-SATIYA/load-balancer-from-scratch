#!/usr/bin/env python3
"""
Shared statistical calculation library for benchmark analysis.
Calculates mean, median, standard deviation, 95% confidence intervals,
min/max, and latency percentiles.
"""
import sys
import math
import statistics
from typing import List, Dict, Any, Optional

# Two-tailed t-distribution critical values for 95% confidence (alpha = 0.05)
# Index corresponds to degrees of freedom df = n - 1 (df from 1 to 30)
STUDENT_T_95 = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571,
    6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228,
    11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131,
    16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086,
    21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064, 25: 2.060,
    26: 2.056, 27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042
}

def get_t_crit(df: int) -> float:
    """Return critical t value for 95% confidence interval."""
    if df <= 0:
        return 1.96
    if df in STUDENT_T_95:
        return STUDENT_T_95[df]
    if df < 60:
        return 2.00
    if df < 120:
        return 1.98
    return 1.96

def percentile(sorted_data: List[float], p: float) -> float:
    """Calculate the p-th percentile (0 <= p <= 100) using linear interpolation."""
    if not sorted_data:
        return 0.0
    if len(sorted_data) == 1:
        return sorted_data[0]
    
    k = (len(sorted_data) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return sorted_data[int(k)]
    d0 = sorted_data[int(f)] * (c - k)
    d1 = sorted_data[int(c)] * (k - f)
    return d0 + d1

def compute_stats(values: List[float]) -> Dict[str, Any]:
    """
    Compute comprehensive statistical summary over a list of numeric samples.
    Returns:
        n, mean, median, stddev, se, ci95_low, ci95_high, ci95_margin,
        min, max, p50, p90, p95, p99, p99_9
    """
    data = [float(v) for v in values if v is not None and not math.isnan(float(v))]
    n = len(data)
    if n == 0:
        return {
            "n": 0, "mean": 0.0, "median": 0.0, "stddev": 0.0, "se": 0.0,
            "ci95_low": 0.0, "ci95_high": 0.0, "ci95_margin": 0.0,
            "min": 0.0, "max": 0.0,
            "p50": 0.0, "p90": 0.0, "p95": 0.0, "p99": 0.0, "p99_9": 0.0
        }

    sorted_data = sorted(data)
    mean_val = statistics.mean(sorted_data)
    median_val = statistics.median(sorted_data)
    
    if n >= 2:
        stddev_val = statistics.stdev(sorted_data)
        se_val = stddev_val / math.sqrt(n)
        t_crit = get_t_crit(n - 1)
        margin = t_crit * se_val
    else:
        stddev_val = 0.0
        se_val = 0.0
        margin = 0.0

    return {
        "n": n,
        "mean": mean_val,
        "median": median_val,
        "stddev": stddev_val,
        "se": se_val,
        "ci95_low": mean_val - margin,
        "ci95_high": mean_val + margin,
        "ci95_margin": margin,
        "min": sorted_data[0],
        "max": sorted_data[-1],
        "p50": percentile(sorted_data, 50.0),
        "p90": percentile(sorted_data, 90.0),
        "p95": percentile(sorted_data, 95.0),
        "p99": percentile(sorted_data, 99.0),
        "p99_9": percentile(sorted_data, 99.9)
    }

def format_stats(stats: Dict[str, Any], unit: str = "", precision: int = 2) -> str:
    """Format statistical summary as a readable one-liner."""
    fmt = f"{{:.{precision}f}}"
    mean_str = fmt.format(stats['mean'])
    std_str = fmt.format(stats['stddev'])
    med_str = fmt.format(stats['median'])
    ci_str = f"[{fmt.format(stats['ci95_low'])}, {fmt.format(stats['ci95_high'])}]"
    u = f" {unit}" if unit else ""
    return (f"n={stats['n']} | Mean: {mean_str} ± {std_str}{u} | "
            f"Median: {med_str}{u} | 95% CI: {ci_str}{u} | "
            f"Min/Max: {fmt.format(stats['min'])} / {fmt.format(stats['max'])}{u}")

if __name__ == "__main__":
    # Test CLI invocation
    args = sys.argv[1:]
    if not args and not sys.stdin.isatty():
        args = sys.stdin.read().split()
    if args:
        vals = [float(x) for x in args]
        s = compute_stats(vals)
        print(format_stats(s))
    else:
        # Self-test
        test_samples = [100.2, 102.5, 99.8, 101.1, 100.7]
        s = compute_stats(test_samples)
        print("Self-test with sample data:", test_samples)
        print(format_stats(s, unit="RPS"))
