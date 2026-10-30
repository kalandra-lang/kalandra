"""Independent command-line formatting and source-preservation checks."""
from pathlib import Path
import subprocess
import sys
import tempfile

server = sys.argv[1]
root = Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)

def run(*args):
    return subprocess.run([server, *map(str, args)], capture_output=True, timeout=20)

with tempfile.TemporaryDirectory(dir=root) as directory:
    path = Path(directory) / "program.k"
    source = b"fn main( ):\r\n    1+2\r\n"
    path.write_bytes(source)
    expected = b"fn main():\n  1 + 2\n"
    result = run(path)
    assert result.returncode == 0 and result.stdout == expected and result.stderr == b"", result
    assert path.read_bytes() == source, "printing formatted source changed the file"
    result = run("--check", path)
    assert result.returncode == 1 and result.stdout == result.stderr == b""
    assert path.read_bytes() == source
    path.write_bytes(expected)
    result = run(path, "--check")
    assert result.returncode == 0 and result.stdout == result.stderr == b""
    path.write_bytes(b"fn main(\n")
    result = run(path)
    assert result.returncode == 1 and result.stdout == b"" and b"error[" in result.stderr
    assert path.read_bytes() == b"fn main(\n"
    retired = path.with_suffix(".kalandra")
    retired.write_bytes(expected)
    result = run(retired)
    assert result.returncode == 1 and result.stdout == b"" and b".k extension" in result.stderr
    for args in [(), ("--write", path), (path, path), ("--check", "--check", path), ("--unknown", path)]:
        result = run(*args)
        assert result.returncode == 2 and result.stdout == b"", (args, result)
    assert run(path.parent / "missing.k").returncode == 1
print("Native formatter CLI: stdout/check, errors, extension and no source writes passed")
