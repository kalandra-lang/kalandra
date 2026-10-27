"""Shared repeated-process protocol; all measured and warmup outputs are validated."""
import datetime,hashlib,json,math,os,platform,random,statistics,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
CLANG=Path(json.loads((ROOT/'native/toolchain-proof/toolchain.json').read_text())['defaultLlvmRoot'])/'bin/clang++'
RUST=Path.home()/'.rustup/toolchains/stable-aarch64-apple-darwin/bin/rustc'
BUILD=ROOT/'benchmarks/.build/fair'
COMMANDS=[]
def run(argv,env=None,timeout=360):
    argv=list(map(str,argv)); COMMANDS.append(argv)
    p=subprocess.run(argv,cwd=ROOT,env=env,text=True,capture_output=True,timeout=timeout)
    if p.returncode:raise RuntimeError(f'{argv}: {p.returncode}\n{p.stdout}\n{p.stderr}')
    return p.stdout
def key(path):
    path=Path(path)
    return str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def observations(output,expected,count,rounds):
    rows=[json.loads(x) for x in output.splitlines()]
    assert [r['round'] for r in rows]==list(range(-2,rounds)),rows
    for row in rows:
        assert math.isfinite(row['ns']) and row['ns']>=0,row
        assert type(row['checksum']) in (int,float) and math.isfinite(row['checksum']) and float(row['checksum'])==expected*count,(row,expected,count)
    return rows

def calibrate_batch(cells,invoke,target_ms,max_iterations,adaptive):
    """Use validated warm batches, including the faster member of a pair."""
    assert target_ms>0 and max_iterations>0
    count=1;history=[]
    for _ in range(12 if adaptive else 1):
        costs=[]
        for cell in cells:
            samples=invoke(cell,count,3)
            elapsed=statistics.median(r['ns'] for r in samples if r['round']>=0)
            assert math.isfinite(elapsed) and elapsed>=0
            costs.append(elapsed)
            history.append(dict(cell=cell,iterations=count,samples=samples))
        elapsed=min(costs) if adaptive else max(costs)
        if not adaptive:
            return max(1,min(max_iterations,math.ceil(target_ms*1e6/max(1,elapsed)))),history
        if elapsed>=target_ms*1e6*.8 or count==max_iterations:
            assert elapsed>0, 'batch remains below timer resolution at cap'
            return count,history
        count=min(max_iterations,max(count+1,math.ceil(count*min(32,target_ms*1e6/max(1,elapsed)))))
    raise AssertionError('batch calibration did not converge')

def measure(cells,invoke,reference,processes=3,rounds=7,target_ms=50,adaptive=False,max_iterations=20000):
    counts={};calibration=[]
    for cell in cells:
        counts[tuple(cell)],probes=calibrate_batch([cell],invoke,target_ms,max_iterations,adaptive)
        calibration.extend(probes)
    records=[];rng=random.Random(20261024)
    for trial in range(processes):
        order=list(cells);rng.shuffle(order)
        for cell in order:
            count=counts[tuple(cell)];samples=invoke(cell,count,rounds)
            assert all(s['ns']>0 for s in samples if s['round']>=0),'batch below timer resolution'
            row=dict(cell=cell,process=trial,iterations=count,samples=samples)
            records.append(row)
            median=statistics.median(r['ns']/count for r in samples if r['round']>=0)
            print(f'{trial+1}/{processes} {cell}: {median/1000:.3f} us',flush=True)
    summary=[]
    for cell in cells:
        groups=[r for r in records if r['cell']==cell]
        medians=[statistics.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0) for r in groups]
        summary.append(dict(cell=cell,expected=reference(cell),median_ns=statistics.median(medians),process_medians_ns=medians))
    return dict(protocol=dict(processes=processes,rounds=rounds,warmups=2,target_ms=target_ms,max_iterations=max_iterations,adaptive_batching=adaptive,seed=20261024),calibration=calibration,observations=records,summary=summary)

def measure_paired(cells,invoke,reference,blocks=9,rounds=3,target_ms=10,adaptive=False,max_iterations=20000):
    """Adjacent ABBA processes reduce drift; retain every sample and paired ratio."""
    groups={}
    for cell in cells:
        assert cell[0] in ('baseline','current')
        groups.setdefault(tuple(cell[1:]),{})[cell[0]]=cell
    assert all(set(g)=={'baseline','current'} for g in groups.values())
    counts={};calibration=[]
    for key,group in groups.items():
        counts[key],probes=calibrate_batch(list(group.values()),invoke,target_ms,max_iterations,adaptive)
        calibration.extend(probes)
    records=[];pairs=[];rng=random.Random(20261024)
    for block in range(blocks):
        order=list(groups);rng.shuffle(order)
        for key in order:
            group=groups[key];count=counts[key];medians={'baseline':[],'current':[]}
            versions=('baseline','current','current','baseline')
            if block%2:versions=('current','baseline','baseline','current')
            for slot,version in enumerate(versions):
                cell=group[version];samples=invoke(cell,count,rounds)
                assert all(s['ns']>0 for s in samples if s['round']>=0)
                median=statistics.median(s['ns']/count for s in samples if s['round']>=0)
                medians[version].append(median)
                records.append(dict(cell=cell,process=block,slot=slot,iterations=count,samples=samples))
            before=statistics.mean(medians['baseline']);after=statistics.mean(medians['current'])
            pairs.append(dict(group=list(key),block=block,baseline_ns=before,current_ns=after,
                              speedup=before/after,time_reduction=1-after/before))
            print(f'{block+1}/{blocks} {key}: {before/1000:.3f} -> {after/1000:.3f} us',flush=True)
    summary=[]
    for cell in cells:
        medians=[statistics.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0)
                 for r in records if r['cell']==cell]
        summary.append(dict(cell=cell,expected=reference(cell),median_ns=statistics.median(medians),
                            process_medians_ns=medians))
    return dict(protocol=dict(blocks=blocks,rounds=rounds,warmups=2,target_ms=target_ms,
        max_iterations=max_iterations,adaptive_batching=adaptive,seed=20261024,ordering='Adjacent ABBA/BAAB fresh processes; identical iterations per pair'),
        calibration=calibration,observations=records,summary=summary,pairs=pairs)

def evidence(output,data,sources,binaries,compiler):
    assert not output.exists() and not output.with_suffix('.md').exists(),'choose new output paths'
    data.update(schema=1,timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                compiler_sha256=sha(compiler),source_sha256={key(p):sha(p) for p in sources},
                artifact_sha256={key(p):sha(p) for p in binaries},commands=COMMANDS,
                revision=run(['git','rev-parse','HEAD']).strip(),worktree_status=run(['git','status','--short']),
                system=platform.platform(),versions=dict(python=sys.version,clang=run([CLANG,'--version']),rust=run([RUST,'--version','--verbose']),go=run(['go','version'])),
                cpu=run(['sysctl','-n','machdep.cpu.brand_string']).strip())
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(data,indent=2)+'\n')
