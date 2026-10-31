"""Independent row-math/bitwise transpose qualification, including failure cleanup.

Decimal references round every arithmetic step and preserve the adjacent-pair
tree. Softmax propagates a four-ULP exp interval through the rounded denominator
and division; it does not use a fitted absolute tolerance.
"""
from OrdinaryTestEnvironment import qualified_variants
import argparse
import concurrent.futures
import decimal as dec
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler',type=Path,required=True)
parser.add_argument('--clang',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--gpu',action='store_true')
parser.add_argument('--quick',action='store_true')
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
args.compiler=args.compiler.resolve();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
dec.getcontext().prec=110
dec.getcontext().traps[dec.InvalidOperation]=False
dec.getcontext().traps[dec.DivisionByZero]=False
dec.getcontext().traps[dec.Overflow]=False
D=dec.Decimal
types=[('Float32','f','I',32),('Float64','d','Q',64)]+[(('U' if unsigned else '')+'Int'+str(bits),'','B' if bits==8 else 'H' if bits==16 else 'I' if bits==32 else 'Q',bits) for bits in (8,16,32,64) for unsigned in (False,True)]
variants=[('optimized','-O2',['-O2'])]
if not args.quick:
    variants += [('debug','-O0',['-O0']),('sanitized','--sanitize',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]
    if sys.platform!='win32':variants += [('thread-sanitized','--thread-sanitize',['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'])]
variants=qualified_variants(variants,args.compiler)
def run(command,timeout=240):
    p=subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=timeout)
    assert not p.returncode,(command,p.returncode,p.stdout[-3000:],p.stderr[-3000:])
    return p.stdout
def to_bits(x,fmt,wire):
    try:return struct.unpack('<'+wire,struct.pack('<'+fmt,float(x)))[0]
    except OverflowError:return struct.unpack('<'+wire,struct.pack('<'+fmt,math.copysign(math.inf,float(x))))[0]
def from_bits(x,fmt,wire):return struct.unpack('<'+fmt,struct.pack('<'+wire,x))[0]
def reference(values,m,n,op,epsilon,fmt,wire,gpu):
    def q(x):return D.from_float(from_bits(to_bits(x,fmt,wire),fmt,wire))
    def tree(xs):
        while len(xs)>1:xs=[q(xs[i]+xs[i+1]) if i+1<len(xs) else xs[i] for i in range(0,len(xs),2)]
        return xs[0]
    if op==0:return [values[r*n+c] for c in range(n) for r in range(m)]
    result=[]
    for row in range(m):
        xs=[D.from_float(from_bits(v,fmt,wire)) for v in values[row*n:(row+1)*n]]
        if not xs:continue
        if op==2:
            denominator=q(q(q(tree([q(x*x) for x in xs])/D(n))+epsilon).sqrt())
            result.extend((q(x/denominator),q(x/denominator)) for x in xs)
            continue
        if any(x.is_nan() for x in xs) or all(x==D('-Infinity') for x in xs):
            result.extend((D('NaN'),D('NaN')) for x in xs);continue
        high=max(xs)
        exps=[D(int(x==high)) if high==D('Infinity') else q(q(x-high).exp()) for x in xs]
        low=[];upper=[]
        for x in exps:
            b=to_bits(x,fmt,wire)
            # Infinity indicator is exact; exp is bounded to four ULP.
            delta=0 if high==D('Infinity') else 4
            lo=max(0,b-delta);hi=b+delta
            if gpu and b<(1<<23):lo=0;hi=max(hi,1<<23)
            low.append(D.from_float(from_bits(lo,fmt,wire)));upper.append(D.from_float(from_bits(hi,fmt,wire)))
        minimum,maximum=tree(low),tree(upper)
        result.extend((q(lo/maximum),q(hi/minimum)) for lo,hi in zip(low,upper))
    return result
checks=0;records=[]
for name,fmt,wire,width in types:
    if args.gpu and name=='Float64':ops=(0,)
    else:ops=(0,1,2) if fmt else (0,)
    language=name.lower()
    for op in ops:
        operation=('transpose','softmaxRows','rmsNormRows')[op]
        signature=f'fn work(x:Tensor[{language},?,?]'+(f',epsilon:{language}' if op==2 else '')+f')->Tensor[{language},?,?]:\n  {operation}(x'+(',epsilon' if op==2 else '')+')\n'
        cpu_source=args.output/f'{name}-{operation}.k';cpu_source.write_text(signature+'fn main()->int64:\n  0\n')
        bundle=args.output/f'{name}-{operation}-bundle'
        if args.gpu:
            source=args.output/f'{name}-{operation}-gpu.k'
            source.write_text(signature+f'''fn main()->Tensor[{language},?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    cpu=devices.cpu()
    x:Tensor[{language},2,3]=tensor([[1,2,3],[4,5,6]])
    a=try await transfer(x,gpu)
    b=try await spawn work(a{',0.00001' if op==2 else ''}) on gpu
    try await transfer(b,cpu)
''')
            run([args.compiler,source,'--target','metal','--arithmetic','hardware','-O2','-o',bundle])
        for variant,flag,cflags in variants:
            binary=args.output/f'{name}-{operation}-{variant}'
            link=[]
            if args.gpu:
                task='libkalandra-task-core-optimized.a' if variant=='optimized' else 'libkalandra-native-runtime'+('' if variant=='debug' else '-'+variant)+'.a'
                link=[args.compiler.parent/'metal'/f'libkalandra-metal-{variant}.a',args.compiler.parent/task,'-framework','Foundation','-framework','Metal','-DGPU']
            else:
                ir=binary.with_suffix('.ll')
                run([args.compiler,cpu_source,'--test-entry','work','--arithmetic','hardware','--emit-llvm',ir,flag,'-o',str(binary)+'-standalone'])
                runtime=args.compiler.parent/(('' if sys.platform=='win32' else 'lib')+'kalandra-native-runtime'+('-'+variant if 'sanitized' in variant else '')+('.lib' if sys.platform=='win32' else '.a'))
                link=[ir,runtime]
            run([args.clang,'-std=c++23','-pthread','-ffp-contract=off','-Wno-override-module',*cflags,*(['-fms-runtime-lib=static'] if sys.platform=='win32' else []),'-DELEMENT='+name,'-DOP='+str(op),root/'native/tests/TensorTransformDriver.cpp',*link,'-o',binary])
            if not args.gpu:
                # Keep numerical checks on the original LLVM/runtime. A second
                # executable pauses an observed poll only for concurrent
                # cancellation, so a late observer timeslice cannot miss the run.
                gated_ir=binary.with_suffix('.cancel-gate.ll')
                original_ir=ir.read_text()
                assert '@kalandra_poll_slow_v1(' in original_ir
                gated_ir.write_text(original_ir.replace('@kalandra_poll_slow_v1(', '@kalandra_transform_poll_gate_v1('))
                gated_binary=Path(str(binary)+'-cancel-gate')
                run([args.clang,'-std=c++23','-pthread','-ffp-contract=off','-Wno-override-module',*cflags,*(['-fms-runtime-lib=static'] if sys.platform=='win32' else []),'-DELEMENT='+name,'-DOP='+str(op),root/'native/tests/TensorTransformDriver.cpp',root/'native/tests/TensorTransformPollGate.cpp',gated_ir,runtime,'-o',gated_binary])
            epsilon=to_bits(0.00001,fmt,wire) if fmt else 0
            stderr=tempfile.TemporaryFile(mode='w+t')
            server=subprocess.Popen([str(binary),'--server'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr,text=True)
            reader=concurrent.futures.ThreadPoolExecutor(max_workers=1)
            def invoke(values,m,n,layout=0,eps=epsilon,fail=0,cancel=0):
                path=args.output/'input.bin';path.write_bytes(struct.pack('<'+wire*len(values),*values))
                command=[m,n,layout,eps,fail,cancel,path,'unused',*([bundle] if args.gpu else [])]
                if not args.gpu and cancel==2:
                    return json.loads(run([gated_binary,*command]))
                server.stdin.write(' '.join(json.dumps(str(x),ensure_ascii=False) for x in command)+'\n');server.stdin.flush()
                try:line=reader.submit(server.stdout.readline).result(timeout=240)
                except BaseException:
                    server.kill();server.wait();raise
                if not line:
                    server.wait();stderr.seek(0);raise AssertionError((binary,command,server.returncode,stderr.read()[-4000:]))
                return json.loads(line)
            for m,n in [(0,3),(3,0),(1,1),(2,3),(3,7),(2,31),(2,32),(2,33),(2,255),(2,256),(2,257),(1,1025)]:
                if fmt:values=[to_bits(D(((i*37+11)%101)-50)/13,fmt,wire) for i in range(m*n)]
                else:values=[((i*0x9e3779b97f4a7c15)^0xa55a0ff00f5aa5)&((1<<width)-1) for i in range(m*n)]
                expected=reference(values,m,n,op,D.from_float(from_bits(epsilon,fmt,wire)) if fmt else None,fmt,wire,args.gpu)
                for layout in ((0,1) if args.gpu and op==2 else (0,) if args.gpu else (0,1,2,3)):
                    got=invoke(values,m,n,layout);assert not got['status'] and not got['cancelled'],got
                    assert len(got['bits'])==len(expected)
                    for actual,wanted in zip(got['bits'],expected):
                        if op==0:assert actual==wanted
                        else:
                            number=D.from_float(from_bits(actual,fmt,wire));lo,hi=wanted
                            assert (number.is_nan() and lo.is_nan()) or (not number.is_nan() and lo<=number<=hi),(name,operation,m,n,actual,wanted)
                        checks+=1
            # IEEE payloads/signs are preserved by transpose; row exceptional
            # cases require NaN propagation and defined infinity handling.
            if fmt:
                exponent=(0x7f800000 if width==32 else 0x7ff0000000000000)
                fraction=23 if width==32 else 52
                specials=[0,1,1<<(width-1),exponent,exponent|(1<<(width-1)),exponent|1,exponent|12345,exponent|(1<<(fraction-1)),(1<<(width-1))|1,exponent-1,(exponent-1)|(1<<(width-1)),1<<fraction]
                for values in [specials,[exponent]*7,[exponent|(1<<(width-1))]*7,[0,0,0,0,0,0,0],[exponent,0,exponent,0,0,0,0],[exponent-1,(exponent-1)|(1<<(width-1)),0,1,1<<fraction]]:
                    expected=reference(values,1,len(values),op,D.from_float(from_bits(epsilon,fmt,wire)),fmt,wire,args.gpu)
                    got=invoke(values,1,len(values));assert not got['status']
                    for actual,wanted in zip(got['bits'],expected):
                        if op==0:assert actual==wanted
                        else:
                            number=D.from_float(from_bits(actual,fmt,wire));lo,hi=wanted
                            assert (number.is_nan() and lo.is_nan()) or (not number.is_nan() and lo<=number<=hi),(name,operation,actual,wanted)
                            if op==2 and not lo.is_nan():assert actual==to_bits(lo,fmt,wire),(name,operation,actual,lo)
                        checks+=1
                if op==2:
                    for valid in (1,exponent-1):
                        got=invoke([to_bits(1,fmt,wire)]*6,2,3,eps=valid)
                        assert not got['status'] and not got['cancelled'] and len(got['bits'])==6,got
                if op==2:
                    for invalid in [0,1<<(width-1),exponent,exponent|1,to_bits(-1,fmt,wire)]:
                        for m,n in [(2,3),(0,3),(3,0)]:
                            for layout in ((0,1) if args.gpu else (0,)):
                                got=invoke([to_bits(1,fmt,wire)]*(m*n),m,n,layout,eps=invalid)
                                assert got['status']==4 and not got['bits'],got
            else:
                mask=(1<<width)-1;sign=1<<(width-1)
                extreme=[0,1,mask,sign,sign-1,sign+1,mask-1,2]
                got=invoke(extreme,2,4);assert not got['status'] and got['bits']==reference(extreme,2,4,0,None,fmt,wire,args.gpu),got
            small=[to_bits(1,fmt,wire) if fmt else 1]*21
            normal=invoke(small,3,7)
            # GPU stats include pre-injection upload attempts; later no-op
            # indices legitimately succeed. Every injected failure must clean.
            for fail in range(1,normal['allocations']+2):
                got=invoke(small,3,7,fail=fail);assert got['status'] in (0,2),got
            if args.gpu:
                for cancellation in (1,2,3,4):
                    got=invoke(small,3,7,cancel=cancellation)
                    assert not got['bits'] and (got['cancelled'] or got['status']),got
            else:
                got=invoke(small,3,7,cancel=1);assert got['cancelled'] and not got['bits'],got
                large=[to_bits(1,fmt,wire) if fmt else 1]*(17*65537)
                got=invoke(large,17,65537,cancel=2);assert got['cancelled'] and not got['bits'],got
            server.stdin.close();assert server.wait(timeout=30)==0
            reader.shutdown();stderr.close()
            records.append(dict(element=name,operation=operation,variant=variant))
            print(f'{name} {operation} {variant}: numerical/shape/ownership/failure/cancellation passed ({checks} cell checks)',flush=True)
if args.gpu:
    print(run([sys.executable,root/'native/tests/TensorTransformDeviceScalarTests.py','--compiler',args.compiler,'--output',args.output/'source-device-epsilon',*(['--quick'] if args.quick else [])]),flush=True)
(args.output/'qualification.json').write_text(json.dumps(dict(cell_checks=checks,records=records,source_device_epsilon_passed=args.gpu,contract=__doc__),indent=2)+'\n')
