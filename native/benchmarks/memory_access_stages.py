"""Diagnostic pack/matrix encoder timestamps with an unsampled control."""
import argparse,json,os,random,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--screen',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args();assert not a.output.exists()
    assert all(os.getenv(k) in (None,'','0') for k in ['MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'])
    screen=json.loads(a.screen.read_text());art=screen['artifact_sha256'];lib=next(p.ROOT/k for k in art if k.endswith('.metallib'));control=next(p.ROOT/k for k in art if not k.endswith('.metallib'));assert all(p.sha(p.ROOT/k)==v for k,v in art.items())
    build=p.ROOT/'benchmarks/.build/memory-access-2026-10-26'/a.output.stem;build.mkdir(parents=True,exist_ok=True);driver=Path(__file__).with_name('MemoryAccessStages.mm');binary=build/'stages'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-ffp-contract=off','-fobjc-arc','-fblocks','-DKALANDRA_PACK_TILED',driver,'-framework','Foundation','-framework','Metal','-o',binary])
    shapes=[(1024,1024,1024),(2048,2048,2048),(1024,4096,1024),(1025,576,1027)];records=[];rng=random.Random(20261026)
    for trial in range(2):
        cells=[(v,s,sampled) for v in ['baseline','block','both','order_m16'] for s in shapes for sampled in [False,True]];rng.shuffle(cells)
        for v,s,sampled in cells:
            rows=list(map(json.loads,p.run([binary if sampled else control,lib,v,*s,1,3,1]).splitlines()));assert [r['round'] for r in rows]==list(range(-2,3)) and all(r['cells']==s[0]*s[2] for r in rows)
            if sampled:assert all(r['matrix_ticks']>0 and r['pack_ticks']>=0 for r in rows)
            records.append(dict(trial=trial,variant=v,shape=s,sampled=sampled,samples=rows));print(trial,v,s,sampled,flush=True)
    p.evidence(a.output,dict(records=records,driver_source=driver.read_text(),runner_source=Path(__file__).read_text(),contract='Untimed-diagnostic acceptance: public encoder-boundary timestamps, raw device ticks. Sampled/unsampled processes randomized separately; two processes/two warmups/three rounds per cell. Pack fraction divides packing ticks by pack+matrix ticks, not whole-command latency. Same preallocated diagnostic and full-cell/guard/input checks. Sampling may perturb execution; no cache/occupancy counters or assumed tick-to-ns conversion.'),[Path(__file__),driver,a.screen],[binary,control,lib],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
