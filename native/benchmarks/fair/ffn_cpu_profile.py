"""Sample only owned FFN benchmark children, separately from acceptance timing."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import protocol as p

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
assert not args.output.exists()
data=json.loads(args.evidence.read_text())
binary=p.ROOT/'benchmarks/.build/residual-ffn/cpu'
digest=p.sha(binary)
assert data['artifact_sha256'][p.key(binary)]==digest
records=[]
for m,d,count in [(1,256,20000),(1,1024,500),(128,1024,60)]:
    expected=next(r['expected'] for r in data['verification'] if (r['backend'],r['m'],r['d'],r['pattern'])==('cpu',m,d,0))
    command=[str(binary),str(m),str(d),str(count),'1','1','0','0']
    path=args.output.with_suffix(f'.{m}x{d}.sample.txt')
    assert not path.exists()
    child=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=os.environ.copy())
    try:
        time.sleep(.2)
        sampler=['/usr/bin/sample',str(child.pid),'2','-file',str(path)]
        sample=subprocess.run(sampler,text=True,capture_output=True,timeout=30)
        assert sample.returncode==0,sample.stderr
        out,err=child.communicate(timeout=90)
        assert child.returncode==0 and not err,(child.returncode,err)
        rows=p.observations(out,expected,count,1)
        records.append(dict(m=m,d=d,count=count,command=command,sampler=sampler,owned_pid=child.pid,
                            samples=rows,sample_sha256=p.sha(path),sample_file=p.key(path),sampler_stdout=sample.stdout))
        print(f'{m}x{d} sampled and outputs verified',flush=True)
    finally:
        if child.poll() is None:child.terminate();child.communicate(timeout=10)
assert p.sha(binary)==digest
args.output.write_text(json.dumps(dict(contract='Two-second 1ms macOS samples of owned CPU FFN benchmark children. Instrumented diagnostics, not acceptance timings or exact exclusive-time percentages.',binary_sha256=digest,script_sha256=p.sha(__file__),records=records),indent=2)+'\n')
