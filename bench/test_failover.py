#!/usr/bin/env python3
import asyncio
import time
import subprocess
import urllib.request
import urllib.error

def check_backend_alive(url):
    try:
        with urllib.request.urlopen(url, timeout=1) as resp:
            return resp.status == 200
    except Exception:
        return False

def run_cmd(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)

def main():
    print("========================================================================")
    print(" FAILOVER & HIGH AVAILABILITY BENCHMARK")
    print("========================================================================")

    # 1. Ensure all 3 backends are up
    print("Ensuring backend1, backend2, backend3 are running...")
    run_cmd("docker compose start backend1 backend2 backend3")
    time.sleep(1)

    # 2. Start load balancer with 1s health check interval inside container
    print("Starting load balancer with 1-second health check interval (-h 1)...")
    run_cmd("docker compose exec -T loadbalancer kill -9 $(pidof loadbalancer) 2>/dev/null || true")
    time.sleep(1)
    run_cmd("docker compose exec -d loadbalancer taskset -c 0 /app/build/loadbalancer -e -h 1")
    time.sleep(2)

    # Verify initial health
    assert check_backend_alive("http://localhost:8080/"), "Load balancer not responding!"
    print("Load balancer active on port 8080. All 3 backends initially UP.")

    target_url = "http://localhost:8080/"
    req_interval = 0.05 # 20 requests per second
    total_duration = 8.0 # 8 seconds total
    
    kill_at = 2.0 # Stop backend2 at 2 seconds
    
    results = [] # (timestamp, seq, success, error_msg)

    print(f"Beginning continuous request stream (20 req/s for {total_duration}s)...")
    start_time = time.time()
    t_kill = None
    kill_triggered = False

    seq = 0
    while (time.time() - start_time) < total_duration:
        elapsed = time.time() - start_time
        
        # Trigger failure at kill_at
        if elapsed >= kill_at and not kill_triggered:
            print(f"\n[ALERT] Killing backend2 container (SIGKILL) at elapsed={elapsed:.3f}s...")
            t_kill = time.time()
            run_cmd("docker compose kill -s SIGKILL backend2")
            kill_triggered = True

        # Send probe request
        t_req = time.time()
        try:
            req = urllib.request.Request(target_url, headers={"Connection": "close"})
            with urllib.request.urlopen(req, timeout=1.0) as resp:
                success = (resp.status == 200)
                err = ""
        except Exception as e:
            success = False
            err = str(e)

        results.append((t_req, seq, success, err))
        seq += 1
        time.sleep(req_interval)

    # Clean up load balancer
    run_cmd("docker compose exec -T loadbalancer kill -9 $(pidof loadbalancer) 2>/dev/null || true")

    # Analyze failover metrics
    print("\n========================================================================")
    print(" FAILOVER METRICS ANALYSIS")
    print("========================================================================")
    
    post_kill = [r for r in results if t_kill and r[0] >= t_kill]
    failures = [r for r in post_kill if not r[2]]
    successes = [r for r in post_kill if r[2]]

    print(f"Total requests sent       : {len(results)}")
    print(f"Post-kill requests sent   : {len(post_kill)}")
    print(f"Failed requests           : {len(failures)}")
    print(f"Successful requests       : {len(successes)}")

    if failures:
        first_fail_t = failures[0][0] - t_kill
        last_fail_t = failures[-1][0] - t_kill
        # Convergence time is the time from kill until the final failure was cleared
        convergence_time = last_fail_t + req_interval
        print(f"First failure observed at : +{first_fail_t:.3f}s after backend stop")
        print(f"Last failure observed at  : +{last_fail_t:.3f}s after backend stop")
        print(f"Total Convergence Time    : {convergence_time:.3f} seconds")
    else:
        print("Convergence Time          : 0.000s (zero requests failed!)")

    # Restart backend2 for clean state
    print("\nRestoring backend2 container...")
    run_cmd("docker compose start backend2")
    time.sleep(1)
    print("Backend2 restored cleanly.")

if __name__ == "__main__":
    main()
