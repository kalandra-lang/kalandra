"""Long-batch replication and guard cases for the promising private-output probe.

Three one-second-target blocks compare actual accepted production against the
same frozen probe: largest resident replication, smaller resident guards, and
largest all-weight-upload guard. No production code or executable is changed.
"""
import runpy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
measure,evidence=p.measure_paired,p.evidence
assert sys.argv[sys.argv.index('--phase')+1]=='measure'
assert sys.argv[sys.argv.index('--study')+1]=='repeat'
groups={(1,512,0),(16,512,0),(64,256,0),(64,512,0),(64,512,2)}
def measured(cells,invoke,reference,**kwargs):
 kwargs.update(blocks=3,target_ms=1000,max_iterations=8192)
 return measure([c for c in cells if tuple(c[1:]) in groups],invoke,reference,**kwargs)
def recorded(output,data,sources,binaries,compiler):
 data['broad_replication']=dict(reason='Focused production result +10.86% median with one negative block; replicate and check smaller resident/all-weight-upload guards before any promotion.',groups=sorted(groups))
 return evidence(output,data,sources+[Path(__file__).resolve()],binaries,compiler)
p.measure_paired,p.evidence=measured,recorded
runpy.run_path(str(Path(__file__).with_name('private_output_probe.py')),run_name='__main__')
