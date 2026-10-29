"""Frozen upload acceptance, same-binary storage controls and matched MPS snapshots."""
import argparse, json, os, shutil, sys
from pathlib import Path
os.environ['OPENBLAS_NUM_THREADS'] = '1'
sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
import transformer_reference as ref

parser = argparse.ArgumentParser(description=__doc__)
for name in ('evidence', 'build-dir', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--study', choices=('paired', 'storage', 'matched', 'repeat'), required=True)
parser.add_argument('--blocks', type=int, default=5)
args = parser.parse_args()
for name in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES', 'KALANDRA_GPU_PROFILE'):
    assert os.getenv(name) in (None, '', '0'), name
assert not args.output.exists()
build = args.build_dir.resolve(); build.mkdir(parents=True, exist_ok=True)
data = json.loads(args.evidence.read_text())
accept = next(p.ROOT/f for f in data['artifact_sha256'] if f.endswith('/TransformerProgram.h')).parent
bundle = accept/'bundle'
baseline = accept/'gpu'
for f in [baseline, accept/'cpu', accept/'TransformerProgram.h', *bundle.glob('*.metallib')]:
    assert p.sha(f) == data['artifact_sha256'][p.key(f)]
fair = Path(__file__).resolve().parent/'fair'
metal = p.ROOT/'build/native/native/metal/libkalandra-metal-optimized.a'
task = p.ROOT/'build/native/native/libkalandra-task-core-optimized.a'
assert p.sha(task) == data['artifact_sha256'][p.key(task)]
assert p.sha(p.ROOT/'build/native/native/kalandra') == data['compiler_sha256']
sources = [Path(__file__), fair/'protocol.py', fair/'TransformerGpu.cpp', fair/'TransformerMps.mm',
           fair/'TransformerBlock.h', fair/'transformer_reference.py', fair/'ffn_reference.py',
           p.ROOT/'native/metal/Metal.mm', p.ROOT/'native/metal/Metal.h']
signature = {p.key(f): p.sha(f) for f in sources+[metal,task]}
manifest = build/'manifest.json'
if not manifest.exists():
    shutil.copy2(metal, build/'metal.a'); shutil.copy2(task, build/'task.a')
    flags = ['-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0']
    driver = (fair/'TransformerGpu.cpp').read_text().replace('../../metal/Metal.h',str(p.ROOT/'native/metal/Metal.h'))
    assert driver.count('ffn::check(argc==9);') == driver.count('auto made=m::Runtime::create();') == 1
    driver = driver.replace('ffn::check(argc==9);','ffn::check(argc==10);').replace('auto made=m::Runtime::create();','m::Options options; options.sharedUploads=std::stoul(argv[9])!=0; auto made=m::Runtime::create(options);')
    (build/'Driver.cpp').write_text(driver)
    p.run([p.CLANG,*flags,'-I'+str(accept),'-I'+str(fair),build/'Driver.cpp',build/'metal.a',build/'task.a','-framework','Foundation','-framework','Metal','-o',build/'gpu'])
    p.run([p.CLANG,*flags,'-fobjc-arc','-fblocks',fair/'TransformerMps.mm','-framework','Foundation','-framework','Metal','-framework','MetalPerformanceShaders','-framework','MetalPerformanceShadersGraph','-o',build/'mps'])
    manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(f):p.sha(f) for f in (build/'gpu',build/'mps',build/'metal.a',build/'task.a',build/'Driver.cpp')},commands=p.COMMANDS),indent=2)+'\n')
saved = json.loads(manifest.read_text()); assert saved['signature'] == signature
for f, digest in saved['artifacts'].items(): assert p.sha(p.ROOT/f) == digest

CASES = [(1,64),(16,64),(64,64),(1,256),(16,256),(64,256),(1,512),(16,512),(64,512)]
def execute(backend, m, d, mode, count=1, rounds=1, dump=0, pattern=0):
    if backend == 'old': command = [baseline,bundle,m,d,count,rounds,mode,dump,pattern]
    elif backend == 'cpu': command = [accept/'cpu',m,d,count,rounds,0,dump,pattern]
    elif backend in ('shared','private'): command = [build/'gpu',bundle,m,d,count,rounds,mode,dump,pattern,int(backend=='shared')]
    else: command = [build/'mps',m,d,count,rounds,mode,dump,pattern,int(backend=='mps-shared')]
    out = p.run(command)
    return json.loads(out) if dump else p.observations(out,m*d*4,count,rounds)

verification_file = build/'verification.json'
if not verification_file.exists():
    verification = []
    for m,d,pattern in [(2,3,1),(3,7,1),(5,17,1),(17,33,1)]+[(m,d,0) for m,d in CASES]:
        baseline_row = execute('old',m,d,0,dump=1,pattern=pattern)
        for backend in ('cpu','shared','private','mps-shared','mps-private'):
            for mode in ((0,) if backend=='cpu' else (0,1,2)):
                row = execute(backend,m,d,mode,dump=1,pattern=pattern)
                metrics,_ = ref.assess(m,d,pattern,row)
                if backend in ('shared','private'): assert row == baseline_row
                metrics.update(backend=backend,m=m,d=d,mode=mode,pattern=pattern,bit_identical_to_baseline=(row==baseline_row))
                verification.append(metrics)
        print(f'{m}x{d}: independent per-cell oracle; both storage policies; unchanged Kalandra bits',flush=True)
    verification_file.write_text(json.dumps(verification,indent=2)+'\n')
verification = json.loads(verification_file.read_text())
if args.study == 'matched':
    cells = [(v,m,d,mode) for m,d in CASES for v in ('cpu','shared','mps-shared') for mode in ((0,) if v=='cpu' else (0,1,2))]
    result = p.measure(cells,lambda c,n,r:execute(*c,n,r),lambda c:c[1]*c[2]*4,processes=args.blocks,rounds=3,target_ms=30,adaptive=True,max_iterations=5000)
else:
    cases = [(1,64),(1,256),(1,512),(64,512)] if args.study=='repeat' else CASES
    kinds = ('kalandra','mps') if args.study=='storage' else ('kalandra',)
    cells = [(v,k,m,d,mode) for k in kinds for m,d in cases for mode in range(3) for v in ('baseline','current')]
    def invoke(cell,count,rounds):
        v,k,m,d,mode = cell
        if args.study=='storage': backend = ('private' if v=='baseline' else 'shared') if k=='kalandra' else ('mps-private' if v=='baseline' else 'mps-shared')
        else: backend = 'old' if v=='baseline' else 'shared'
        return execute(backend,m,d,mode,count,rounds)
    result = p.measure_paired(cells,invoke,lambda c:c[2]*c[3]*4,blocks=args.blocks,target_ms=30,adaptive=True,max_iterations=5000)
result.update(contract=__doc__,study=args.study,verification=verification,baseline_evidence_sha256=p.sha(args.evidence),
              model=data['model'],timing='Warm whole-block execution including selected uploads and cleanup. Shared policies allocate and CPU-copy a fresh owned snapshot. No output readback in timing. Private controls use the existing blit path. No arithmetic/compiler/task changes.')
assert signature == {p.key(f):p.sha(f) for f in sources+[metal,task]}
p.evidence(args.output,result,sources+[args.evidence,manifest,verification_file],[baseline,accept/'cpu',build/'gpu',build/'mps',build/'metal.a',build/'task.a',*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
