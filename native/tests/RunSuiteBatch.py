"""Run independent test suite commands concurrently with bounded worker pools."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 2)))
    parser.add_argument("--child-jobs", type=int, default=2)
    args = parser.parse_args()

    entries = json.loads(args.manifest.read_text(encoding="utf-8"))
    env = dict(os.environ, KALANDRA_TEST_JOBS=str(args.child_jobs))

    def run_entry(entry):
        label = entry["label"]
        command = entry["command"]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
        elapsed = time.monotonic() - started
        return label, command, result.returncode, result.stdout, result.stderr, elapsed

    failures = []
    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, len(entries)))) as pool:
        futures = [pool.submit(run_entry, entry) for entry in entries]
        for future in as_completed(futures):
            label, command, code, stdout, stderr, elapsed = future.result()
            status = "PASS" if code == 0 else "FAIL"
            print(f"[{status} {elapsed:.1f}s] {label}", flush=True)
            if code != 0:
                if stdout:
                    print(stdout, end="" if stdout.endswith("\n") else "\n", flush=True)
                if stderr:
                    print(stderr, end="" if stderr.endswith("\n") else "\n", file=sys.stderr, flush=True)
                failures.append((label, command, code))

    if failures:
        summary = ", ".join(f"{label} (exit {code})" for label, _, code in failures)
        raise SystemExit(f"Suite batch failed: {summary}")


if __name__ == "__main__":
    main()
