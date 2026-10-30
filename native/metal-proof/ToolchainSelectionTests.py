"""Explicit Metal selection must verify the exact compiler version."""
from pathlib import Path
import subprocess
import sys
import tempfile

cmake, module, compiler, version = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="kalandra metal selection ") as temporary:
    root = Path(temporary)
    script = root / "select.cmake"
    script.write_text(f'function(resolve_toolchain)\ninclude("{module}")\nendfunction()\n'
                      'resolve_toolchain()\n'
                      'if(NOT IS_DIRECTORY "${KALANDRA_METAL_SDK}")\n'
                      'message(FATAL_ERROR "verified SDK did not survive toolchain scope")\nendif()\n')

    def check(candidate, expected):
        result = subprocess.run(
            [cmake, f"-DKALANDRA_METAL_COMPILER={candidate}",
             f"-DLOCK_metalCompilerVersion={version}", "-P", script],
            capture_output=True, text=True, timeout=20)
        assert (result.returncode == 0) == expected, (candidate, result.stdout, result.stderr)
        if not expected:
            assert "Pinned Metal compiler" in result.stderr, result.stderr

    check(compiler, True)
    # Explicit selection must not accept another release or a version prefix.
    for name, text in (("wrong-version", "Apple metal version 32023.921 (metalfe-other)"),
                       ("suffix-version", f"Apple metal version {version}0 (metalfe-other)"),
                       ("malformed", "not a Metal compiler")):
        fake = root / name
        fake.write_text(f"#!/bin/sh\nprintf '%s\\n' '{text}'\n")
        fake.chmod(0o755)
        check(fake, False)
    check(root / "missing compiler", False)
print("Explicit pinned Metal compiler accepted; wrong, prefix-only, malformed and missing versions rejected")
