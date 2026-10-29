"""Repeat the frozen upload-cache protocol with longer batches after noisy controls.

Pass the same arguments as upload_cache_study.py, using --phase measure and
--study repeat or aa. Only five predeclared troublesome cases are measured.
The frozen executables, driver loop, qualification and identity checks are reused.
"""
import runpy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
selected={(1,512,1),(16,512,0),(64,512,1),(64,256,0),(64,256,1)}
measure,evidence=p.measure_paired,p.evidence

def settled(cells,invoke,reference,**kwargs):
 kwargs.update(blocks=3,target_ms=1000,max_iterations=8192)
 return measure([c for c in cells if tuple(c[1:]) in selected],invoke,reference,**kwargs)

def record(output,data,sources,binaries,compiler):
 assert data['study'] in ('repeat','aa')
 data['settlement']=dict(reason='Initial same-build controls and replication have large swings; longer batches test five predeclared regression/noise cases without discarding the original series.',groups=sorted(selected))
 return evidence(output,data,sources+[Path(__file__).resolve()],binaries,compiler)

assert '--phase' in sys.argv and sys.argv[sys.argv.index('--phase')+1]=='measure'
p.measure_paired,p.evidence=settled,record
runpy.run_path(str(Path(__file__).with_name('upload_cache_study.py')),run_name='__main__')
