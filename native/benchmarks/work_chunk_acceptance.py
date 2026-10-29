"""Frozen work-bounded GPU progression: baseline, same-binary control and matched references."""
import argparse, json, os, shutil, sys
from pathlib import Path
os.environ['OPENBLAS_NUM_THREADS'] = '1'
sys.path.insert(0, str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref

parser = argparse.ArgumentParser(description=__doc__)
for name in ('acceptance-dir', 'program-dir', 'prepared-dir', 'build-dir', 'output'):
    parser.add_argument('--'+name, type=Path, required=True)
parser.add_argument('--study', choices=('paired', 'control', 'matched', 'repeat'), required=True)
parser.add_argument('--blocks', type=int, default=5)
args = parser.parse_args()
for flag in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES', 'KALANDRA_GPU_PROFILE'):
    assert os.getenv(flag) in (None, '', '0'), flag
assert not args.output.exists()
accept=args.acceptance_dir.resolve(); program=args.program_dir.resolve()
prepared=args.prepared_dir.resolve(); bundle=program/'bundle'
old=json.loads((accept/'manifest.json').read_text())
for f,digest in old['artifacts'].items(): assert p.sha(p.ROOT/f)==digest
build=args.build_dir.resolve(); build.mkdir(parents=True,exist_ok=True)
fair=Path(__file__).resolve().parent/'fair'
compiler=p.ROOT/'build/native/native/kalandra'
metal=compiler.parent/'metal/libkalandra-metal-optimized.a'
task=compiler.parent/'libkalandra-task-core-optimized.a'
assert p.sha(task)==p.sha(accept/'task.a')
shader_texts=[f for f in bundle.iterdir() if f.suffix in ('.metal','.mlir')]
assert shader_texts and all(p.sha(f)==p.sha(prepared/'bundle'/f.name) for f in shader_texts)
qualified=json.loads((prepared/'qualification.json').read_text())
assert {r['mode'] for r in qualified['records']}=={'optimized','debug','sanitized','thread-sanitized'}
sources=[Path(__file__), fair/'protocol.py', fair/'transformer_reference.py', fair/'ffn_reference.py',
         fair/'TransformerBlock.h', prepared/'qualification.json', prepared/'TransformerProgram.h',
         *[p.ROOT/f for f in ('native/metal/Metal.h','native/metal/Metal.mm',
                             'native/metal-source/NumericProgram.h','native/metal-source/NumericProgram.cpp',
                             'native/metal-source/Compiler.cpp')]]
signature={p.key(f):p.sha(f) for f in sources+[compiler,metal,task]}
manifest=build/'manifest.json'
if not manifest.exists():
    shutil.copy2(metal,build/'metal.a'); shutil.copy2(task,build/'task.a')
    shutil.copy2(compiler,build/'kalandra')
    shutil.copy2(prepared/'TransformerProgram.h',build/'TransformerProgram.h')
    driver=(accept/'Driver.cpp').read_text()
    assert driver.count('ffn::check(argc==10);')==driver.count('auto made=m::Runtime::create(options);')==1
    driver=driver.replace('ffn::check(argc==10);','ffn::check(argc==11);').replace('auto made=m::Runtime::create(options);','options.workBoundedProgression=std::stoul(argv[10])!=0; auto made=m::Runtime::create(options);')
    (build/'Driver.cpp').write_text(driver)
    p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0',
           '-I'+str(build),'-I'+str(fair),build/'Driver.cpp',build/'metal.a',build/'task.a',
           '-framework','Foundation','-framework','Metal','-o',build/'gpu'])
    p.run([compiler,p.ROOT/'native/benchmarks/TransformerBlock.k','--test-entry','block',
           '--arithmetic','hardware','-O2','--emit-llvm',build/'cpu.ll','-o',build/'cpu-standalone'])
    assert p.sha(build/'cpu.ll')==p.sha(program/'cpu.ll')
    artifacts=[build/f for f in ('gpu','metal.a','task.a','kalandra','TransformerProgram.h','Driver.cpp','cpu.ll')]
    artifacts += [accept/'gpu',accept/'mps',program/'cpu',*bundle.glob('*.metallib'),*shader_texts]
    manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(f):p.sha(f) for f in artifacts},commands=p.COMMANDS),indent=2)+'\n')
