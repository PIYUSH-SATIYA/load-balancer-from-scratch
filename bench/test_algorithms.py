#!/usr/bin/env python3
import time
import subprocess
import urllib.request
import re

def run_cmd(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)

def test_algorithm(algo_name):
    # Stop any existing load balancer
    run_cmd("docker compose exec -T loadbalancer pkill -9 loadbalancer 2>/dev/null || true")
    time.sleep(0.5)
    
    # Start loadbalancer with specified algorithm
    run_cmd(f"docker compose exec -d loadbalancer taskset -c 0 /app/build/loadbalancer -e -a {algo_name}")
    time.sleep(1)
    
    # Send 3000 requests using wrk
    run_cmd("docker compose exec loadbalancer taskset -c 4-7 wrk -t 2 -c 10 -d 3s http://localhost:8080/ > /dev/null 2>&1")
    
    # Send SIGUSR1 to dump stats to stderr/logs
    # Capture the output from container log or by reading /proc
    # Wait, in main.cpp, SIGUSR1 prints to std::cerr!
    # Let's read from docker compose logs
    # Alternatively, capture stderr by launching with stderr redirected to /tmp/stats.log
    run_cmd("docker compose exec -T loadbalancer pkill -USR1 loadbalancer")
    time.sleep(0.5)
    
    # Clean up
    run_cmd("docker compose exec -T loadbalancer pkill -9 loadbalancer 2>/dev/null || true")

def main():
    print("Testing Load Balancing Algorithms Distribution...")
    algos = ["round_robin", "least_connections", "consistent_hashing"]
    
    for algo in algos:
        # Run with logging to file
        run_cmd("docker compose exec -T loadbalancer pkill -9 loadbalancer 2>/dev/null || true")
        time.sleep(0.3)
        run_cmd(f"docker compose exec -d loadbalancer bash -c 'taskset -c 0 /app/build/loadbalancer -e -a {algo} > /tmp/algo_{algo}.log 2>&1'")
        time.sleep(1)
        
        # Send 10,000 requests
        run_cmd("docker compose exec loadbalancer taskset -c 4-7 wrk -t 4 -c 20 -d 3s http://localhost:8080/ > /dev/null 2>&1")
        
        # Trigger stats dump
        run_cmd("docker compose exec loadbalancer pkill -USR1 loadbalancer")
        time.sleep(0.5)
        
        # Read stats log
        res = run_cmd(f"docker compose exec loadbalancer cat /tmp/algo_{algo}.log")
        run_cmd("docker compose exec -T loadbalancer pkill -9 loadbalancer 2>/dev/null || true")
        
        print(f"\n=== Algorithm: {algo} ===")
        lines = [l for l in res.stdout.splitlines() if "Total Requests:" in l]
        for l in lines:
            print("  " + l.strip())

if __name__ == "__main__":
    main()
