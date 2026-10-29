"""Long-batch acceptance of atomic live+idle accounting after the locked candidate.

Use the frozen upload_cache_study.py measure arguments. The repeat covers all
12 original cases; the identical-build control covers the five earlier unstable
cases. All binaries, numerical qualification and timed code remain frozen.
"""
import runpy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
measure,evidence=p.measure_paired,p.evidence
controls={(1,512,1),(16,512,0),(64,512,1),(64,256,0),(64,256,1)}
study=sys.argv[sys.argv.index('--study')+1]
assert study in ('aa','repeat') and sys.argv[sys.argv.index('--phase')+1]=='measure'

def measured(cells,invoke,reference,**kwargs):
 kwargs.update(blocks=3,target_ms=1000,max_iterations=8192)
 if study=='aa':cells=[c for c in cells if tuple(c[1:]) in controls]
 return measure(cells,invoke,reference,**kwargs)

def recorded(output,data,sources,binaries,compiler):
 data['atomic_followup']=dict(reason='The locked cache regressed resident 64x256 in longer batches. Ordinary allocations now use atomic total reservations; cache mutation/eviction alone locks.',control_groups=sorted(controls))
 return evidence(output,data,sources+[Path(__file__).resolve()],binaries,compiler)

p.measure_paired,p.evidence=measured,recorded
runpy.run_path(str(Path(__file__).with_name('upload_cache_study.py')),run_name='__main__')
