"""Check SDK discovery and reject Objective-C pointer signatures at import."""
import argparse
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser()
for tool in ("compiler", "checker"):
    parser.add_argument("--" + tool, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="Kalandra SDK consumer ") as temp:
    root = Path(temp)
    app = root / "hello"
    manifest = root / "kalandra.toml"
    manifest.write_text('format=1\n[project]\nentry="main.k"\n[dependencies]\nui={system="Foundation"}\n')
    source = root / "main.k"
    source.write_text('from "ui" import NSPageSize\nfrom "std/process" import ExitStatus\n'
                      'fn main() -> ExitStatus:\n  if NSPageSize() > 0:\n'
                      '    return ExitStatus {code: 0}\n  return ExitStatus {code: 1}\n')
    subprocess.run([args.compiler, "--project", manifest, "-o", app], check=True)
    assert "Foundation.framework" in subprocess.check_output(["otool", "-L", app], text=True)
    subprocess.run([app], check=True)
    source.write_text('from "ui" import objc_getClass\nfn main() -> i64:\n  return 0\n')
    invalid = subprocess.run([args.checker, "--project", manifest], capture_output=True, text=True)
    assert invalid.returncode and "objc_getClass" in invalid.stderr, invalid
    manifest.write_text(manifest.read_text().replace('system="Foundation"', 'system="sqlite3"'))
    source.write_text('from "ui" import sqlite3_libversion_number\nfrom "std/process" import ExitStatus\n'
                      'fn main() -> ExitStatus:\n  if sqlite3_libversion_number() >= 3000000:\n'
                      '    return ExitStatus {code: 0}\n  return ExitStatus {code: 1}\n')
    subprocess.run([args.compiler, "--project", manifest, "-o", app], check=True)
    subprocess.run([app], check=True)
