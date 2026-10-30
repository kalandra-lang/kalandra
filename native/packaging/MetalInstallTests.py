"""Relocated Metal compiler builds GPU applications outside its checkout."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
from MetalPackageTests import qualify_packages

cmake, build, config, packager, fixtures = sys.argv[1:]
build, fixtures = Path(build), Path(fixtures)

def run(args, cwd, expected=0, env=None):
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, capture_output=True, text=True, timeout=90)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

with tempfile.TemporaryDirectory(prefix="kalandra metal install ") as temporary:
    cwd = Path(temporary)
    output = cwd / "artifacts"
    run([cmake, f"-DBUILD_DIR={build}", f"-DCONFIG={config}", f"-DOUTPUT_DIR={output}",
         "-DPACKAGE_CPU=ON", "-DPACKAGE_METAL=ON", "-P", packager], cwd)
    archive = output / "kalandra-native-metal-macos-arm64.tar.gz"
    assert archive.with_suffix(".gz.sha256").read_text().split()[0] == digest(archive)
    original_archive = digest(archive)
    sidecar = archive.with_suffix(".gz.sha256").read_bytes()
    run([cmake, f"-DBUILD_DIR={build}", "-DCONFIG=NotConfigured", f"-DOUTPUT_DIR={output}",
         "-DPACKAGE_CPU=ON", "-DPACKAGE_METAL=ON", "-P", packager], cwd, 1)
    assert digest(archive) == original_archive and archive.with_suffix(".gz.sha256").read_bytes() == sidecar
    with tarfile.open(archive) as bundle:
        assert all(member.isdir() or member.isfile() for member in bundle.getmembers())
        bundle.extractall(cwd / "relocated prefix", filter="data")
    shutil.rmtree(output / "package-stage")
    prefix = cwd / "relocated prefix/kalandra-native-metal"
    steel_notice=Path(__file__).parents[1]/'third_party/mlx-steel/LICENSE'
    assert (prefix/'share/doc/kalandra-native-metal/MLX-Steel-LICENSE.txt').read_bytes()==steel_notice.read_bytes()
    names = set()
    for line in (prefix / "SHA256SUMS").read_text().splitlines():
        expected, name = line.split("  ", 1)
        assert name not in names and (prefix / name).resolve().is_relative_to(prefix.resolve())
        names.add(name)
        assert digest(prefix / name) == expected
    assert names == {str(p.relative_to(prefix)) for p in prefix.rglob("*") if p.is_file()} - {"SHA256SUMS"}
    resources = prefix / "lib/kalandra/native"
    assert len(list(resources.glob("*.a"))) == 35
    metadata = json.loads((prefix / "share/kalandra-native-metal/toolchain.json").read_text())
    assert metadata["format"] == 1 and metadata["configuration"] == config
    assert metadata["llvmVersion"] == json.loads((Path(__file__).parents[1] / "toolchain-proof/toolchain.json").read_text())["llvmVersion"] and metadata["externalDependencies"] is True
    for name in ("kalandra-metal-strict", "kalandra-metal-strict64", "kalandra-metal-extract"):
        assert os.access(resources / name, os.X_OK)
        assert run(["lipo", "-archs", resources / name], cwd).stdout.strip() == "arm64"
    assert not (prefix / "bin/kalandrac").exists()
    compiler = cwd / "compiler symlink"
    compiler.symlink_to(prefix / "bin/kalandra")
    starter = cwd / "installed starter"
    run([compiler, "new", starter], cwd)
    run([compiler, "--project", starter / "kalandra.toml", "-o", cwd / "starter program"], cwd)
    assert run([cwd / "starter program"], cwd).stdout.strip() == "[2, 4, 6]"
    (cwd / "cpu.k").write_text("fn main() -> int:\n  42\n")
    run([compiler, cwd / "cpu.k", "-o", cwd / "cpu"], cwd)
    assert run([cwd / "cpu"], cwd).stdout.strip() == "42"
    source = cwd / "placed.k"
    source.write_bytes((fixtures / "placed.k").read_bytes())
    applications = []
    assert (resources / "headers/include/kalandra/TensorTransform.h").is_file()
    # The installed compiler must compile direct native transform kernels and
    # its relocated private Metal.h must find the transform type declaration.
    transform_source = cwd / "row-transform.k"
    transform_source.write_text("fn work(x:Tensor[float32,?,?])->float32:\n  reduceSum(transpose(softmaxRows(x)))\nfn main()->float32 ! TaskError:\n  try parallel:\n    gpu=devices.gpu()\n    x:Tensor[float32,2,3]=tensor([[1,2,3],[-1,0,1]])\n    a=try await transfer(x,gpu)\n    try await spawn work(a) on gpu\n")
    transform_app = cwd / "transform application"
    run([compiler, transform_source, "--target", "metal", "--arithmetic", "hardware", "-O2", "-o", transform_app], cwd)
    assert run([transform_app / "program"], cwd).stdout.strip() == "40000000"
    applications.append((transform_app, "40000000"))
    for name, flags in (("debug", []), ("optimized", ["-O2"]),
                        ("sanitized", ["--sanitize"]), ("thread-sanitized", ["--thread-sanitize"])):
        app = cwd / ("application " + name)
        run([compiler, source, "--target", "metal", "-o", app, *flags], cwd)
        assert run([app / "program"], cwd).stdout.strip() == "[40a00000,41400000,41a80000]"
        deps = run(["otool", "-L", app / "program"], cwd).stdout
        assert "libLLVM" not in deps and "libMLIR" not in deps
        applications.append((app, "[40a00000,41400000,41a80000]"))
    # Composed source entries also retain their supplemental reduction shader when moved.
    composed_source = cwd / "pre-reduction.k"
    composed_source.write_bytes((fixtures / "pre-reduction.k").read_bytes())
    composed_app = cwd / "composed application"
    run([compiler, composed_source, "--target", "metal", "-o", composed_app, "-O2"], cwd)
    assert (composed_app / "kernel0-reduce.metallib").is_file()
    assert run([composed_app / "program"], cwd).stdout.strip() == "42960000"
    applications.append((composed_app, "42960000"))
    post_source = cwd / "post-reduction.k"
    post_source.write_bytes((fixtures / "post-reduction.k").read_bytes())
    post_app = cwd / "post reduction application"
    run([compiler, post_source, "--target", "metal", "-o", post_app, "-O2"], cwd)
    assert (post_app / "kernel0-post.metallib").is_file()
    assert run([post_app / "program"], cwd).stdout.strip() == "c1d00000"
    applications.append((post_app, "c1d00000"))
    matrix_source = cwd / "post-matmul.k"
    matrix_source.write_bytes((fixtures / "post-matmul.k").read_bytes())
    matrix_app = cwd / "post matrix application"
    run([compiler, matrix_source, "--target", "metal", "-o", matrix_app, "-O2"], cwd)
    assert (matrix_app / "kernel0-post.metallib").is_file()
    assert (matrix_app/'THIRD_PARTY_NOTICES.txt').read_bytes()==steel_notice.read_bytes()
    assert 'matmul_policy=optimized' in (matrix_app/'manifest.txt').read_text()
    assert 'kalandra_matmul_steel_' in (matrix_app/'kernel0.metal').read_text()
    matrix_result = "[42380000,42700000,42d00000,43080000]"
    assert run([matrix_app / "program"], cwd).stdout.strip() == matrix_result
    applications.append((matrix_app, matrix_result))
    pre_source = cwd / "pre-matmul.k"
    pre_source.write_bytes((fixtures / "pre-matmul.k").read_bytes())
    pre_app = cwd / "matrix operand application"
    run([compiler, pre_source, "--target", "metal", "-o", pre_app, "-O2"], cwd)
    for stage in ("left", "right", "post"):
        assert (pre_app / f"kernel0-{stage}.metallib").is_file()
    pre_result = "[42680000,42a80000,42c40000,430e0000]"
    assert run([pre_app / "program"], cwd).stdout.strip() == pre_result
    applications.append((pre_app, pre_result))
    sum_source = cwd / "matmul-reduction.k"
    sum_source.write_bytes((fixtures / "matmul-reduction.k").read_bytes())
    sum_app = cwd / "matrix reduction application"
    run([compiler, sum_source, "--target", "metal", "-o", sum_app, "-O2"], cwd)
    for stage in ("left", "right", "reduce"):
        assert (sum_app / f"kernel0-{stage}.metallib").is_file()
    assert run([sum_app / "program"], cwd).stdout.strip() == "43350000"
    applications.append((sum_app, "43350000"))
    adjusted_source = cwd / "matmul-adjust-reduction.k"
    adjusted_source.write_bytes((fixtures / "matmul-adjust-reduction.k").read_bytes())
    adjusted_app = cwd / "adjusted matrix reduction application"
    run([compiler, adjusted_source, "--target", "metal", "-o", adjusted_app, "-O2"], cwd)
    for stage in ("left", "right", "post", "reduce"):
        assert (adjusted_app / f"kernel0-{stage}.metallib").is_file()
    assert run([adjusted_app / "program"], cwd).stdout.strip() == "43bf0000"
    applications.append((adjusted_app, "43bf0000"))
    scalar_source = cwd / "matmul-scalar-reduction.k"
    scalar_source.write_bytes((fixtures / "matmul-scalar-reduction.k").read_bytes())
    scalar_app = cwd / "matrix scalar reduction application"
    run([compiler, scalar_source, "--target", "metal", "-o", scalar_app, "-O2"], cwd)
    for stage in ("left", "right", "post", "reduce", "scalar"):
        assert (scalar_app / f"kernel0-{stage}.metallib").is_file()
    assert run([scalar_app / "program"], cwd).stdout.strip() == "c3400000"
    applications.append((scalar_app, "c3400000"))
    graph_source = cwd / "matrix-graph.k"
    graph_source.write_bytes((fixtures / "matrix-graph.k").read_bytes())
    graph_app = cwd / "matrix graph application"
    run([compiler, graph_source, "--target", "metal", "-o", graph_app, "-O2"], cwd)
    graph_result = "[42d40000,431c0000,43710000,43b10000]"
    assert run([graph_app / "program"], cwd).stdout.strip() == graph_result
    applications.append((graph_app, graph_result))
    graph_sum_source = cwd / "matrix-graph-reduction.k"
    graph_sum_source.write_bytes((fixtures / "matrix-graph-reduction.k").read_bytes())
    graph_sum_app = cwd / "matrix graph reduction application"
    run([compiler, graph_sum_source, "--target", "metal", "-o", graph_sum_app, "-O2"], cwd)
    for stage in ("reduce", "scalar"):
        assert (graph_sum_app / f"kernel0-{stage}.metallib").is_file()
    assert run([graph_sum_app / "program"], cwd).stdout.strip() == "c3d6c000"
    applications.append((graph_sum_app, "c3d6c000"))
    mixed_source = cwd / "matrix-graph-arithmetic.k"
    mixed_source.write_bytes((fixtures / "matrix-graph-arithmetic.k").read_bytes())
    mixed_app = cwd / "mixed matrix graph application"
    run([compiler, mixed_source, "--target", "metal", "-o", mixed_app, "-O2"], cwd)
    for stage in (1, 2, 3):
        assert (mixed_app / f"kernel0-map{stage}.metallib").is_file()
    mixed_result = "[c2620000,c2a60000,c3000000,c33c0000]"
    assert run([mixed_app / "program"], cwd).stdout.strip() == mixed_result
    applications.append((mixed_app, mixed_result))
    multi_source = cwd / "multiple-reductions.k"
    multi_source.write_bytes((fixtures / "multiple-reductions.k").read_bytes())
    multi_app = cwd / "multiple reduction application"
    run([compiler, multi_source, "--target", "metal", "-o", multi_app, "-O2"], cwd)
    for stage in ("reduce", "join"):
        assert (multi_app / f"kernel0-{stage}.metallib").is_file()
    assert run([multi_app / "program"], cwd).stdout.strip() == "c3ad8000"
    applications.append((multi_app, "c3ad8000"))
    feedback_source = cwd / "reduction-feedback.k"
    feedback_source.write_bytes((fixtures / "reduction-feedback.k").read_bytes())
    feedback_app = cwd / "reduction feedback application"
    run([compiler, feedback_source, "--target", "metal", "-o", feedback_app, "-O2"], cwd)
    for stage in ("reduce", "join", "map0", "map1"):
        assert (feedback_app / f"kernel0-{stage}.metallib").is_file()
    assert run([feedback_app / "program"], cwd).stdout.strip() == "402aaaab"
    applications.append((feedback_app, "402aaaab"))
    tensor_source = cwd / "tensor-feedback.k"
    tensor_source.write_bytes((fixtures / "tensor-feedback.k").read_bytes())
    tensor_app = cwd / "tensor feedback application"
    run([compiler, tensor_source, "--target", "metal", "-o", tensor_app, "-O2"], cwd)
    assert (tensor_app / "kernel0-map0.metallib").is_file()
    assert not (tensor_app / "kernel0-join.metallib").exists()
    tensor_result = "[3f000000,3f800000,3fc00000]"
    assert run([tensor_app / "program"], cwd).stdout.strip() == tensor_result
    applications.append((tensor_app, tensor_result))
    control_source = cwd / "scalar-control.k"
    control_source.write_bytes((fixtures / "scalar-control.k").read_bytes())
    control_app = cwd / "scalar control application"
    run([compiler, control_source, "--target", "metal", "-o", control_app, "-O2"], cwd)
    assert (control_app / "kernel1-map0.metallib").is_file()
    control_result = "[3e2aaaab,3eaaaaab,3f000000]"
    assert run([control_app / "program"], cwd).stdout.strip() == control_result
    applications.append((control_app, control_result))
    tensor_control_source = cwd / "tensor-control.k"
    tensor_control_source.write_bytes((fixtures / "tensor-control.k").read_bytes())
    tensor_control_app = cwd / "tensor control application"
    run([compiler, tensor_control_source, "--target", "metal", "-o", tensor_control_app, "-O2"], cwd)
    assert (tensor_control_app / "kernel1-map0.metallib").is_file()
    assert not (tensor_control_app / "kernel1-map1.metallib").exists()
    assert run([tensor_control_app / "program"], cwd).stdout.strip() == control_result
    applications.append((tensor_control_app, control_result))
    sources_source = cwd / "tensor-sources.k"
    sources_source.write_bytes((fixtures / "tensor-sources.k").read_bytes())
    sources_app = cwd / "tensor sources application"
    run([compiler, sources_source, "--target", "metal", "-o", sources_app, "-O2"], cwd)
    assert (sources_app / "kernel1-map0.metallib").is_file()
    assert run([sources_app / "program"], cwd).stdout.strip() == control_result
    applications.append((sources_app, control_result))
    dynamic_source = cwd / "dynamic-choices.k"
    dynamic_source.write_bytes((fixtures / "dynamic-choices.k").read_bytes())
    dynamic_app = cwd / "dynamic choices application"
    run([compiler, dynamic_source, "--target", "metal", "-o", dynamic_app, "-O2"], cwd)
    assert (dynamic_app / "kernel1-map1.metallib").is_file()
    dynamic_result = "[3e7b823f,3eaaaaab,3ed79436]"
    assert run([dynamic_app / "program"], cwd).stdout.strip() == dynamic_result
    applications.append((dynamic_app, dynamic_result))
    nary_source = cwd / "nary-choices.k"
    nary_source.write_bytes((fixtures / "nary-choices.k").read_bytes())
    nary_app = cwd / "nary choices application"
    run([compiler, nary_source, "--target", "metal", "-o", nary_app, "-O2"], cwd)
    assert (nary_app / "kernel1-map2.metallib").is_file()
    nary_result = "[3d20a0a1,3da0a0a1,3df0f0f1]"
    assert run([nary_app / "program"], cwd).stdout.strip() == nary_result
    applications.append((nary_app, nary_result))
    operand_source = cwd / "operand-choices.k"
    operand_source.write_bytes((fixtures / "operand-choices.k").read_bytes())
    operand_app = cwd / "operand choices application"
    run([compiler, operand_source, "--target", "metal", "-o", operand_app, "-O2"], cwd)
    assert (operand_app / "kernel0-left.metallib").is_file()
    assert (operand_app / "kernel0-right.metallib").is_file()
    operand_result = "[41b00000,41e00000,42440000,42800000]"
    assert run([operand_app / "program"], cwd).stdout.strip() == operand_result
    applications.append((operand_app, operand_result))
    result_source = cwd / "result-choices.k"
    result_source.write_bytes((fixtures / "result-choices.k").read_bytes())
    result_app = cwd / "matrix result choices application"
    run([compiler, result_source, "--target", "metal", "-o", result_app, "-O2"], cwd)
    assert (result_app / "kernel0-post.metallib").is_file()
    result_bits = "[42300000,42600000,42c40000,43000000]"
    assert run([result_app / "program"], cwd).stdout.strip() == result_bits
    applications.append((result_app, result_bits))
    choice_source = cwd / "reduction-choices.k"
    choice_source.write_bytes((fixtures / "reduction-choices.k").read_bytes())
    choice_app = cwd / "reduction choices application"
    run([compiler, choice_source, "--target", "metal", "-o", choice_app, "-O2"], cwd)
    assert (choice_app / "kernel0-post.metallib").is_file()
    assert run([choice_app / "program"], cwd).stdout.strip() == "41400000"
    applications.append((choice_app, "41400000"))
    # Every numeric type traverses resident reductions, selected stages,
    # conversion and a rank-eight feedback map in the relocated compiler.
    numeric_names = ("int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64", "float32", "float64")
    for numeric in numeric_names:
        typed_source = cwd / f"typed-{numeric}.k"
        typed_source.write_text(f"""fn increment(value:int32)->int32:
  value+1
