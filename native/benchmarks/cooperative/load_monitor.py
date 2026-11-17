"""Interval CPU-load samples for isolated GPU benchmark jobs."""
import os
from pathlib import Path
import subprocess
import sys
import time

platform = "linux" if sys.platform.startswith("linux") else "macos"
previous_cpu = {}
previous_time = None
last_cpu_sample = None


def load():
    global previous_cpu, previous_time, last_cpu_sample
    now = time.monotonic()
    if previous_time is not None and now - previous_time < 0.5:
        return last_cpu_sample
    states = {}
    if platform == "linux":
        for directory in Path("/proc").iterdir():
            if not directory.name.isdigit():
                continue
            try:
                fields = (directory / "stat").read_text()
                end = fields.rindex(")")
                parts = fields[end + 2 :].split()
                states[int(directory.name)] = (
                    int(parts[1]),
                    (int(parts[11]) + int(parts[12])) / os.sysconf("SC_CLK_TCK"),
                    fields[fields.index("(") + 1 : end],
                )
            except (OSError, ValueError, IndexError):
                continue
    else:
        for line in subprocess.check_output(
            ["ps", "-axo", "pid=,ppid=,time=,comm="], text=True
        ).splitlines():
            pid, parent, cpu, exe = line.split(None, 3)
            seconds = 0
            for field in cpu.split(":"):
                seconds = seconds * 60 + float(field)
            states[int(pid)] = (int(parent), seconds, exe)
    own = {os.getpid()}
    for _ in range(20):
        own |= {pid for pid, (parent, _, _) in states.items() if parent in own}
    rows = []
    elapsed = now - previous_time if previous_time is not None else 0
    for pid, (parent, seconds, exe) in states.items():
        if pid not in own and pid in previous_cpu and elapsed > 0:
            rows.append((pid, parent, max(0, 100 * (seconds - previous_cpu[pid]) / elapsed), exe))
    previous_cpu = {pid: s[1] for pid, s in states.items()}
    previous_time = now
    last_cpu_sample = dict(
        utc=time.time(),
        total=sum(r[2] for r in rows),
        top=[
            dict(pid=r[0], cpu=r[2], exe=r[3])
            for r in sorted(rows, key=lambda x: x[2], reverse=True)[:8]
        ],
    )
    return last_cpu_sample


def admitted(sample):
    return sample["total"] <= 200 and all(r["cpu"] <= 90 for r in sample["top"])
