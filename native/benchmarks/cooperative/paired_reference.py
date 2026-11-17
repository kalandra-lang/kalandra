"""Same-context ABBA/BAAB native-reference comparison with retained load controls."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time
import load_monitor as monitor

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--configuration", type=Path, required=True)
p.add_argument("--driver", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
p.add_argument("--study", choices=("aa", "reference", "repeat"), required=True)
p.add_argument("--control", type=Path)
a = p.parse_args()
a.driver = a.driver.resolve()
assert not a.output.exists()
cfg = json.loads(a.configuration.read_text())
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
current = cfg["commands"]["cooperative-current"][1]
native = cfg["commands"]["cooperative-reference"][1]
artifacts = {
    str(p): sha(p) for p in (a.driver, current, native, Path(__file__), Path(monitor.__file__))
}
evidence = dict(
    status="running",
    study=a.study,
    configuration=cfg,
    configurationSHA256=sha(a.configuration),
    artifacts=artifacts,
    utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    idle=[],
    invocations=[],
    pairs=[],
)


def save():
    a.output.write_text(json.dumps(evidence, indent=2) + "\n")


try:
    if a.study != "aa":
        assert a.control
        control = json.loads(a.control.read_text())
        assert control["status"] == "passed" and control["study"] == "aa"
        assert (
            control["configurationSHA256"] == evidence["configurationSHA256"]
            and control["artifacts"] == artifacts
        )
    for attempt in range(5):
        passed = True
        for _ in range(3):
            sample = monitor.load()
            evidence["idle"].append(sample)
            passed = passed and monitor.admitted(sample)
            if cfg["platform"] == "linux":
                gpu = subprocess.check_output(
                    ["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                    text=True,
                ).strip()
                evidence.setdefault("gpuIdle", []).append(gpu)
                passed = passed and all(int(n.strip()) <= 2 for n in gpu.splitlines())
            save()
            time.sleep(1)
        if passed:
            break
        if attempt == 4:
            raise AssertionError("Idle gate exhausted; every attempt retained")
        time.sleep(2)
    for trial in range(3):
        for n in (65537, 16, 1048576):
            for attempt in range(5):
                command = [
                    *cfg.get("prefix", []),
                    str(a.driver),
                    current if a.study == "aa" else native,
                    current,
                    str(n),
                    "0",
                    "5",
                    "3",
                ]
                record = dict(
                    trial=trial, n=n, attempt=attempt, command=command, load=[monitor.load()]
                )
                evidence["invocations"].append(record)
                save()
                if not monitor.admitted(record["load"][0]):
                    record["loadRejected"] = True
                    save()
                    if attempt == 4:
                        raise AssertionError("Load-only retries exhausted")
                    time.sleep(2)
                    continue
                stdoutPath = a.output.with_name(
                    a.output.name + ".process-" + str(len(evidence["invocations"])) + ".stdout"
                )
                stderrPath = stdoutPath.with_suffix(".stderr")
                with stdoutPath.open("w") as stdoutFile, stderrPath.open("w") as stderrFile:
                    with subprocess.Popen(
                        command, stdout=stdoutFile, stderr=stderrFile, text=True
                    ) as job:
                        started = time.monotonic()
                        while job.poll() is None:
                            if time.monotonic() - started > 180:
                                job.kill()
                                job.wait()
                                raise TimeoutError("Paired benchmark process exceeded 180 seconds")
                            time.sleep(0.5)
                            record["load"].append(monitor.load())
                stdout, stderr = stdoutPath.read_text(), stderrPath.read_text()
                record.update(returncode=job.returncode, stdout=stdout, stderr=stderr)
                save()
                assert job.returncode == 0 and not stderr, (stdout, stderr)
                if not all(monitor.admitted(s) for s in record["load"]):
                    record["loadRejected"] = True
                    save()
                    if attempt == 4:
                        raise AssertionError("Load-only retries exhausted")
                    time.sleep(2)
                    continue
                rows = [json.loads(line) for line in stdout.splitlines()]
                assert len(rows) == 3 * 4 * 7
                for block in range(3):
                    times = {0: [], 1: []}
                    for slot in range(4):
                        group = [r for r in rows if r["block"] == block and r["slot"] == slot]
                        assert [r["round"] for r in group] == list(range(-2, 5))
                        variant = (0 if slot in (0, 3) else 1) ^ (block % 2)
                        assert all(r["variant"] == variant for r in group)
                        count = group[0]["iterations"]
                        assert 1 <= count <= 3000
                        assert all(
                            r["iterations"] == count
                            and r["checksum"] == n * 4 * count
                            and r["ns"] > 0
                            for r in group
                        )
                        times[variant].append(
                            statistics.median(r["ns"] / count for r in group if r["round"] >= 0)
                        )
                    baseline = statistics.mean(times[0])
                    currentTime = statistics.mean(times[1])
                    evidence["pairs"].append(
                        dict(
                            trial=trial,
                            n=n,
                            block=block,
                            baseline_ns=baseline,
                            current_ns=currentTime,
                            time_change=currentTime / baseline - 1,
                        )
                    )
                save()
                print(trial + 1, n, "qualified paired process", flush=True)
                break
    if a.study == "aa":
        checks = []
        for n in (16, 65537, 1048576):
            changes = [r["time_change"] for r in evidence["pairs"] if r["n"] == n]
            checks.append(
                dict(
                    n=n,
                    median=statistics.median(changes),
                    worst=max(map(abs, changes)),
                    passed=abs(statistics.median(changes)) <= 0.05
                    and max(map(abs, changes)) <= 0.10,
                )
            )
        evidence["control_checks"] = checks
        save()
        assert all(
            c["passed"] for c in checks
        ), "Same-context identical control exceeded unchanged 5%/10% limits; retained"
    evidence["status"] = "passed"
    save()
except BaseException as error:
    evidence.update(status="failed", error=str(error))
    save()
    raise
print(a.study, "passed", flush=True)
