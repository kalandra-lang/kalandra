"""Execute the native CPU language/runtime contract on Linux and Windows."""
from VersionCheck import expected_version, verify_binary_version
from WindowsPlatform import has_sanitizers, pe_machine, rename_after_process_exit
from LoopCases import CASES as LOOP_CASES
from FileSystemCases import SOURCE as FILESYSTEM_SOURCE, EXPECTED as FILESYSTEM_EXPECTED
from ParseCases import SOURCE as PARSE_SOURCE, EXPECTED as PARSE_EXPECTED
from PathCases import SOURCE as PATH_SOURCE, expected as path_expected
from TimeCases import SOURCE as TIME_SOURCE, EXPECTED as TIME_EXPECTED
from CallCases import SOURCE as CALL_SOURCE, EXPECTED as CALL_EXPECTED
from BufferCases import SOURCE as BUFFER_SOURCE, EXPECTED as BUFFER_EXPECTED
from EnvironmentCases import SOURCE as ENV_SOURCE, expected as env_expected, with_environment
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
    verify_binary_version(compiler, expected_version(), "kalandra")
    windows = platform.system() == "Windows"
    modes = [("normal", []), ("optimized", ["-O2"]), ("address-undefined", ["--sanitize"])]
    windows_arm64 = windows and not has_sanitizers(compiler)
    if windows_arm64:
        modes = [mode for mode in modes if mode[0] != "address-undefined"]
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

        cases = [("loop " + name, text, expected) for name, (text, expected) in LOOP_CASES.items()]
        values = {"i8": "127", "u8": "255", "i16": "32767", "u16": "65535",
                  "i32": "2147483647", "u32": "4294967295",
                  "i64": "9223372036854775807", "u64": "18446744073709551615",
                  "f32": "1.5", "f64": "1.5"}
        rank8_values = "[" * 8 + "1,2" + "]" * 8
        for numeric, value in values.items():
            cases += [(f"{numeric} scalar", f'fn main()->{numeric}:\n  return {value}\n', value),
                      (f"{numeric} array", f'fn main()->[{numeric}]:\n  return [{value},0]\n', f"[{value}, 0]"),
                      (f"{numeric} tensor", f'fn main()->Tensor[{numeric},2,2]:\n  return tensor([[1,2],[3,4]])*2\n',
                       "tensor([2, 4, 6, 8], [2, 2])"),
                      (f"{numeric} rank-zero tensor", f'fn main()->Tensor[{numeric}]:\n  return tensor(1)*2\n',
                       "tensor([2], [])"),
                      (f"{numeric} rank-eight tensor", f'fn main()->Tensor[{numeric},1,1,1,1,1,1,1,2]:\n  return tensor({rank8_values})*2\n',
                       "tensor([2, 4], [1, 1, 1, 1, 1, 1, 1, 2])"),
                      (f"{numeric} worker", f'fn echo(value:{numeric})->{numeric}:\n  return value\nfn main()->{numeric} ! TaskError:\n  return try parallel:\n    work=spawn echo({value})\n    first=try await work\n    again=try await work\n    if first==again:again\n    else:0\n', value)]
            cases.append((f"{numeric} recursive scalar", f'fn tree(v:{numeric},n:i64)->{numeric}:\n  return if n==0:v\n  else:tree(v,n-1)+tree(v,n-1)\nfn main()->{numeric}:\n  return tree(1,3)\n', "8"))
        cases += [("recursive Bool", 'fn tree(v:bool,n:i64)->bool:\n  return if n==0:v\n  else:tree(v,n-1)==tree(v,n-1)\nfn main()->bool:\n  return tree(false,7)\n', "true"),
                  ("bool", 'fn main()->bool:\n  return true\n', "true"),
                  ("String", 'fn main()->String:\n  return "é"+"🙂"\n', "é🙂"),
                  ("String array", 'fn main()->[String]:\n  return ["é"+"🙂","retained"]\n', '["é🙂", "retained"]'),
                  ("record/enum ownership", """type Row:
  tag:u8
  amount:u64
  text:String
  values:[i64]
enum Reading:
  Missing
  Value:
    item:Row
fn echo(value:Reading)->Reading:
  return value
fn main()->i64 ! TaskError:
  return try parallel:
    original=Reading.Value {item:Row {tag:255,amount:41,text:"é"+"🙂",values:[7,8]}}
    work=spawn echo(original)
    first=try await work
    again=try await work
    match again:
      Reading.Missing:0
      Reading.Value {item}:i64(item.tag)+i64(item.amount)+length(item.text)+item.values[0]
""", "305"),
                  ("expected-error channel", 'error ReadError:\n  Bad:\n    text:String\nfn read()->i64 ! ReadError:\n  fail ReadError.Bad {text:"é"+"🙂"}\nfn work()->i64:\n  return read() catch fn(error):\n    return match error:\n      ReadError.Bad {text}:length(text)+40\nfn main()->i64 ! TaskError:\n  return try parallel:\n    result=spawn work()\n    try await result\n', "42")]
        cases.append(("recursive signed remainder", """fn rem(a:i64,b:i64)->i64:
  return a%b
fn gcd(a:i64,b:i64)->i64:
  return if b==0:a
  else:gcd(b,a%b)
fn main()->[i64]:
  return [rem(-9223372036854775808,-1),rem(-9223372036854775808,3),rem(9223372036854775807,-1),rem(-37,7),rem(37,-7),gcd(987654321,123456789)]
""", "[0, -2, 0, -2, 2, 9]"))
        # Pass values through parameters: the frontend must preserve every
        # comparison predicate rather than fold only literal expressions.
        for numeric in ("i64", "f64"):
            for a, b in ((-1, 0), (0, 0), (1, 0)):
                expected = "[" + ", ".join(str(v).lower() for v in
                    (a == b, a != b, a < b, a <= b, a > b, a >= b)) + "]"
                text = (f'fn compare(a:{numeric},b:{numeric})->[bool]:\n  return [a==b,a!=b,a<b,a<=b,a>b,a>=b]\nfn main()->[bool]:\n  return compare({a},{b})\n')
                cases.append((f"{numeric} comparisons {a}/{b}", text, expected))
        for numeric in ("f32", "f64"):
            cases += [(f"{numeric} static matrix product", f"""fn main()->{numeric}:
  a:Tensor[{numeric},1,2]=tensor([[2,3]])
  b:Tensor[{numeric},2,1]=tensor([[4],[5]])
  return reduceSum(matmul(a,b))
""", "23"),
                      (f"{numeric} static fused reduction", f"""fn main()->{numeric}:
  a:Tensor[{numeric},3]=tensor([1,2,3])
  return reduceSum(a*3+1)
""", "21")]
        compiled = 0
        for mode, flags in modes:
            for name, text, expected in cases:
                source.write_text(text, encoding="utf-8")
                run([compiler, "--test-entry", "main", source, "-o", executable, *flags])
                if windows:
                    assert pe_machine(executable) == pe_machine(compiler), "Generated program architecture mismatch"
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
        # Real applications use the public entry ABI, not numerical observation.
        source.write_text('from "std/io" import println, IoError\nfrom "std/fmt" import format\n'
                          'from "std/process" import ExitStatus\n'
                          'fn main(args:[String])->ExitStatus ! IoError:\n'
                          '  try println(format("{}:{}",length(args),args[0]))\n'
                          '  return ExitStatus {code:42}\n', encoding="utf-8")
        for flags in ([], ["-O2"]):
            run([compiler, source, "-o", executable, *flags])
            assert run([executable, "é🙂"], expected=42).stdout.strip() == "1:é🙂"
        # Normal filesystem entry, owned handles and buffered transport must also
        # work from a downloaded package, not only the source-tree compiler.
        source.write_text(FILESYSTEM_SOURCE, encoding="utf-8")
        for mode, flags in modes:
            run([compiler, source, "-o", executable, *flags])
            environment = dict(os.environ)
            if windows and mode == "address-undefined":
                resources = compiler.parent.parent / "lib/kalandra/toolchain/clang/lib/windows"
                if not resources.is_dir():
                    resources, = Path(os.environ["KALANDRA_LLVM_ROOT"]).glob("lib/clang/*/lib/windows")
                environment["PATH"] = str(resources) + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([str(executable)], cwd=work, capture_output=True,
                encoding="utf-8", timeout=60, env=environment)
            assert result.returncode == 0 and not result.stderr and result.stdout == FILESYSTEM_EXPECTED, result
        source.write_text(PARSE_SOURCE, encoding="utf-8")
        for mode, flags in modes:
            run([compiler, source, "-o", executable, *flags])
            environment = dict(os.environ)
            if windows and mode == "address-undefined":
                resources = compiler.parent.parent / "lib/kalandra/toolchain/clang/lib/windows"
                if not resources.is_dir():
                    resources, = Path(os.environ["KALANDRA_LLVM_ROOT"]).glob("lib/clang/*/lib/windows")
                environment["PATH"] = str(resources) + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([str(executable)], cwd=work, capture_output=True,
                encoding="utf-8", timeout=60, env=environment)
            assert result.returncode == 0 and not result.stderr and result.stdout == PARSE_EXPECTED, result
        for app_source, expected, configure in (
            (PATH_SOURCE, path_expected(windows), lambda env: env),
            (TIME_SOURCE, TIME_EXPECTED, lambda env: env),
            (CALL_SOURCE, CALL_EXPECTED, lambda env: env),
            (BUFFER_SOURCE, BUFFER_EXPECTED, lambda env: env),
            (ENV_SOURCE, env_expected(windows), lambda env: with_environment(env, windows=windows)),
        ):
            source.write_text(app_source, encoding="utf-8")
            for mode, flags in modes:
                run([compiler, source, "-o", executable, *flags])
                environment = dict(os.environ)
                if windows and mode == "address-undefined":
                    resources = compiler.parent.parent / "lib/kalandra/toolchain/clang/lib/windows"
                    if not resources.is_dir():
                        resources, = Path(os.environ["KALANDRA_LLVM_ROOT"]).glob("lib/clang/*/lib/windows")
                    environment["PATH"] = str(resources) + os.pathsep + environment.get("PATH", "")
                result = subprocess.run([str(executable)], cwd=work, capture_output=True,
                    encoding="utf-8", timeout=60, env=configure(environment))
                assert result.returncode == 0 and not result.stderr and result.stdout == expected, result
        project = work / "starter project λ🙂"
        run([compiler, "new", project])
        run([compiler, "--project", project / "kalandra.toml", "-o", executable])
        assert run([executable]).stdout.strip() == "2\n4\n6"
        # Activate a pinned local dependency, then compile using its immutable cache.
        import shutil
        git = shutil.which("git")
        assert git, "Git is a declared package-management prerequisite"
        library = work / "library source λ🙂"
        library.mkdir()
        (library / "kalandra.toml").write_text("format=1\n[package]\nname='numbers'\nversion='1.0.0'\nentry='public.k'\n", encoding="utf-8")
        (library / "public.k").write_text('export fn answer()->i64:\n  return 42\n', encoding="utf-8")
        git_env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="NUL" if windows else "/dev/null",
                       GIT_AUTHOR_NAME="Fixture", GIT_AUTHOR_EMAIL="fixture@example.invalid",
                       GIT_COMMITTER_NAME="Fixture", GIT_COMMITTER_EMAIL="fixture@example.invalid")
        for args in (["init", "--template=", "--initial-branch=main"], ["add", "--all"], ["commit", "-m", "fixture"]):
            run([git, "-C", library, *args], env=git_env)
        commit = run([git, "-C", library, "rev-parse", "HEAD"], env=git_env).stdout.strip()
        manifest = project / "kalandra.toml"
        manifest.write_text("format=1\n[project]\nentry='main.k'\n[dependencies]\nnumbers={git=" + json.dumps(str(library), ensure_ascii=False) +
                            ",rev=" + json.dumps(commit) + "}\n", encoding="utf-8")
        (project / "main.k").write_text('from "numbers" import answer\nfn main()->i64:\n  return answer()\n', encoding="utf-8")
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
        run([compiler, "--test-entry", "main", "--project", manifest, "-o", executable])
        assert run([executable]).stdout.strip() == "42"
        prior_package_output = executable.read_bytes()
        cached_source, = (project / ".kalandra/snapshots").glob("*/source/public.k")
        original_source = cached_source.read_bytes()
        cached_source.write_text("corrupted", encoding="utf-8")
        run([compiler, "--test-entry", "main", "--project", manifest, "-o", executable], expected=1)
        assert executable.read_bytes() == prior_package_output
        assert (project / "kalandra.lock").read_bytes() == active
        assert run([executable]).stdout.strip() == "42"
        cached_source.write_bytes(original_source)
        # Restore the ordinary starter output for the compiler-removal check.
        run([compiler, "new", work / "independent starter"])
        run([compiler, "--project", work / "independent starter/kalandra.toml", "-o", executable])
        # A failing build must preserve a previously working output.
        prior = executable.read_bytes()
        source.write_text('fn main():\n  return missing_value\n', encoding="utf-8")
        run([compiler, "--test-entry", "main", source, "-o", executable], expected=1)
        assert executable.read_bytes() == prior
        if windows_arm64:
            rejected = run([compiler, "--test-entry", "main", source, "-o", executable, "--sanitize"], expected=2)
            assert "ASan/UBSan are unavailable" in rejected.stderr
            assert executable.read_bytes() == prior
        if windows:
            rejected = run([compiler, "--test-entry", "main", source, "-o", executable, "--thread-sanitize"], expected=2)
            assert "ThreadSanitizer is unavailable" in rejected.stderr
            assert executable.read_bytes() == prior
        prefix = compiler.parent.parent
        private_clang = prefix / "libexec/kalandra" / ("clang++.exe" if windows else "clang++")
        if (prefix / "SHA256SUMS").is_file():
            # Missing bundled tools must not fall back to an ambient LLVM install.
            source.write_text('fn main()->i64:\n  return 42\n', encoding="utf-8")
            unavailable_clang = private_clang.with_name(private_clang.name + ".unavailable")
            rename_after_process_exit(private_clang, unavailable_clang)
            try:
                rejected = run([compiler, "--test-entry", "main", source, "-o", executable], expected=1)
                assert "bundled Clang or its compiler resources are missing" in rejected.stderr
                assert executable.read_bytes() == prior
            finally:
                rename_after_process_exit(unavailable_clang, private_clang)
        # Ordinary applications must remain executable after removing the compiler.
        if (prefix / "SHA256SUMS").is_file():
            hidden = prefix.with_name(prefix.name + ".hidden")
            rename_after_process_exit(prefix, hidden)
            try:
                assert run([executable]).stdout.strip() == "2\n4\n6"
            finally:
                rename_after_process_exit(hidden, prefix)
        print(json.dumps({"platform": platform.platform(), "programsCompiled": compiled + 3,
                          "numericTypes": list(values), "modes": [name for name, _ in modes],
                          "workerCounts": [1, 2, 4], "starter": "passed"}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    qualify(parser.parse_args().compiler)
