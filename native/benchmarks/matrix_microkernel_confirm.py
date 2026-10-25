"""Mirrored paired confirmation of a screened strict matmul entry and its shape edges."""
import argparse,json,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--screen',type=Path,required=True);parser.add_argument('--variant',required=True);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args();assert not a.output.exists()
    for name in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):assert os.getenv(name) in (None,'','0'),name
    screen=json.loads(a.screen.read_text());variant=next(v for v in screen['variants'] if v[0]==a.variant)
    paths=[Path(k) if Path(k).is_absolute() else p.ROOT/k for k in screen['artifact_sha256']]
    library=next(f for f in paths if f.name=='candidates.metallib');binary=next(f for f in paths if f.name=='study')
    assert p.sha(library)==screen['artifact_sha256'][p.key(library)] and p.sha(binary)==screen['artifact_sha256'][p.key(binary)]
    shapes=[(512,512,512),(1024,1024,1024),(2048,2048,2048),(256,512,256),(257,512,257),(128,1024,512),(512,2048,128),(16,2048,4097),(4097,2048,16)]
    records=[];pairs=[];rng=random.Random(20261025)
    for block in range(7):
        order=shapes.copy();rng.shuffle(order)
        for m,k,n in order:
            medians={'baseline':[],'current':[]}
            versions=('baseline','current','current','baseline') if not block%2 else ('current','baseline','baseline','current')
            for slot,version in enumerate(versions):
                entry,rows,cols,threads=('kalandra_matmul_hardware_register16',16,16,256) if version=='baseline' else variant[:4]
                samples=list(map(json.loads,p.run([binary,library,entry,rows,cols,threads,m,k,n,5]).splitlines()))
                assert [x['round'] for x in samples]==list(range(-2,5))
                assert all(x['cells']==m*n and x['gpu_ns']>0 for x in samples)
                medians[version].append(statistics.median(x['gpu_ns'] for x in samples if x['round']>=0))
                records.append(dict(block=block,slot=slot,version=version,shape=[m,k,n],samples=samples))
            before=statistics.mean(medians['baseline']);after=statistics.mean(medians['current'])
            pairs.append(dict(block=block,shape=[m,k,n],baseline_gpu_ns=before,current_gpu_ns=after,time_reduction=1-after/before))
            print(block,(m,k,n),before/1e6,after/1e6,flush=True)
    p.evidence(a.output,dict(records=records,pairs=pairs,variant=variant,contract='Seven randomized mirrored ABBA/BAAB blocks. Same library/driver, fresh processes, two warmups and five measured GPU command timestamps. Every-cell independent dyadic checks and poison/guard checks every round outside timing. Preallocated shared inputs; excludes language scheduling/allocation and epilogue/reduction. Includes non-square, partial-tile and narrow admission boundaries. Not complete-pipeline throughput.'),[Path(__file__),a.screen,Path(__file__).with_name('MatrixKernelStudy.mm')],[binary,library],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
