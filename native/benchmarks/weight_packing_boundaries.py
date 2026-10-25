"""Paired first-use packed-panel admission-boundary screen; includes fresh GPU construction."""
import argparse,json,math,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--screen',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--layout',choices=['both','block','blockpad','panelpad'],default='both');parser.add_argument('--suite',choices=['boundaries','envelope'],default='boundaries');a=parser.parse_args();assert not a.output.exists()
    for key in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','KALANDRA_GPU_PROFILE','DYLD_INSERT_LIBRARIES'):assert os.getenv(key) in (None,'','0'),key
    screen=json.loads(a.screen.read_text());assert screen['pack_method']=='tiled'
    library=next(Path(k) if Path(k).is_absolute() else p.ROOT/k for k in screen['artifact_sha256'] if k.endswith('packing.metallib'));assert p.sha(library)==screen['artifact_sha256'][p.key(library)]
    build=p.ROOT/'benchmarks/.build/weight-packing-2026-10-25'/a.output.stem;build.mkdir(parents=True,exist_ok=True);binary=build/'boundaries';driver=Path(__file__).with_name('WeightPackingStudy.mm')
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-ffp-contract=off','-fobjc-arc','-fblocks','-DKALANDRA_PACK_TILED',driver,'-framework','Foundation','-framework','Metal','-o',binary])
    shapes=[(1024,512,1024),(1025,576,1027),(1024,4096,1024),(2048,512,1024),(1024,1024,2048),(2048,1024,1024),(1024,4096,2048),(2048,4096,1024)]
    if a.suite=="envelope": shapes=[(1024,2048,1024),(1536,768,1024),(1536,1536,1536),(1536,3072,1024),(2048,4096,2048),(1025,576,1025),(2048,512,2048),(1088,1024,1024)]
    def invoke(version,shape,count):
        rows=list(map(json.loads,p.run([binary,library,'baseline' if version=='baseline' else a.layout,*shape,1,3,count]).splitlines()))
        assert [r['round'] for r in rows]==list(range(-2,3)) and all(r['cells']==shape[0]*shape[2] and r['gpu_ns']>0 for r in rows)
        return rows
    counts={};calibration=[]
    for shape in shapes:
        times=[]
        for version in ['baseline','current']:
            rows=invoke(version,shape,1);times.append(statistics.median(r['gpu_ns'] for r in rows if r['round']>=0));calibration.append(dict(shape=list(shape),version=version,samples=rows))
        counts[shape]=max(1,min(512,math.ceil(20e6/max(times))))
    records=[];pairs=[];rng=random.Random(20261025)
    for block in range(5):
        order=shapes.copy();rng.shuffle(order)
        for shape in order:
            count=counts[shape];medians={'baseline':[],'current':[]}
            versions=('baseline','current','current','baseline') if block%2==0 else ('current','baseline','baseline','current')
            for slot,version in enumerate(versions):
                rows=invoke(version,shape,count);medians[version].append(statistics.median(r['gpu_ns']/count for r in rows if r['round']>=0));records.append(dict(block=block,slot=slot,version=version,shape=list(shape),iterations=count,samples=rows))
            before=statistics.mean(medians['baseline']);after=statistics.mean(medians['current']);pairs.append(dict(block=block,shape=list(shape),baseline_gpu_ns=before,current_gpu_ns=after,time_reduction=1-after/before));print(block,shape,before/1e6,after/1e6,flush=True)
    p.evidence(a.output,dict(records=records,pairs=pairs,calibration=calibration,layout=a.layout,suite=a.suite,runner_source=Path(__file__).read_text(),driver_source=driver.read_text(),contract='Five mirrored randomized ABBA/BAAB blocks across eight shapes in the recorded suite. Equal calibrated iterations per pair, 20ms GPU target, two warmups/three rounds. R=1: selected input layout freshly constructed on every iteration. Preallocated private buffers; no buffer-allocation/upload cost. Complete final output/packing/guard/immutability checks outside timing. This is a numerical-kernel shape-policy screen, not managed pipeline throughput.'),[Path(__file__),driver,a.screen],[binary,library],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
