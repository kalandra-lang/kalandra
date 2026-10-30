"""Same-runtime batch512 control: test longer useful GPU work per invocation.

Frozen runtime and shaders unchanged. Additional full-output/oracle qualification
precedes timing. This remains instrumented diagnostic data, not production
acceptance; all earlier small-batch controls remain retained.
"""
import json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
assert sys.argv[sys.argv.index('--phase')+1] in ('verify','measure')
base=Path(__file__).with_name('private_output_inprocess.py');body=base.read_text()
old='for m,d in [(1,512),(16,512),(64,512),(64,256),(3,33),(7,65),(1,17),(31,31)]:';assert body.count(old)==1
body=body.replace(old,'for m,d in [(512,512)]:').replace('groups=[(64,512,0)] if', 'groups=[(512,512,0)] if')
evidence=p.evidence
def recorded(output,data,sources,binaries,compiler):
 data['large_batch_probe']=dict(shape=[512,512],reason='GPU command-duration variation tracks reported DVFS states; test longer useful GPU work per invocation with unchanged code/shaders.')
 if '--phase' in sys.argv and sys.argv[sys.argv.index('--phase')+1]=='measure':
  b=Path(sys.argv[sys.argv.index('--build')+1]);q=b/'large-qualification.json';d=json.loads(q.read_text());assert len(d['checks'])==18 and all(x['passed'] for x in d['checks'])
  data['large_qualification_sha256']=p.sha(q)
 return evidence(output,data,sources+[Path(__file__).resolve()],binaries,compiler)
p.evidence=recorded
exec(compile(body,str(base),'exec'),dict(__file__=str(base),__name__='__main__'))
