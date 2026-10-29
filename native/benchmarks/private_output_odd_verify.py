"""Qualify the frozen private-output probe on odd/small transformer shapes."""
import runpy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
assert sys.argv[sys.argv.index('--phase')+1]=='verify'
evidence=p.evidence
p.evidence=lambda output,data,sources,binaries,compiler:evidence(output,dict(data,odd_shape_qualification=True),sources+[Path(__file__).resolve()],binaries,compiler)
source=Path(__file__).with_name('private_output_probe.py');body=source.read_text()
old='shapes=[(1,512),(16,512),(64,512),(64,256)]';assert body.count(old)==1
body=body.replace(old,'shapes=[(3,33),(7,65),(1,17),(31,31)]')
exec(compile(body,str(source),'exec'),dict(__file__=str(source),__name__='__main__'))
