"""Bounded thread-pool helper for independent subprocess-heavy test cases."""
from concurrent.futures import ThreadPoolExecutor
import os


def default_workers(cap=4):
    configured = os.environ.get("KALANDRA_TEST_JOBS")
    if configured:
        return max(1, int(configured))
    return max(1, min(cap, os.cpu_count() or 1))


def run_parallel(items, fn, max_workers=None):
    sequence = list(items)
    if not sequence:
        return []
    workers = min(len(sequence), max_workers or default_workers())
    if workers <= 1:
        return [fn(item) for item in sequence]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(fn, item) for item in sequence]
        results = []
        try:
            for future in futures:
                results.append(future.result())
        except BaseException:
            for future in futures:
                future.cancel()
            raise
        return results
