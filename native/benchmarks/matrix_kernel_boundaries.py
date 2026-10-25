"""Paired raw-kernel shape-boundary check, separate from language throughput."""
import argparse,json,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--library',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args()
    assert not a.output.exists()
    for name in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):assert os.getenv(name) in (None,'','0'),name
    source=Path(__file__).with_name('MatrixKernelStudy.mm');build=p.ROOT/'benchmarks/.build'/a.output.stem;build.mkdir(parents=True,exist_ok=True);binary=build/'boundaries'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-fobjc-arc','-fblocks',source,'-framework','Foundation','-framework','Metal','-o',binary])
    shapes=[(256,512,256),(128,512,512),(512,512,128),(16,512,4097),(4097,512,16),
            (257,576,257),(257,4032,257),(257,4096,257),(1024,4096,128),(16,4096,4096)]
    records=[];pairs=[];rng=random.Random(20261025)
    for block in range(3):
        order=shapes.copy();rng.shuffle(order)
        for m,k,n in order:
            medians={'baseline':[],'current':[]}
            versions=('baseline','current','current','baseline') if not block%2 else ('current','baseline','baseline','current')
            for slot,version in enumerate(versions):
                entry,cols,threads=('kalandra_matmul_hardware_block64',8,128) if version=='baseline' else ('kalandra_matmul_hardware_register16',16,256)
                samples=list(map(json.loads,p.run([binary,a.library,entry,16,cols,threads,m,k,n,5]).splitlines()))
                assert [x['round'] for x in samples]==list(range(-2,5))
                assert all(x['cells']==m*n and x['gpu_ns']>0 for x in samples)
                medians[version].append(statistics.median(x['gpu_ns'] for x in samples if x['round']>=0))
                records.append(dict(block=block,slot=slot,version=version,shape=[m,k,n],samples=samples))
            before=statistics.mean(medians['baseline']);after=statistics.mean(medians['current'])
            pairs.append(dict(block=block,shape=[m,k,n],baseline_gpu_ns=before,current_gpu_ns=after,time_reduction=1-after/before))
            print(block,(m,k,n),before/1e6,after/1e6,flush=True)
    p.evidence(a.output,dict(records=records,pairs=pairs,contract='Three randomized mirrored ABBA/BAAB blocks; fresh processes with two warmups and five measured GPU command timestamps. Independent dyadic every-cell checks and poison/guard checks every round, outside timing. Preallocated buffers; this excludes the language runtime and is a shape-policy screen, not application throughput.'),[Path(__file__),source,p.ROOT/'native/metal-source/HardwareShaders.h',p.ROOT/'native/metal/MatmulLayout.h'],[binary,a.library],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
