"""Execute the native CPU language/runtime contract on Linux and Windows."""
import argparse
import json
import os
from pathlib import Path
import platform
import stat
import subprocess
import tempfile


def qualify(compiler):
    compiler = compiler.resolve()
    windows = platform.system() == "Windows"
    modes = [("normal", []), ("optimized", ["-O2"]), ("address-undefined", ["--sanitize"])]
    if not windows:
        modes.append(("thread", ["--thread-sanitize"]))
    with tempfile.TemporaryDirectory(prefix="kalandra portable acceptance ") as folder:
        work = Path(folder).resolve()
        source = work / "source λ🙂.k"
        executable = work / ("program.exe" if windows else "program")

        def run(args, expected=0, env=None):
            result = subprocess.run(list(map(str, args)), cwd=work, capture_output=True,
                                    text=True, encoding="utf-8", timeout=120, env=env)
            assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
            return result

        cases = []
        values = {"int8": "127", "uint8": "255", "int16": "32767", "uint16": "65535",
                  "int32": "2147483647", "uint32": "4294967295",
                  "int64": "9223372036854775807", "uint64": "18446744073709551615",
                  "float32": "1.5", "float64": "1.5"}
        rank8_values = "[" * 8 + "1,2" + "]" * 8
        for numeric, value in values.items():
            cases += [(f"{numeric} scalar", f"fn main()->{numeric}:\n  {value}\n", value),
                      (f"{numeric} array", f"fn main()->[{numeric}]:\n  [{value},0]\n", f"[{value}, 0]"),
                      (f"{numeric} tensor", f"fn main()->Tensor[{numeric},2,2]:\n  tensor([[1,2],[3,4]])*2\n",
                       "tensor([2, 4, 6, 8], [2, 2])"),
                      (f"{numeric} rank-zero tensor", f"fn main()->Tensor[{numeric}]:\n  tensor(1)*2\n",
                       "tensor([2], [])"),
                      (f"{numeric} rank-eight tensor", f"fn main()->Tensor[{numeric},1,1,1,1,1,1,1,2]:\n  tensor({rank8_values})*2\n",
                       "tensor([2, 4], [1, 1, 1, 1, 1, 1, 1, 2])"),
                      (f"{numeric} worker", f"fn echo(value:{numeric})->{numeric}:\n  value\n"
                       f"fn main()->{numeric} ! TaskError:\n  try parallel:\n"
                       f"    work=spawn echo({value})\n    first=try await work\n    again=try await work\n"
                       "    if first==again:again\n    else:0\n", value)]
            cases.append((f"{numeric} recursive scalar", f"fn tree(v:{numeric},n:int)->{numeric}:\n"
                          "  if n==0:v\n  else:tree(v,n-1)+tree(v,n-1)\n"
                          f"fn main()->{numeric}:\n  tree(1,3)\n", "8"))
        cases += [("recursive Bool", "fn tree(v:Bool,n:int)->Bool:\n  if n==0:v\n"
                   "  else:tree(v,n-1)==tree(v,n-1)\nfn main()->Bool:\n  tree(false,7)\n", "true"),
                  ("Bool", "fn main()->Bool:\n  true\n", "true"),
                  ("String", 'fn main()->String:\n  "é"+"🙂"\n', "é🙂"),
                  ("String array", 'fn main()->[String]:\n  ["é"+"🙂","retained"]\n', '["é🙂", "retained"]'),
                  ("record/enum ownership", """type Row:
  tag:uint8
  amount:uint64
  text:String
  values:[int]
enum Reading:
  Missing
  Value:
    item:Row
fn echo(value:Reading)->Reading:
  value
fn main()->int ! TaskError:
  try parallel:
    original=Reading.Value {item:Row {tag:255,amount:41,text:"é"+"🙂",values:[7,8]}}
    work=spawn echo(original)
    first=try await work
    again=try await work
    match again:
      Reading.Missing:0
      Reading.Value {item}:int(item.tag)+int(item.amount)+length(item.text)+item.values[0]
""", "305"),
                  ("expected-error channel", """error ReadError:
  Bad:
    text:String
fn read()->int ! ReadError:
  fail ReadError.Bad {text:"é"+"🙂"}
fn work()->int:
  read() catch fn(error):
    match error:
      ReadError.Bad {text}:length(text)+40
fn main()->int ! TaskError:
  try parallel:
    result=spawn work()
    try await result
""", "42")]
        cases.append(("recursive signed remainder", """fn rem(a:int,b:int)->int:
  a%b
fn gcd(a:int,b:int)->int:
  if b==0:a
  else:gcd(b,a%b)
fn main()->[int]:
  [rem(-9223372036854775808,-1),rem(-9223372036854775808,3),rem(9223372036854775807,-1),rem(-37,7),rem(37,-7),gcd(987654321,123456789)]
""", "[0, -2, 0, -2, 2, 9]"))
        # Pass values through parameters: the frontend must preserve every
        # comparison predicate rather than fold only literal expressions.
        for numeric in ("int64", "float64"):
            for a, b in ((-1, 0), (0, 0), (1, 0)):
                expected = "[" + ", ".join(str(v).lower() for v in
                    (a == b, a != b, a < b, a <= b, a > b, a >= b)) + "]"
                text = (f"fn compare(a:{numeric},b:{numeric})->[Bool]:\n"
                        "  [a==b,a!=b,a<b,a<=b,a>b,a>=b]\n"
                        f"fn main()->[Bool]:\n  compare({a},{b})\n")
                cases.append((f"{numeric} comparisons {a}/{b}", text, expected))
        for numeric in ("float32", "float64"):
            cases += [(f"{numeric} static matrix product", f"""fn main()->{numeric}:
  a:Tensor[{numeric},1,2]=tensor([[2,3]])
  b:Tensor[{numeric},2,1]=tensor([[4],[5]])
  reduceSum(matmul(a,b))
""", "23"),
                      (f"{numeric} static fused reduction", f"""fn main()->{numeric}:
  a:Tensor[{numeric},3]=tensor([1,2,3])
  reduceSum(a*3+1)
""", "21")]
        compiled = 0
        for mode, flags in modes:
            for name, text, expected in cases:
                source.write_text(text, encoding="utf-8")
                run([compiler, source, "-o", executable, *flags])
                for workers in (1, 2, 4):
                    environment = dict(os.environ, KALANDRA_WORKERS=str(workers))
                    if windows and mode == "address-undefined":
                        resources = compiler.parent.parent / "lib/kalandra/toolchain/clang/lib/windows"
                        if not resources.is_dir():
                            root = Path(os.environ["KALANDRA_LLVM_ROOT"])
                            resources, = root.glob("lib/clang/*/lib/windows")
                        environment["PATH"] = str(resources) + os.pathsep + environment.get("PATH", "")
                    result = run([executable], env=environment)
                    assert result.stdout.strip() == expected, (mode, name, workers, result.stdout, expected)
                compiled += 1
            print(f"Portable CPU {platform.system()} {mode}: {len(cases)} programs, workers 1/2/4 passed", flush=True)
        project = work / "starter project λ🙂"
        run([compiler, "new", project])
        run([compiler, "--project", project / "kalandra.toml", "-o", executable])
        assert run([executable]).stdout.strip() == "[2, 4, 6]"
        # Activate a pinned local dependency, then compile using its immutable cache.
        import shutil
        git = shutil.which("git")
        assert git, "Git is a declared package-management prerequisite"
        library = work / "library source λ🙂"
        library.mkdir()
        (library / "kalandra.toml").write_text("format=1\n[package]\nname='numbers'\nversion='1.0.0'\nentry='public.k'\n", encoding="utf-8")
        (library / "public.k").write_text("export fn answer()->int:\n  42\n", encoding="utf-8")
        git_env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="NUL" if windows else "/dev/null",
                       GIT_AUTHOR_NAME="Fixture", GIT_AUTHOR_EMAIL="fixture@example.invalid",
                       GIT_COMMITTER_NAME="Fixture", GIT_COMMITTER_EMAIL="fixture@example.invalid")
        for args in (["init", "--template=", "--initial-branch=main"], ["add", "--all"], ["commit", "-m", "fixture"]):
            run([git, "-C", library, *args], env=git_env)
        commit = run([git, "-C", library, "rev-parse", "HEAD"], env=git_env).stdout.strip()
        manifest = project / "kalandra.toml"
        manifest.write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\nnumbers={git=" + json.dumps(str(library), ensure_ascii=False) +
                            ",rev=" + json.dumps(commit) + "}\n", encoding="utf-8")
        (project / "main.k").write_text('from "numbers" import answer\nfn main()->int:\n  answer()\n', encoding="utf-8")
        run([compiler, "install", "--project", manifest])
        active = (project / "kalandra.lock").read_bytes()
        def remove_fixture_readonly(function, path, error):
            # Git marks object files read-only on Windows. This is a disposable
            # local fixture; remove it so frozen/offline tests cannot use origin.
            if not windows or not isinstance(error, PermissionError):
                raise error
            os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
            function(path)

        shutil.rmtree(library, onexc=remove_fixture_readonly)
        for flag in ("--frozen", "--offline"):
            run([compiler, "install", "--project", manifest, flag])
            assert (project / "kalandra.lock").read_bytes() == active
        run([compiler, "--project", manifest, "-o", executable])
        assert run([executable]).stdout.strip() == "42"
        prior_package_output = executable.read_bytes()
        cached_source, = (project / ".kalandra/snapshots").glob("*/source/public.k")
        original_source = cached_source.read_bytes()
        cached_source.write_text("corrupted", encoding="utf-8")
        run([compiler, "--project", manifest, "-o", executable], expected=1)
        assert executable.read_bytes() == prior_package_output
        assert (project / "kalandra.lock").read_bytes() == active
        assert run([executable]).stdout.strip() == "42"
        cached_source.write_bytes(original_source)
        # Restore the ordinary starter output for the compiler-removal check.
        run([compiler, "new", work / "independent starter"])
        run([compiler, "--project", work / "independent starter/kalandra.toml", "-o", executable])
        # A failing build must preserve a previously working output.
        prior = executable.read_bytes()
        source.write_text("fn main():\n  missing_value\n", encoding="utf-8")
        run([compiler, source, "-o", executable], expected=1)
        assert executable.read_bytes() == prior
        if windows:
            rejected = run([compiler, source, "-o", executable, "--thread-sanitize"], expected=2)
            assert "ThreadSanitizer is unavailable" in rejected.stderr
            assert executable.read_bytes() == prior
        prefix = compiler.parent.parent
        private_clang = prefix / "libexec/kalandra" / ("clang++.exe" if windows else "clang++")
        if (prefix / "SHA256SUMS").is_file():
            # Missing bundled tools must not fall back to an ambient LLVM install.
            source.write_text("fn main()->int:\n  42\n", encoding="utf-8")
            unavailable_clang = private_clang.with_name(private_clang.name + ".unavailable")
            private_clang.rename(unavailable_clang)
            try:
                rejected = run([compiler, source, "-o", executable], expected=1)
                assert "bundled Clang or its compiler resources are missing" in rejected.stderr
                assert executable.read_bytes() == prior
            finally:
                unavailable_clang.rename(private_clang)
        # Ordinary applications must remain executable after removing the compiler.
        if (prefix / "SHA256SUMS").is_file():
            hidden = prefix.with_name(prefix.name + ".hidden")
            prefix.rename(hidden)
            try:
                assert run([executable]).stdout.strip() == "[2, 4, 6]"
            finally:
                hidden.rename(prefix)
        print(json.dumps({"platform": platform.platform(), "programsCompiled": compiled + 3,
                          "numericTypes": list(values), "modes": [name for name, _ in modes],
                          "workerCounts": [1, 2, 4], "starter": "passed"}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    qualify(parser.parse_args().compiler)
