"""Exercise a real native window and its platform event queue from Kalandra."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True, type=Path)
args = parser.parse_args()
compiler = args.compiler.resolve()
tests = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="kalandra window ") as temp:
    project = Path(temp) / "window"
    shutil.copytree(tests.parent / "examples/window", project)
    executable = project / "window.exe"
    for source in (project / "main.k", tests / "WindowQualification.k"):
        if source.name != "main.k":
            shutil.copyfile(source, project / "main.k")
        subprocess.run([compiler, "--project", "kalandra.toml", "-O2", "-o", executable],
                       cwd=project, check=True, timeout=120)
    subprocess.run([executable], cwd=project, check=True, timeout=45)
