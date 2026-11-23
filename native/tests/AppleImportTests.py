"""Check SDK discovery and exercise real Objective-C dispatch without a GUI session."""
import argparse
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser()
for tool in ("compiler", "checker", "example"):
    parser.add_argument("--" + tool, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="Kalandra SDK consumer ") as temp:
    root = Path(temp)
    app = root / "hello"
    subprocess.run([args.compiler, "--project", Path(args.example) / "kalandra.toml", "-o", app], check=True)
    assert "AppKit.framework" in subprocess.check_output(["otool", "-L", app], text=True)
    manifest = root / "kalandra.toml"
    manifest.write_text('format=1\n[project]\nentry="main.k"\n[dependencies]\nui={system="Foundation"}\n')
    source = root / "main.k"
    source.write_text('from "ui" import objc_getClass, NSString_stringWithUTF8String, NSString_isEqualToString\n'
                      'from "std/process" import ExitStatus\nfn main() -> ExitStatus:\n'
                      '  cls = unsafe objc_getClass("NSString")\n'
                      '  text = unsafe NSString_stringWithUTF8String(cls, "hello")\n'
                      '  if unsafe NSString_isEqualToString(text, text):\n'
                      '    return ExitStatus {code: 0}\n  return ExitStatus {code: 1}\n')
    subprocess.run([args.checker, "--project", manifest], check=True)
    subprocess.run([args.compiler, "--project", manifest, "-o", app], check=True)
    subprocess.run([app], check=True)
    source.write_text(source.read_text().replace("NSString_isEqualToString", "NSString_unknownMethod"))
    invalid = subprocess.run([args.checker, "--project", manifest], capture_output=True, text=True)
    assert invalid.returncode and "NSString_unknownMethod" in invalid.stderr
    manifest.write_text(manifest.read_text().replace('system="Foundation"', 'system="sqlite3"'))
    source.write_text('from "ui" import sqlite3_libversion_number\nfrom "std/process" import ExitStatus\n'
                      'fn main() -> ExitStatus:\n  if unsafe sqlite3_libversion_number() >= 3000000:\n'
                      '    return ExitStatus {code: 0}\n  return ExitStatus {code: 1}\n')
    subprocess.run([args.compiler, "--project", manifest, "-o", app], check=True)
    subprocess.run([app], check=True)
