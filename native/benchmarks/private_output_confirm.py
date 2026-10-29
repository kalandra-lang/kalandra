"""Focused post-interruption control for resident64x512 private-output reuse.

Five adjacent one-second-target paired blocks; original binaries/hot loop.
This is diagnostic feasibility, not production acceptance. Earlier noisy AA
and the interrupted short screen remain retained.
"""
import runpy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
measure,evidence=p.measure_paired,p.evidence
assert sys.argv[sys.argv.index('--phase')+1]=='measure'
assert sys.argv[sys.argv.index('--study')+1] in ('aa','production','repeat')
def measured(cells,invoke,reference,**kwargs):
 kwargs.update(blocks=5,target_ms=1000,max_iterations=8192)
 return measure([c for c in cells if tuple(c[1:])==(64,512,0)],invoke,reference,**kwargs)
def recorded(output,data,sources,binaries,compiler):
 data['focused_confirmation']=dict(reason='Initial identical-build control spans up to44.6%; enabled short screen interrupted by desktop crash. Confirm largest resident case using fresh production/control cohorts.',group=[64,512,0])
 return evidence(output,data,sources+[Path(__file__).resolve()],binaries,compiler)
p.measure_paired,p.evidence=measured,recorded
runpy.run_path(str(Path(__file__).with_name('private_output_probe.py')),run_name='__main__')