fn operation(values:Tensor[{numeric},?,?,?,?,?,?,?,?],scale:{numeric})->float64:
  amount=reduceSum(values)
  if scale==1:
    float64(reduceSum(values+amount))
  else:
    float64(reduceSum(values)/scale)
fn main()->float64 ! TaskError:
  try parallel:
    worker=spawn increment(0)
    gpu=devices.gpu()
    values:Tensor[{numeric},1,1,1,1,1,1,1,3]=tensor([[[[[[[[1,2,3]]]]]]]])
    resident=try await transfer(values,gpu)
    scale=try await worker
    try await spawn operation(resident,{numeric}(scale)) on gpu
""")
        for mode, flags in (("debug", []), ("optimized", ["-O2"]),
                            ("sanitized", ["--sanitize"]), ("thread-sanitized", ["--thread-sanitize"])):
            typed_app = cwd / f"typed {numeric} {mode}"
            run([compiler, typed_source, "--target", "metal", "--arithmetic", "hardware-with-software-fallback", "-o", typed_app, *flags], cwd)
            assert run([typed_app / "program"], cwd).stdout.strip() == "4038000000000000"
            applications.append((typed_app, "4038000000000000"))
    matrix_control_source = cwd / "selected matrix shapes.k"
    matrix_control_source.write_text("""fn operation(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:
  product=matmul(a,b)
  if reduceSum(product)>0:
    product
  else:
    product+a
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,2,3]=tensor([[1,2,1],[1,1,1]])
    b:Tensor[float32,3,2]=tensor([[1,1],[1,1],[1,1]])
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    answer=try await spawn operation(x,y) on gpu
    try await transfer(answer,devices.cpu())
