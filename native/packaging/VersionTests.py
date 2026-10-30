"""Check source-free version queries for every shipped tool and compiler mode."""
from pathlib import Path
import subprocess
import sys
import tempfile

version, *tools = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="kalandra-version-") as temporary:
    for tool in tools:
        path = Path(tool).resolve()
        name = path.stem if path.suffix == ".exe" else path.name
        result = subprocess.run([str(path), "--version"], cwd=temporary,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, (name, result.stderr)
        assert result.stdout == f"{name} {version}\n", (name, result.stdout)
        assert not result.stderr, (name, result.stderr)
        invalid = subprocess.run([str(path), "--version", "unexpected.k"], cwd=temporary,
                                 capture_output=True, text=True, timeout=10)
        assert invalid.returncode != 0, (name, "Version query ignored extra arguments")
    assert not list(Path(temporary).iterdir()), "Version queries created files"
print(f"All {len(tools)} tool version queries match {version}")
