"""Adjacent ABBA/BAAB comparisons of frozen broader ordinary CPU artifacts."""
import argparse,json
from pathlib import Path
import protocol as p
from broad_ordinary import PROGRAMS,oracle,observations

def main():
 q=argparse.ArgumentParser();q.add_argument('--baseline',type=Path,required=True);q.add_argument('--current',type=Path,required=True);q.add_argument('--output',type=Path,required=True);a=q.parse_args();assert not a.output.exists()
 folders={'baseline':a.baseline.resolve(),'current':a.current.resolve()};manifests={};files=[]
 for version,folder in folders.items():
  m=json.loads((folder/'manifest.json').read_text());manifests[version]=m
  for path,h in m['hashes'].items():assert p.sha(path)==h;files.append(Path(path))
  for key in ('compiler','runtime'):
   f=Path(m[key]);assert p.sha(f)==m['signature'][p.key(f)];files.append(f)
 for path,h in manifests['baseline']['signature'].items():
  if path not in (p.key(Path(manifests['baseline']['compiler'])),p.key(Path(manifests['baseline']['runtime']))):assert manifests['current']['signature'][path]==h,path
 def invoke(cell,count,rounds):
  version,name,n=cell;expected=oracle(name,17,n)
  return observations(p.run([folders[version]/name/'kalandra',17,n,count,rounds,repr(expected)]),expected,count,rounds)
 cells=[[version,name,n] for name in PROGRAMS for n in (0,1,16,256) for version in folders]
 data=p.measure_paired(cells,invoke,lambda c:oracle(c[1],17,c[2]),blocks=9,target_ms=10,adaptive=True,max_iterations=20000000)
 data.update(manifests=manifests,contract='Ordinary execution, identical per-program source algorithms and driver. Nine adjacent ABBA/BAAB fresh-process blocks; identical iteration count within a pair. Exact return-bit and integer-checksum validation. No build, test or profiler overlap. No frequency/affinity isolation.')
 sources=[Path(__file__).resolve(),Path(__file__).with_name('broad_ordinary.py'),Path(__file__).with_name('protocol.py')]
 data['source_snapshots']={p.key(f):f.read_text() for f in sources}
 p.evidence(a.output,data,sources,files,Path(manifests['current']['compiler']))
if __name__=='__main__':main()
