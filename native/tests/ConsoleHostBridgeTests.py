"""Check the supervisor's native console packet ABI using compiled LLVM and real OS I/O."""
import argparse
import os
from pathlib import Path
import subprocess
import threading
p=argparse.ArgumentParser(description=__doc__)
for name in ('generator','clang','runtime','output'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'),required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
def run(command,**kw):
    result=subprocess.run(list(map(str,command)),capture_output=True,timeout=180,**kw)
    assert result.returncode==0,(command,result)
    return result
run([a.generator,a.output,a.variant])
flags={'debug':['-O0'],'optimized':['-O2'],'sanitized':['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'thread-sanitized':['-O1','-fsanitize=thread']}[a.variant]
exe=a.output/('driver.exe' if os.name=='nt' else 'driver')
result=run([a.clang,'-std=c++23','-Wno-override-module',*flags,Path(__file__).with_name('ConsoleHostBridgeDriver.cpp'),a.output/'bridge.ll',a.runtime,'-o',exe])
(a.output/'build.log').write_bytes(result.stdout+result.stderr)
assert run([exe,'write']).stdout=='α\0é\n'.encode()
for mode,data in [('text','α\0é\r\n'.encode()),('end',b''),('limit',b'long\n'),('encoding',b'\xff\n')]:
    result=run([exe,mode],input=data)
    assert result.stdout.replace(b'\r\n',b'\n')==b'PASS\n' and not result.stderr,result
child=subprocess.Popen([exe,'cancel'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
timer=threading.Timer(10,child.kill);timer.start()
try:
    assert child.wait(timeout=12)==0
    out,err=child.communicate(timeout=2)
    assert out.replace(b'\r\n',b'\n')==b'PASS\n' and not err,(out,err)
finally:
    timer.cancel()
    if child.poll() is None:child.kill();child.communicate()
print('PASS: real supervisor console ABI, Unicode/NUL, EOF, inactive fields, typed errors and stopped publication')

read_fd,write_fd=os.pipe();os.close(read_fd)
child=subprocess.Popen([exe,'broken'],stdout=write_fd,stderr=subprocess.PIPE);os.close(write_fd)
assert child.wait(timeout=10)==0
assert child.stderr.read().replace(b'\r\n',b'\n')==b'PASS\n'
print('PASS: real broken-pipe system code and owned diagnostic String packet')