""")
    matrix_control_app = cwd / "selected matrix application"
    run([compiler, matrix_control_source, "--target", "metal", "-o", matrix_control_app, "-O2"], cwd)
    matrix_control_result = "[40800000,40800000,40400000,40400000]"
    assert run([matrix_control_app / "program"], cwd).stdout.strip() == matrix_control_result
    assert "numeric_program_stages=" in (matrix_control_app / "manifest.txt").read_text()
    applications.append((matrix_control_app, matrix_control_result))
    # The strict64 resource is independently required and failed compilation
    # must preserve the last complete application.
    strict64 = resources / "kalandra-metal-strict64"
    strict64_saved = strict64.read_bytes()
    strict64.unlink()
    saved_typed = (typed_app / "program").read_bytes()
    missing64 = run([compiler, typed_source, "--target", "metal", "--arithmetic", "hardware-with-software-fallback", "-o", typed_app], cwd, 1)
    assert "installed Metal resource is missing" in missing64.stderr
    assert (typed_app / "program").read_bytes() == saved_typed
    strict64.write_bytes(strict64_saved)
    strict64.chmod(0o755)
    # Local module/project imports go through the same installed Metal compiler.
    (cwd / "helper.k").write_text("export " + source.read_text().split("\n\nfn main", 1)[0] + "\n")
    source.write_text('from "math" import combine\n\nfn main' + source.read_text().split("\n\nfn main", 1)[1])
    manifest = cwd / "kalandra.toml"
    manifest.write_text('format=1\n[project]\nentry="placed.k"\n[packages]\nmath="helper.k"\n')
    app = cwd / "project application"
    run([compiler, "--project", manifest, "--target", "metal", "-o", app], cwd)
    assert run([app / "program"], cwd).stdout.strip() == "[40a00000,41400000,41a80000]"
    applications.append((app, "[40a00000,41400000,41a80000]"))
    applications.extend(qualify_packages(compiler, prefix / "bin/kalandra-check", cwd, run))
    saved = (app / "program").read_bytes()
    helper = resources / "kalandra-metal-strict"
    helper.unlink()
    result = run([compiler, "--project", manifest, "--target", "metal", "-o", app], cwd, 1)
    assert "installed Metal resource is missing" in result.stderr
    assert (app / "program").read_bytes() == saved
    # Generated applications retain only their own shaders and system dependencies.
    shutil.rmtree(prefix)
    for i, (app, expected) in enumerate(applications):
        moved = cwd / f"moved application {i}"
        app.rename(moved)
        assert run([moved / "program"], cwd).stdout.strip() == expected
print("Metal bundle: checksums, relocation/symlink, CPU compile, all ten types/rank-eight resident graphs/conversions in four GPU modes, scoped managed packages with CPU workers, named updates/private pins/rollback, frozen/offline relocation, source integrity, project import, missing helper and independent moved applications passed")
