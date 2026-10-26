"""Independently recompute exact modular oracles and paired benchmark statistics."""
import argparse, hashlib, json, math, statistics
from pathlib import Path

def oracle(kind,seed,rounds):
    modulus=1000003
    power=pow(13,rounds,modulus)
    geometric=(power-1)*pow(12,-1,modulus)%modulus
    count=8 if kind==0 else 1
    offset=7 if kind==0 else seed+7
    return sum((power*(seed+i)+offset*geometric)%modulus for i in range(count))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--paired',type=Path,nargs='+',required=True)
    parser.add_argument('--workloads',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--verify-artifacts',action='store_true')
    args=parser.parse_args()
    assert not args.output.exists()
    digest=lambda path:hashlib.sha256(Path(path).read_bytes()).hexdigest()
    reports=[]
    for path in args.paired:
        data=json.loads(path.read_text())
        for name,source in data['source_snapshots'].items():
            assert hashlib.sha256(source.encode()).hexdigest()==data['source_sha256'][name]
        if args.verify_artifacts:
            for name,sha in data['artifact_sha256'].items(): assert digest(name)==sha,name
        processes=0;groups=[]
        for part in ('source','task_scope'):
            record=data[part];blocks=record['protocol']['blocks']
            rounds=record['protocol']['rounds']
            processes+=len(record['observations'])
            for row in record['observations']:
                _,_,kind,size=row['cell'];count=row['iterations']
                assert count>0 and [s['round'] for s in row['samples']]==list(range(-2,rounds))
                for sample in row['samples']:
                    assert sample['checksum']==count*oracle(kind,17,size)
                    assert math.isfinite(sample['ns']) and sample['ns']>0
            for row in record['summary']:
                rows=[r for r in record['observations'] if r['cell']==row['cell']]
                medians=[statistics.median(s['ns']/r['iterations'] for s in r['samples']
                                          if s['round']>=0) for r in rows]
                assert len(rows)==blocks*2 and row['median_ns']==statistics.median(medians)
                assert row['expected']==oracle(row['cell'][2],17,row['cell'][3])
            for pair in record['pairs']:
                rows=sorted((r for r in record['observations'] if r['cell'][1:]==pair['group']
                             and r['process']==pair['block']),key=lambda r:r['slot'])
                assert len(rows)==4 and len({r['iterations'] for r in rows})==1
                versions=['baseline','current','current','baseline']
                if pair['block']%2: versions=['current','baseline','baseline','current']
                assert [r['cell'][0] for r in rows]==versions
                times={version:statistics.mean(statistics.median(s['ns']/r['iterations']
                       for s in r['samples'] if s['round']>=0) for r in rows if r['cell'][0]==version)
                       for version in ('baseline','current')}
                assert pair['baseline_ns']==times['baseline'] and pair['current_ns']==times['current']
                assert pair['speedup']==times['baseline']/times['current']
                assert pair['time_reduction']==1-times['current']/times['baseline']
            for group in sorted({tuple(p['group']) for p in record['pairs']}):
                pairs=[p for p in record['pairs'] if tuple(p['group'])==group]
                assert len(pairs)==blocks
                values=[p['time_reduction'] for p in pairs]
                groups.append(dict(section=part,group=group,median_time_reduction=statistics.median(values),
                                   min_time_reduction=min(values),max_time_reduction=max(values)))
        reports.append(dict(path=str(path),sha256=digest(path),measured_processes=processes,
                            groups=groups,artifact_files_verified=args.verify_artifacts,verified=True))
    data=json.loads(args.workloads.read_text())
    for row in data['verification']:
        expected=oracle(row['cell'][1],row['seed'],row['cell'][2])
        assert all(s['checksum']==expected for s in row['samples'])
    for record in (data,data['task_scope_diagnostic']):
        for row in record['observations']:
            expected=oracle(row['cell'][1],17,row['cell'][2])
            assert all(s['checksum']==expected*row['iterations'] for s in row['samples'])
            assert all(math.isfinite(s['ns']) and s['ns']>0 for s in row['samples'])
        for row in record['summary']:
            records=[r for r in record['observations'] if r['cell']==row['cell']]
            medians=[statistics.median(s['ns']/r['iterations'] for s in r['samples']
                                      if s['round']>=0) for r in records]
            assert len(records)==3 and row['median_ns']==statistics.median(medians)
    reports.append(dict(path=str(args.workloads),sha256=digest(args.workloads),
                        verification_workloads=len(data['verification']),verified=True))
    args.output.write_text(json.dumps(reports,indent=2)+'\n')
    print(json.dumps(reports,indent=2))

if __name__=='__main__': main()
