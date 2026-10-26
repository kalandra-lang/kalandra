"""Sample only this runner's own CPU benchmark children, outside acceptance timing."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import time
import protocol as p
from runtime_workloads import oracle

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--build-dir', type=Path)
args = parser.parse_args()
assert not args.output.exists()
build = args.build_dir.resolve() if args.build_dir else p.BUILD/'runtime-workloads'
records = []
for name,kind,size,count in [('arrays',0,256,1000000), ('closure',1,0,60000000),
                             ('scheduled-256',1,256,50000)]:
    binary = build/name
    sample_path = args.output.with_suffix('.'+name+'.sample.txt')
    assert not sample_path.exists()
    command = [str(binary),str(kind),'17',str(size),str(count),'1',str(oracle(kind,17,size))]
    digest = p.sha(binary)
    child = subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                             env=dict(os.environ,KALANDRA_WORKERS='1'))
    try:
        time.sleep(0.1)
        sampler = ['/usr/bin/sample',str(child.pid),'2','-file',str(sample_path)]
        sampled = subprocess.run(sampler,capture_output=True,text=True,timeout=30)
        assert sampled.returncode == 0, sampled.stderr
        stdout,stderr = child.communicate(timeout=60)
        assert child.returncode == 0 and not stderr, (child.returncode,stderr)
        samples = p.observations(stdout,oracle(kind,17,size),count,1)
        assert p.sha(binary) == digest
        records.append(dict(name=name,command=command,sampler=sampler,pid=child.pid,
            binary_sha256=digest,sample_path=str(sample_path),sample_sha256=p.sha(sample_path),
            samples=samples,stdout=sampled.stdout,stderr=sampled.stderr))
        print(name+' sampled and exact output verified',flush=True)
    finally:
        if child.poll() is None:
            child.terminate()
            child.communicate(timeout=10)
args.output.write_text(json.dumps(dict(timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    contract='Instrumented macOS sample diagnostics of owned child processes, interval default 1ms, duration 2s. Includes warmup or measured batches; these times are not acceptance results. Task diagnostic explicitly uses one host-driven worker. Sampling counts are observations, not exact exclusive time percentages.',
    sources={p.key(Path(__file__)):p.sha(__file__)},records=records),indent=2)+'\n')
