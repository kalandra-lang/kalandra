"""Check Vulkan transforms with the shared independent Decimal/bitwise oracles.

The initial profile is the existing Metal qualification oracle, unchanged.
Failures retain the exact request/input and observed bounds for investigation.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import struct
import subprocess
import sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--driver',type=Path,required=True)
p.add_argument('--shaders',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--device',type=int,default=0)
p.add_argument('--profile',choices=('metal-exact','vulkan-hardware'),default='metal-exact')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(repo/'native/tests'))
from TensorTransformReference import D, to_bits, from_bits, reference
if a.profile=='vulkan-hardware':
    from TransformReference import reference
stderr=(a.output/'driver.log').open('w+')
server=subprocess.Popen([a.driver,a.shaders,str(a.device)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr,text=True)
reader=concurrent.futures.ThreadPoolExecutor(max_workers=1)
checks=0;cases=0

def invoke(name,fmt,wire,width,operation,values,m,n,epsilon,device_epsilon=0):
    global checks,cases
    file=(a.output/'input.bin').resolve()
    file.write_bytes(struct.pack('<'+wire*len(values),*values))
    request=f'{name} {operation} {m} {n} {epsilon} {device_epsilon} '+json.dumps(str(file))
    server.stdin.write(request+'\n');server.stdin.flush()
    line=reader.submit(server.stdout.readline).result(timeout=120)
    if not line:
        code=server.wait(timeout=30)
        stderr.flush();raise AssertionError((request,code,(a.output/'driver.log').read_text()[-4000:]))
    actual=json.loads(line)
    assert actual['status']==0,(request,actual)
    expected=reference(values,m,n,operation,D.from_float(from_bits(epsilon,fmt,wire)) if fmt else None,fmt,wire,True)
    assert len(actual['bits'])==len(expected),(request,actual)
    for bits,wanted in zip(actual['bits'],expected):
        if operation==0:valid=bits==wanted
        else:
            value=D.from_float(from_bits(bits,fmt,wire));lo,hi=wanted
            valid=(value.is_nan() and lo.is_nan()) or (not value.is_nan() and lo<=value<=hi)
        if not valid:
            failure={'request':request,'actual':bits,'expected':str(wanted),'case':cases,'element':checks}
            (a.output/'failure.json').write_text(json.dumps(failure,indent=2)+'\n')
            raise AssertionError(failure)
        checks+=1
    cases+=1

try:
    types=[('float32','f','I',32),('float64','d','Q',64)]+[(('u' if unsigned else '')+'int'+str(bits),'','B' if bits==8 else 'H' if bits==16 else 'I' if bits==32 else 'Q',bits) for bits in (8,16,32,64) for unsigned in (False,True)]
    for name,fmt,wire,width in types:
        for operation in ((0,1,2) if name=='float32' else (0,)):
            epsilon=to_bits(0.00001,fmt,wire) if fmt else 0
            for m,n in ((0,3),(3,0),(1,1),(2,3),(3,7),(2,31),(2,32),(2,33),(2,255),(2,256),(2,257),(1,1025)):
                values=[to_bits(D(((i*37+11)%101)-50)/13,fmt,wire) for i in range(m*n)] if fmt else [((i*0x9e3779b97f4a7c15)^0xa55a0ff00f5aa5)&((1<<width)-1) for i in range(m*n)]
                for device_epsilon in ((0,1) if operation==2 else (0,)):
                    invoke(name,fmt,wire,width,operation,values,m,n,epsilon,device_epsilon)
            if name=='float32' and operation in (1,2) and a.profile=='vulkan-hardware':
                for power in (-30,-10,0,10,30):
                    values=[to_bits(D(((i*73+19)%211)-105)/17*(D(2)**power),fmt,wire) for i in range(3*33)]
                    invoke(name,fmt,wire,width,operation,values,3,33,epsilon)
            if fmt:
                exponent=0x7f800000 if width==32 else 0x7ff0000000000000
                fraction=23 if width==32 else 52
                specials=[0,1,1<<(width-1),exponent,exponent|(1<<(width-1)),exponent|1,exponent|12345,exponent|(1<<(fraction-1)),(1<<(width-1))|1,exponent-1,(exponent-1)|(1<<(width-1)),1<<fraction]
                for values in (specials,[exponent]*7,[exponent|(1<<(width-1))]*7,[0]*7,[exponent,0,exponent,0,0,0,0],[exponent-1,(exponent-1)|(1<<(width-1)),0,1,1<<fraction]):
                    invoke(name,fmt,wire,width,operation,values,1,len(values),epsilon)
            print('PASS:',name,operation,cases,'cases',checks,'cells',flush=True)
    (a.output/'qualification.json').write_text(json.dumps({'cases':cases,'cells':checks,'oracle':a.profile},indent=2)+'\n')
finally:
    server.stdin.close()
    try:
        status=server.wait(timeout=30)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=30)
        raise
    reader.shutdown(wait=True)
    stderr.close()
assert status==0,status
