"""Paired batched packed-weight cost/reuse confirmation with retained construction cost."""
import argparse,json,math,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--screen',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--layouts',nargs='+',default=['block','both']);a=parser.parse_args();assert not a.output.exists()
    for name in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'):assert os.getenv(name) in (None,'','0'),name
    screen=json.loads(a.screen.read_text());assert all(v in screen['layouts'] and v!='baseline' for v in a.layouts)
    library=next(Path(k) if Path(k).is_absolute() else p.ROOT/k for k in screen['artifact_sha256'] if k.endswith('packing.metallib'))
    assert p.sha(library)==screen['artifact_sha256'][p.key(library)]
    driver=Path(__file__).with_name('WeightPackingStudy.mm');build=p.ROOT/'benchmarks/.build/weight-packing-2026-10-25'/a.output.stem;build.mkdir(parents=True,exist_ok=True);binary=build/'paired'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-ffp-contract=off','-fobjc-arc','-fblocks',*(['-DKALANDRA_PACK_TILED'] if screen.get('pack_method')=='tiled' else []),driver,'-framework','Foundation','-framework','Metal','-o',binary])
    cells=[(layout,n,reuse) for layout in a.layouts for n in [512,1024,2048] for reuse in [1,8]]
    def invoke(layout,n,reuse,iterations):
        rows=list(map(json.loads,p.run([binary,library,layout,n,n,n,reuse,3,iterations]).splitlines()))
        assert [r['round'] for r in rows]==list(range(-2,3)) and all(r['cells']==n*n*reuse and r['gpu_ns']>0 for r in rows)
        return rows
    counts={};calibration=[]
    for cell in cells:
        costs=[]
        for version in ['baseline',cell[0]]:
            rows=invoke(version,cell[1],cell[2],1);costs.append(statistics.median(r['gpu_ns'] for r in rows if r['round']>=0));calibration.append(dict(cell=list(cell),version=version,samples=rows))
        counts[cell]=max(1,min(512,math.ceil(20e6/max(costs))))
    records=[];pairs=[];rng=random.Random(20261025)
    for block in range(7):
        order=cells.copy();rng.shuffle(order)
        for layout,n,reuse in order:
            count=counts[layout,n,reuse];medians={'baseline':[],'current':[]}
            versions=('baseline','current','current','baseline') if not block%2 else ('current','baseline','baseline','current')
            for slot,version in enumerate(versions):
                rows=invoke('baseline' if version=='baseline' else layout,n,reuse,count)
                medians[version].append(statistics.median(r['gpu_ns']/count/reuse for r in rows if r['round']>=0))
                records.append(dict(block=block,slot=slot,layout=layout,n=n,reuse=reuse,version=version,iterations=count,samples=rows))
            before=statistics.mean(medians['baseline']);after=statistics.mean(medians['current'])
            pairs.append(dict(block=block,layout=layout,n=n,reuse=reuse,baseline_gpu_ns_per_call=before,current_gpu_ns_per_call=after,time_reduction=1-after/before))
            print(block,layout,n,reuse,before/1e6,after/1e6,flush=True)
    p.evidence(a.output,dict(records=records,pairs=pairs,calibration=calibration,driver_source=driver.read_text(),screen=a.screen.name,pack_method=screen.get('pack_method','flat'),contract='Seven mirrored randomized ABBA/BAAB blocks with equal calibrated iterations per pair, targeting at least 20ms GPU work per round (cap 512). Two warmups/three measured rounds. Each iteration constructs packed B anew, then executes R=1 or R=8 matmuls with shared immutable B and alternating activations; both additionally packs A for every call. Buffers are preallocated private inputs/outputs/workspace. GPU and host intervals include pack dispatches and encoding/commit/wait; allocation/uploads/loading are excluded. All R distinct outputs from the last identical iteration, packed contents/padding/guards and unchanged inputs are independently checked outside timing every round. B packing is never amortized across iterations/rounds; only across the specified R calls. Not complete managed language throughput or full numerical/safety qualification.'),[Path(__file__),driver,a.screen],[binary,library],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
