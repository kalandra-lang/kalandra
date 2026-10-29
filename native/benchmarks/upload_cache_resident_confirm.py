"""Confirm the sole >5% median slowdown in the atomic twelve-case acceptance.

Seven long-batch paired blocks of resident 16x512 only, identical frozen binaries
and hot loop. Run --phase measure --study aa or repeat. All prior data remain.
"""
import runpy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
measure,evidence=p.measure_paired,p.evidence
assert sys.argv[sys.argv.index('--study')+1] in ('aa','repeat')
assert sys.argv[sys.argv.index('--phase')+1]=='measure'

def measured(cells,invoke,reference,**kwargs):
 kwargs.update(blocks=7,target_ms=1000,max_iterations=8192)
 return measure([c for c in cells if tuple(c[1:])==(16,512,0)],invoke,reference,**kwargs)

def recorded(output,data,sources,binaries,compiler):
 data['atomic_followup']=True
 data['resident_confirmation']=dict(trigger='Only the resident 16x512 case exceeded 5% median slowdown in the twelve-case atomic acceptance; independently confirm with seven paired blocks and identical-build control.',group=[16,512,0])
 return evidence(output,data,sources+[Path(__file__).resolve()],binaries,compiler)

p.measure_paired,p.evidence=measured,recorded
runpy.run_path(str(Path(__file__).with_name('upload_cache_study.py')),run_name='__main__')
