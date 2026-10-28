"""Reproduce the rejected event-gated graph prototype against commit 090d4087.

The patch is compiled in an isolated source snapshot, never the production
runtime. This is a numerical/performance experiment, not cancellation or failure
qualification of the rejected protocol. Both executables use the same compiler
emitted graph and shader libraries; the baseline bundle is never rebuilt.
"""
import argparse, io, json, os, shutil, struct, subprocess, sys, tarfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p

BASE = '090d408789b4a43a8ca80a0c2f3ddcffac2d0d83'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--blocks', type=int, default=3)
    args = parser.parse_args()
    assert args.blocks > 0 and not args.output.exists()
    for key in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):
        assert os.getenv(key) in (None, '', '0'), key
    baseline = args.baseline.resolve(); bundle = args.bundle.resolve()
    build = args.build_dir.resolve(); build.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((baseline / 'manifest.json').read_text())
    for name, record in manifest.items():
        assert p.sha(baseline / name) == record['sha256'], name
    # Use exact historical sources, including headers, instead of assuming that
    # the current checkout remains ABI-compatible with the archived prototype.
    source = build / 'source'; source.mkdir()
    archived = subprocess.check_output(['git', 'archive', BASE, 'native'], cwd=p.ROOT)
    with tarfile.open(fileobj=io.BytesIO(archived)) as archive:
        archive.extractall(source, filter='data')
    patch = Path(__file__).parent / 'fair/event_progression.patch'
    subprocess.run(['patch','--batch','-p1','-d',str(source)], input=patch.read_bytes(), check=True)
    metal = source / 'native/metal/Metal.mm'; tasks = source / 'native/runtime/Tasks.cpp'
    flags = ['-std=c++23','-O2','-pthread','-mmacosx-version-min=26.0']
    p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs',
           '-c',metal,'-o',build/'Metal.o'])
    p.run([p.CLANG,*flags,'-c',tasks,'-o',build/'Tasks.cpp.o'])
    p.run(['ar','rcs',build/'metal.a',build/'Metal.o'])
    shutil.copy2(baseline/'task.a',build/'task.a')
    p.run(['ar','r',build/'task.a',build/'Tasks.cpp.o'])
    p.run(['ranlib',build/'task.a'])
    driver = (baseline/'Driver.cpp').read_text()
    assert driver.count('m::Runtime::create()') == 1
    driver = driver.replace('m::Runtime::create()', 'm::Runtime::create({.eventProgression=true})')
    driver = driver.replace('../../metal/Metal.h', str(source/'native/metal/Metal.h'))
    (build/'Driver.cpp').write_text(driver)
    shutil.copy2(baseline/'Program.h',build/'TransformerProgram.h')
    p.run([p.CLANG,*flags,'-ffp-contract=off','-I'+str(build),
           '-I'+str(source/'native/benchmarks/fair'),build/'Driver.cpp',build/'metal.a',
           build/'task.a','-framework','Foundation','-framework','Metal','-o',build/'gpu'])
    binaries = {'baseline':baseline/'gpu','current':build/'gpu'}
    shapes = [(m,d) for d in (64,256,512) for m in (1,16,64)]
    verification = []
    for m,d,pattern in [(2,3,1),(3,7,1),(5,17,1),(17,33,1)] + [(m,d,0) for m,d in shapes]:
        for mode in range(3):
            results = [json.loads(p.run([binary,bundle,m,d,1,1,mode,1,pattern])) for binary in binaries.values()]
            packed = [struct.pack('<'+'f'*len(r['cells']),*r['cells']) for r in results]
            assert packed[0] == packed[1], (m,d,pattern,mode)
            verification.append(dict(m=m,d=d,pattern=pattern,mode=mode,cells=m*d,bit_identical=True))
    def invoke(cell, count, rounds):
        version,m,d,mode = cell
        return p.observations(p.run([binaries[version],bundle,m,d,count,rounds,mode,0,0]),m*d*4,count,rounds)
    cells = [(version,m,d,mode) for m,d in shapes for mode in range(3) for version in binaries]
    witness = Path(__file__).parent/'fair/EventGateProbe.mm'
    p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks',witness,'-framework','Metal',
           '-framework','Foundation','-o',build/'gate-witness'])
    witness_output = p.run([build/'gate-witness'],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'))
    data = p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=args.blocks,
                            target_ms=20,adaptive=True,max_iterations=5000)
    data.update(contract=__doc__,baseline_commit=BASE,baseline_manifest=manifest,
                verification=verification,production_enabled=False,
                cancellation_failure_qualification=False,gate_witness=witness_output,
                gate_witness_environment=dict(MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'),
                note='Every warmup and measured result checksum is validated. Bitwise output verification is outside timing. No numerical shader changes. The GPU command interval includes CPU event waits and cannot be interpreted as exclusive compute time.')
    sources = [Path(__file__),patch,witness,baseline/'manifest.json',build/'Driver.cpp',build/'TransformerProgram.h',metal,tasks,
               source/'native/metal/Metal.h',source/'native/runtime/Tasks.h']
    p.evidence(args.output,data,sources,[*binaries.values(),build/'gate-witness',build/'metal.a',build/'task.a',baseline/'metal.a',baseline/'task.a',*bundle.glob('*.metallib')],baseline/'compiler')

if __name__ == '__main__':
    main()
