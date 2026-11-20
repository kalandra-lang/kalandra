"""Exercise the shipped process and stream APIs through a native Kalandra application."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", required=True, type=Path)
parser.add_argument("--variant", required=True, choices=["debug", "optimized", "sanitized", "thread-sanitized"])
args = parser.parse_args()
flags = {"debug": "-O0", "optimized": "-O2", "sanitized": "--sanitize", "thread-sanitized": "--thread-sanitize"}
helper = """import os, sys
from pathlib import Path
mode = sys.argv[1]
if mode == 'both':
    assert os.environ['KALANDRA_STREAM_TEST'] == 'child value'
    assert sys.argv[2:] == ['', 'a b', 'x$()', 'a"b'], sys.argv
    assert sys.stdin.buffer.read() == b''
    sys.stderr.buffer.write(bytes([255]) * 70001)
    sys.stdout.buffer.write(bytes([0]) * 70001)
    sys.exit(39)
elif mode == 'limit':
    sys.stdout.buffer.write(b'abc')
    sys.stderr.buffer.write(b'de')
elif mode == 'status':
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.ExitProcess(0xFEDCBA98)
    else:
        import signal
        os.kill(os.getpid(), signal.SIGTERM)
elif mode == 'store':
    Path('stored.bin').write_bytes(sys.stdin.buffer.read())
    sys.exit(17)
elif mode == 'emit':
    sys.stdout.buffer.write(Path('stored.bin').read_bytes())
"""
with tempfile.TemporaryDirectory() as name:
    root = Path(name)
    (root / "child").mkdir()
    (root / "child/helper.py").write_text(helper)
    data = bytes(range(256)) * 1025 + b"payload"
    (root / "input.bin").write_bytes(data)
    app = root / ("app.exe" if os.name == "nt" else "app")
    source = Path(__file__).with_name("streams") / "process.k"
    subprocess.run([args.compiler.resolve(), source, flags[args.variant], "-o", app], check=True, timeout=180)
    for workers in ("1", "4"):
        run = subprocess.run([app, sys.executable], cwd=root, env=dict(os.environ, KALANDRA_WORKERS=workers),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
        assert run.returncode == 0, (run.returncode, run.stderr, run.stdout)
        assert run.stdout.splitlines() == [b"39:70001:70001:0:255", b"17:17", b"0"], run
        assert not run.stderr, run
        assert (root / "result.bin").read_bytes() == data

    errors = source.with_name("process-errors.k")
    subprocess.run([args.compiler.resolve(), errors, flags[args.variant], "-o", app], check=True, timeout=180)
    run = subprocess.run([app, sys.executable], cwd=root, env=dict(os.environ, KALANDRA_WORKERS="1"),
                         capture_output=True, timeout=45)
    status = str(0xFEDCBA98).encode() if os.name == "nt" else b"-15"
    assert run.returncode == 0 and not run.stderr, run
    assert run.stdout.splitlines() == [b"true:4:true:1:0:0", status], run
