"""Execute public tree operations against a private temporary directory."""
import concurrent.futures
import os
from pathlib import Path
import subprocess
import tempfile


def exercise(compiler, variant, target, compile_only):
    flags = {"debug": "-O0", "optimized": "-O2", "sanitized": "--sanitize",
             "thread-sanitized": "--thread-sanitize"}
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        app = root / ("runner.exe" if os.name == "nt" else "runner")
        source = Path(__file__).with_name("RecursiveFilesystem.k")
        if target != "cpu":
            mixed = root / "mixed.k"
            text = source.read_text().replace("FsError | IoError):", "FsError | IoError | TaskError):")
            text = text.replace("  path = args[1]", """  doubled = try parallel:
    gpu = devices.gpu()
    try await spawn double(21) on gpu
  if doubled != 42:
    try println("wrong device result")
  path = args[1]""")
            mixed.write_text("fn double(n:i64)->i64:\n  return n*2\n" + text)
            source = mixed
        command = [compiler, source, flags[variant], "-o", app]
        if target != "cpu":
            command.extend(["--target", target])
        subprocess.run(command, check=True, timeout=240)
        if compile_only:
            return
        if target != "cpu":
            app = app / ("program.exe" if os.name == "nt" else "program")

        def run(action, path, success=True):
            result = subprocess.run([app, action, str(path)], cwd=root, capture_output=True,
                                    text=True, encoding="utf-8", timeout=45)
            assert (result.returncode == 0) == success, (action, path, result)
            return result.stdout.splitlines()

        # Two independently scheduled processes can both create the same parents.
        leaf = Path("tree") / "nested" / "café"
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(lambda _: run("create", leaf), range(2)))
        run("create", leaf)
        (root / leaf / "file").write_bytes(b"contents")
        run("create", leaf / "file" / "child", False)
        run("remove", leaf / "file", False)
        run("remove", "tree/.", False)
        run("remove", "tree/nested/..", False)
        assert (root / leaf / "file").read_bytes() == b"contents"
        run("walk", "missing", False)
        run("remove", "missing", False)
        run("first", "tree")

        outside = root / "untouched"
        outside.mkdir()
        (outside / "keep").write_bytes(b"sentinel")

        def link(at, to):
            if os.name == "nt":
                subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(at), str(to)],
                               capture_output=True, check=True)
            else:
                at.symlink_to(to, target_is_directory=True)

        link(root / "tree" / "link", outside)
        link(root / "tree" / "cycle", root / "tree")
        (outside / "child").mkdir()
        run("watch", Path("tree") / "link" / "child", False)
        run("watch", Path("tree") / "link", False)
        run("walk", Path("tree") / "link", False)
        run("remove", Path("tree") / "link" / "keep", False)
        entries = [(kind, Path(name)) for kind, name in
                   (line.split(" ", 1) for line in run("walk", "tree"))]
        assert sorted(entries) == sorted([("dir", leaf.parent), ("dir", leaf),
                                          ("file", leaf / "file"), ("link", Path("tree/link")),
                                          ("link", Path("tree/cycle"))]), entries
        assert entries.index(("dir", leaf.parent)) < entries.index(("dir", leaf)) < entries.index(("file", leaf / "file"))
        run("remove", Path("tree") / "link")
        assert not os.path.lexists(root / "tree" / "link")
        run("remove", "tree")
        assert not os.path.lexists(root / "tree")
        assert (outside / "keep").read_bytes() == b"sentinel"
