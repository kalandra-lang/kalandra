"""Qualify observable prefixes of the actual transformer source on CPU and Metal."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
os.environ['OPENBLAS_NUM_THREADS']='1'
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as r

def references(m,d):
    arrays=r.inputs(m,d,1);x,wq,wk,wv,wo,up,down=arrays
    eps=r.np.float32(.00001);scale=r.np.float32(1/r.math.sqrt(d))
    nominal={};nominal['norm']=r.rms32(x,eps)
    for name,w in zip(('q','k','v'),(wq,wk,wv)):nominal[name]=r.strict_matmul(nominal['norm'],w)
    nominal['probabilities']=r.softmax32(r.strict_matmul(nominal['q'],nominal['k'].T)*scale)
    nominal['residual']=x+r.strict_matmul(r.strict_matmul(nominal['probabilities'],nominal['v']),wo)
    nominal['feed']=r.rms32(nominal['residual'],eps)
    nominal['hidden']=r.np.maximum(r.strict_matmul(nominal['feed'],up),r.np.float32(0))
    values=[(v.astype(r.np.float64),r.np.zeros(v.shape,r.np.float64)) for v in arrays]
    x,wq,wk,wv,wo,up,down=values
    high={'norm':r.rms(*x,float(eps))}
    for name,w in zip(('q','k','v'),(wq,wk,wv)):high[name]=r.mm(high['norm'],w)
    score,error=r.mm(high['q'],(high['k'][0].T,high['k'][1].T))
    error=error*float(scale)+r.gamma(2)*r.np.abs(score*float(scale));score*=float(scale)
    high['probabilities']=r.softmax(score,error)
    high['residual']=r.add(x,r.mm(r.mm(high['probabilities'],high['v']),wo))
    high['feed']=r.rms(*high['residual'],float(eps))
    h,e=r.mm(high['feed'],up);high['hidden']=(r.np.maximum(h,0),e)
    return nominal,high

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--full-build',dest='full_bundle',type=Path,required=True,help='Frozen full-block build directory supplying its authored main/capture layout')
    args=parser.parse_args();assert not args.output.exists()
    root=p.ROOT;fair=Path(__file__).parent/'fair';source=Path(__file__).parent/'TransformerBlock.k';build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
    compiler=root/'build/native/native/kalandra';native=compiler.parent/'libkalandra-native-runtime.a';metal=compiler.parent/'metal/libkalandra-metal-optimized.a';task=compiler.parent/'libkalandra-task-core-optimized.a'
    full=(args.full_bundle/'block-gpu.k').read_text();gpu_main='fn main()'+full.split('fn main()',1)[1]
    signature=source.read_text().split('fn main()')[0];header=next(line for line in signature.splitlines() if line.startswith('fn block('))
    assignments=[line for line in signature.splitlines() if line.startswith('  ') and '=' in line]
    assert len(assignments)==7
    assignments+=['  hidden=relu(matmul(feed,up))']
    sources=[Path(__file__),source,fair/'TransformerCpu.cpp',fair/'TransformerGpu.cpp',fair/'TransformerBlock.h',fair/'ResidualFfn.h',fair/'transformer_reference.py',fair/'ffn_reference.py']
    initial={str(f):p.sha(f) for f in sources+[compiler,native,metal,task]};artifacts=[];records=[]
    for index,line in enumerate(assignments):
        stage=line.strip().split('=',1)[0];directory=build/stage;directory.mkdir(exist_ok=True)
        body=header+'\n'+'\n'.join(assignments[:index+1])+'\n  '+stage+'\n'
        cpu_source=directory/'cpu.k';cpu_source.write_text(body+'fn main()->int64:\n  0\n')
        ir=directory/'cpu.ll';p.run([compiler,cpu_source,'--test-entry','block','--arithmetic','hardware','-O2','--emit-llvm',ir,'-o',directory/'standalone'])
        gpu_source=directory/'gpu.k';gpu_source.write_text(body+gpu_main);bundle=directory/'bundle'
        p.run([compiler,gpu_source,'--target','metal','--arithmetic','hardware','-O2','-o',bundle])
        host=(bundle/'host.cpp').read_text();loads=re.findall(r'auto (kernel\d+)=(r\.load[^;]+);if\(!kernel\d+\)\{[^\n]+',host)
        start=host.index('auto operands0=');end=host.index('=r.numericProgram',start);code=host[start:host.rfind('\nf.',start,end)]
        slots=list(dict.fromkeys(re.findall(r'f\.v\d+','\n'.join(s for s in code.splitlines() if s.startswith('auto scalars')))));assert 1<=len(slots)<=2
        for old,new in zip(slots,('epsilon','scale')):code=code.replace(old,new)
        result=re.search(r'\.numericProgram\(task,f\.v\d+,std::array<m::Buffer,7>\{[^\n]*?\},code,(\d+)u,kalandra::NumericType::Float32,false\)',host);assert result
        text=f'struct Program {{std::array<m::Kernel,{len(loads)}> kernels;\nProgram(m::Runtime&r,std::string directory){{auto target=r.device();ffn::check(bool(target));\n'
        for name,call in loads:text+=f'auto {name}='+call.replace('f.directory','directory')+f';ffn::check(bool({name}));\n'
        text+='kernels={'+','.join('*'+name for name,_ in loads)+'};}\nTaskHandle launch(m::Runtime&r,TaskCore&core,GroupHandle group,const std::array<m::Buffer,7>&inputs,float epsilon,float scale){\n'
        for i in range(len(loads)):text+=f'auto*kernel{i}=&kernels[{i}];\n'
        text+=code+f'\nreturn r.numericProgram(core,group,inputs,code,{result[1]}u,kalandra::NumericType::Float32,false);\n'+'}\n};\n'
        generated=directory/'TransformerProgram.h';generated.write_text(text)
        cols='m' if stage=='probabilities' else '4*d' if stage=='hidden' else 'd'
        cpu=(fair/'TransformerCpu.cpp').read_text().replace('../../runtime/Tensor.h',str(root/'native/runtime/Tensor.h'))
        cpu=cpu.replace('out.sizes==data.shapes[0]',f'(out.sizes[0]==m && out.sizes[1]==({cols}))').replace('cells(m*d)',f'cells(m*({cols}))').replace('c<d;','c<('+cols+');').replace('cells[r*d+c]',f'cells[r*({cols})+c]')
        gpu=(fair/'TransformerGpu.cpp').read_text().replace('../../metal/Metal.h',str(root/'native/metal/Metal.h'))
        gpu_cols='rows' if stage=='probabilities' else '4*d' if stage=='hidden' else 'd'
        gpu=gpu.replace('out.shape()[1]==d && out.bytes()==rows*d*4ull',f'out.shape()[1]==({gpu_cols}) && out.bytes()==rows*({gpu_cols})*4ull').replace('cells(rows*d)',f'cells(rows*({gpu_cols}))')
        cpp=directory/'Cpu.cpp';cpp.write_text(cpu);gpu_cpp=directory/'Gpu.cpp';gpu_cpp.write_text(gpu)
        flags=['-std=c++23','-O2','-pthread','-ffp-contract=off','-Wno-override-module','-I'+str(fair.resolve()),'-I'+str(directory)]
        p.run([p.CLANG,*flags,cpp,ir,native,'-o',directory/'cpu'])
        p.run([p.CLANG,*flags,gpu_cpp,metal,task,'-framework','Foundation','-framework','Metal','-o',directory/'gpu'])
        for m,d in ((3,7),(16,64)):
            nominal,high=references(m,d);want,bound=high[stage];bound=2*bound+r.np.finfo(r.np.float32).tiny
            for backend in ('cpu','gpu'):
                command=[directory/backend,*([bundle] if backend=='gpu' else []),m,d,1,1,0,1,1]
                row=json.loads(p.run(command));actual=r.np.asarray(row['cells'],r.np.float32).reshape(want.shape)
                error=r.np.abs(actual.astype(r.np.float64)-want);assert r.np.all(error<=bound),(stage,backend,float(r.np.max(error/bound)))
                exact=int(r.np.sum(actual.view(r.np.uint32)==nominal[stage].view(r.np.uint32)))
                if stage in ('norm','q','k','v'):assert exact==actual.size,(stage,backend,exact,actual.size)
                records.append(dict(stage=stage,backend=backend,m=m,d=d,cells=int(actual.size),nominal_matching_cells=exact,max_abs_error=float(r.np.max(error)),max_error_bound_ratio=float(r.np.max(error/bound))))
        artifacts +=[cpu_source,gpu_source,cpp,gpu_cpp,generated,ir,directory/'cpu',directory/'gpu',bundle/'host.cpp',*bundle.glob('*.metallib')]
        print(stage+': complete CPU/GPU intermediate matrices verified for odd and tiled shapes',flush=True)
    assert all(p.sha(f)==h for f,h in initial.items())
    p.evidence(args.output,dict(contract=__doc__,records=records,source_snapshots={p.key(f):f.read_text() for f in sources}),sources,artifacts+[compiler,native,metal,task],compiler)
if __name__=='__main__':main()
