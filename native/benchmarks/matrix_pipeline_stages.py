"""Diagnostic matrix/reduction stage fractions with Metal encoder-boundary timestamps."""
import argparse,json,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import pipeline_reference as ref

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--bundle',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args();assert not a.output.exists()
    for key in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','KALANDRA_GPU_PROFILE','DYLD_INSERT_LIBRARIES'):assert os.getenv(key) in (None,'','0'),key
    build=p.ROOT/'benchmarks/.build/microkernel-2026-10-25'/a.output.stem;build.mkdir(parents=True,exist_ok=True)
    source=Path(__file__).with_name('PipelineStages.mm');binary=build/'stages'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-fobjc-arc','-fblocks',source,'-framework','Foundation','-framework','Metal','-o',binary])
    records=[];rng=random.Random(20261025)
    for trial in range(3):
        order=[512,1024,2048];rng.shuffle(order)
        for n in order:
            rows=list(map(json.loads,p.run([binary,a.bundle,n,7]).splitlines()));metadata,*samples=rows
            assert metadata['stage_sampling'] and all(r['result']==ref.exact(n) for r in samples)
            assert len(samples)==18
            for r in samples:
                assert r['command_ns']>0
                if r['sampled']:assert r['matrix_ticks']>0 and r['reduction_ticks']>0 and r['span_ticks']>=r['matrix_ticks']+r['reduction_ticks']
            records.append(dict(trial=trial,n=n,metadata=metadata,samples=samples))
            print(trial,n,'matrix fraction',statistics.median(r['matrix_ticks']/(r['matrix_ticks']+r['reduction_ticks']) for r in samples if r['sampled'] and r['round']>=0),flush=True)
    p.evidence(a.output,dict(records=records,driver_source=source.read_text(),contract='Preallocated shared inputs/private outputs. Same fused matrix and serial multi-dispatch reduction entries, one command with two encoders and scalar readback. Encoder-boundary timestamp sampling vs an uninstrumented control, two warmups/seven rounds, three randomized processes. Sampling can perturb execution; raw counter tick fractions require no clock-unit assumption. Full output and suffix checks plus an independent scalar oracle every round. Not managed language throughput and not instruction/occupancy/stall profiling. The device exposes only the public timestamp counter set, with stage-boundary but not dispatch-boundary sampling.'),[Path(__file__),source,*a.bundle.glob('*.metal')],[binary,*a.bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
