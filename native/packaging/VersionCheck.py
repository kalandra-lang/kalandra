"""Verify installed toolchain identity without compiling or loading source."""
import json
from pathlib import Path
import subprocess


def expected_version():
    return (Path(__file__).resolve().parents[2] / "VERSION").read_text().strip()


def verify_binary_version(binary, version, name=None):
    binary = Path(binary).resolve()
    tool = name or (binary.stem if binary.suffix == ".exe" else binary.name)
    result = subprocess.run([str(binary), "--version"], capture_output=True,
                            text=True, timeout=10)
    assert result.returncode == 0 and not result.stderr, (binary, result.stdout, result.stderr)
    assert result.stdout == f"{tool} {version}\n", (binary, result.stdout, version)


def verify_package_version(prefix):
    prefix = Path(prefix)
    version = expected_version()
    metadata = [prefix / "share/kalandra-native-tools/toolchain.json"]
    metal = prefix / "share/kalandra-native-metal/toolchain.json"
    if metal.is_file():
        metadata.append(metal)
    for path in metadata:
        assert json.loads(path.read_text())["version"] == version, path
    for name in ("kalandra", "kalandra-check", "kalandra-fmt", "kalandra-parse", "kalandra-lsp"):
        for suffix in ("", ".exe"):
            binary = prefix / "bin" / (name + suffix)
            if binary.is_file():
                verify_binary_version(binary, version, name)