saved=json.loads(manifest.read_text()); assert saved['signature']==signature
for f,digest in saved['artifacts'].items(): assert p.sha(p.ROOT/f)==digest

shapes=[(1,64),(16,64),(64,64),(1,256),(16,256),(64,256),(1,512),(16,512),(64,512)]
def execute(backend,m,d,mode,count=1,rounds=1,dump=0,pattern=0):
    if backend=='old': command=[accept/'gpu',bundle,m,d,count,rounds,mode,dump,pattern,1]
    elif backend=='cpu': command=[program/'cpu',m,d,count,rounds,0,dump,pattern]
    elif backend=='mps': command=[accept/'mps',m,d,count,rounds,mode,dump,pattern,1]
    else: command=[build/'gpu',bundle,m,d,count,rounds,mode,dump,pattern,1,int(backend=='new')]
    output=p.run(command)
    return json.loads(output) if dump else p.observations(output,m*d*4,count,rounds)
verification_file=build/'verification.json'
if not verification_file.exists():
    verification=[]
    for m,d,pattern in [(2,3,1),(3,7,1),(5,17,1),(17,33,1)]+[(m,d,0) for m,d in shapes]:
        baseline=execute('old',m,d,0,dump=1,pattern=pattern)
        for backend in ('old','new','control','cpu','mps'):
            for mode in ((0,) if backend=='cpu' else range(3)):
                row=execute(backend,m,d,mode,dump=1,pattern=pattern)
                if backend in ('old','new','control'): assert row==baseline
                metrics,_=ref.assess(m,d,pattern,row)
                metrics.update(backend=backend,m=m,d=d,mode=mode,pattern=pattern,bit_identical_to_baseline=(row==baseline))
                verification.append(metrics)
        print(f'{m}x{d}: all cells checked; baseline/control/current bits identical',flush=True)
    verification_file.write_text(json.dumps(verification,indent=2)+'\n')
verification=json.loads(verification_file.read_text())
if args.study=='matched':
    cells=[(backend,m,d,mode) for m,d in shapes for backend in ('cpu','new','mps') for mode in ((0,) if backend=='cpu' else range(3))]
    result=p.measure(cells,lambda c,n,r:execute(*c,n,r),lambda c:c[1]*c[2]*4,processes=args.blocks,rounds=3,target_ms=30,adaptive=True,max_iterations=5000)
else:
    cases=[(1,64),(1,512),(16,512),(64,512)] if args.study=='repeat' else shapes
    cells=[(version,m,d,mode) for m,d in cases for mode in range(3) for version in ('baseline','current')]
    def invoke(cell,count,rounds):
        version,m,d,mode=cell
        backend='new' if version=='current' else 'control' if args.study=='control' else 'old'
        return execute(backend,m,d,mode,count,rounds)
    result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=args.blocks,target_ms=30,adaptive=True,max_iterations=5000)
assert signature=={p.key(f):p.sha(f) for f in sources+[compiler,metal,task]}
result.update(contract=__doc__,study=args.study,verification=verification,qualification=qualified,
              unchanged_shader_texts={p.key(f):p.sha(f) for f in shader_texts},
              unchanged_cpu_llvm_sha256=p.sha(build/'cpu.ll'),
              timing='Warm whole-block wall time, selected fresh owned uploads and cleanup; identical shaders. Cancellation remains enabled. Work units are not a wall-clock cancellation deadline.')
p.evidence(args.output,result,sources+[manifest],[p.ROOT/f for f in saved['artifacts']],compiler)
